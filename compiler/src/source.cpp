#include "source.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <windows.h>

std::vector<std::filesystem::path> collectFiles(const std::filesystem::path& root,
                                                const std::vector<std::string>& extensions, std::vector<FileStamp>* stamps)
{
  std::vector<std::pair<std::filesystem::path, FileStamp>> found;
  std::error_code ec;
  if (std::filesystem::is_regular_file(root))
  {
    FileStamp stamp;
    stamp.time = std::filesystem::last_write_time(root, ec).time_since_epoch().count();
    stamp.size = std::filesystem::file_size(root, ec);
    found.push_back({root, stamp});
  }
  else
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied))
    {
      if (!entry.is_regular_file())
        continue;
      std::string extension = entry.path().extension().string();
      if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
        continue;
      FileStamp stamp; // (on Windows the listing's own: the entry caches them)
      if (stamps)
      {
        stamp.time = entry.last_write_time(ec).time_since_epoch().count();
        stamp.size = entry.file_size(ec);
      }
      found.push_back({entry.path(), stamp});
    }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::filesystem::path> result;
  for (auto& [path, stamp] : found)
  {
    result.push_back(std::move(path));
    if (stamps)
      stamps->push_back(stamp);
  }
  return result;
}

namespace
{
  // A directory's kept listing and its change notifications (collectFilesWatched). A thread of its
  // own reads the notifications: a pending read is cancelled when the thread that issued it ends,
  // and a compile server's builds each run on a thread of their own (each second build's watch broke).
  struct Watch
  {
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    HANDLE armed = nullptr; // set once the first read is pending (or failed)
    std::string identity;  // the directory watched (a root renamed away and replaced is another): directoryIdentity's
    std::vector<DWORD> buffer = std::vector<DWORD>(16384); // 64 KB (DWORD-aligned, as the notifications need)
    // (the thread's and collectFilesWatched's, under watchMutex)
    std::vector<std::filesystem::path> files;
    std::vector<FileStamp> stamps;
    std::set<std::wstring> written; // the names (under the directory) written since the listing was brought up to date
    bool relist = false;            // a file or directory added, removed or renamed, or notifications lost
    bool broken = false;            // the thread ended: a read failed, or the watch was cancelled
    uint64_t generation = 0;        // reads the thread has recorded
  };
  std::mutex watchMutex;
  std::condition_variable watchRecorded; // a thread recorded a read, or ended
  std::map<std::string, Watch*> watches; // by directory and extensions (a broken one is closed and erased)

  bool arm(Watch& w)
  {
    const DWORD changes = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    return ReadDirectoryChangesW(w.directory, w.buffer.data(), DWORD(w.buffer.size() * sizeof(DWORD)), TRUE, changes, nullptr, &w.overlapped, nullptr);
  }

  // The watch's thread: a read pending at every moment, each one's notifications recorded (a written
  // file's name kept, anything else a relist; none at all, an overflow, a relist too). A read that
  // fails (the directory deleted, the watch cancelled) closes the directory at once, so that a
  // directory deleted while watched is gone, not delete-pending, and can be made again.
  void watchLoop(Watch* w)
  {
    bool ok = arm(*w);
    SetEvent(w->armed);
    for (;;)
    {
      DWORD bytes = 0;
      if (ok)
        ok = GetOverlappedResult(w->directory, &w->overlapped, &bytes, TRUE);
      std::lock_guard lock(watchMutex);
      if (ok)
      {
        if (bytes == 0)
          w->relist = true;
        else
          for (const char* at = reinterpret_cast<const char*>(w->buffer.data());;)
          {
            const auto* n = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(at);
            if (n->Action == FILE_ACTION_MODIFIED)
              w->written.insert(std::wstring(n->FileName, n->FileNameLength / sizeof(wchar_t)));
            else
              w->relist = true;
            if (!n->NextEntryOffset)
              break;
            at += n->NextEntryOffset;
          }
        // The next read pending before the lock goes: a build that finds the read completed waits
        // for this record, and one that finds it pending has what came before it.
        ResetEvent(w->overlapped.hEvent);
        ok = arm(*w);
      }
      if (!ok)
      {
        CloseHandle(w->directory);
        w->directory = INVALID_HANDLE_VALUE;
        w->broken = true;
        watchRecorded.notify_all();
        return;
      }
      ++w->generation;
      watchRecorded.notify_all();
    }
  }

  // Under watchMutex: the watch's thread stopped (its read cancelled) and the watch freed.
  void dropWatch(std::unique_lock<std::mutex>& lock, Watch* w)
  {
    if (w->directory != INVALID_HANDLE_VALUE)
      CancelIoEx(w->directory, &w->overlapped);
    watchRecorded.wait(lock, [&] { return w->broken; });
    if (w->overlapped.hEvent)
      CloseHandle(w->overlapped.hEvent);
    if (w->armed)
      CloseHandle(w->armed);
    delete w;
  }

  // An open directory's identity: its volume and its 128-bit file id (FileIdInfo; on ReFS the 64-bit
  // file index of GetFileInformationByHandle is not unique, thread A), else the volume and file index;
  // empty when neither can be read.
  std::string directoryIdentity(HANDLE h)
  {
    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id))
      return std::to_string(id.VolumeSerialNumber) + ":" + std::string(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof id.FileId.Identifier);
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info))
      return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
    return std::string();
  }

  // The identity of the directory a path names now (empty when it cannot be opened).
  std::string identityOf(const std::filesystem::path& path)
  {
    HANDLE h = CreateFileW(path.wstring().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      return std::string();
    std::string identity = directoryIdentity(h);
    CloseHandle(h);
    return identity;
  }

  // A file's stamp from its directory's entry, as the listing has it (collectFiles: the listing's own),
  // so that a file stamped again and the same file listed agree.
  bool listedStamp(const std::filesystem::path& path, FileStamp& stamp)
  {
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(path.wstring().c_str(), &data);
    if (h == INVALID_HANDLE_VALUE)
      return false;
    FindClose(h);
    stamp.time = int64_t((uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime);
    stamp.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
  }
}

std::vector<std::filesystem::path> collectFilesWatched(const std::filesystem::path& root, const std::vector<std::string>& extensions,
                                                       std::vector<FileStamp>& stamps, std::string& note, std::string& missed)
{
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec))
    return collectFiles(root, extensions, &stamps);
  std::filesystem::path absolute = std::filesystem::absolute(root, ec).lexically_normal();
  std::string key = absolute.string();
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(tolower(c)); });
  for (const std::string& e : extensions)
    key += "|" + e;
  auto list = [&](Watch& w)
  {
    w.stamps.clear();
    w.files = collectFiles(root, extensions, &w.stamps);
    w.written.clear();
    w.relist = false;
  };
  // KPP_WATCH_CHECK=1: what the watch gives compared with a listing, each difference a miss.
  auto give = [&](const Watch& w)
  {
    if (getenv("KPP_WATCH_CHECK"))
    {
      std::vector<FileStamp> fresh;
      std::vector<std::filesystem::path> files = collectFiles(root, extensions, &fresh);
      for (size_t i = 0, j = 0; i < files.size() || j < w.files.size();)
      {
        if (j == w.files.size() || (i < files.size() && files[i] < w.files[j]))
          missed += files[i++].string() + " (not in the watched listing); ";
        else if (i == files.size() || w.files[j] < files[i])
          missed += w.files[j++].string() + " (gone, still in the watched listing); ";
        else
        {
          if (!(fresh[i] == w.stamps[j]))
            missed += files[i].string() + " (written, its stamp not the listing's); ";
          ++i, ++j;
        }
      }
      if (!missed.empty())
      {
        stamps.insert(stamps.end(), fresh.begin(), fresh.end());
        return files;
      }
    }
    stamps.insert(stamps.end(), w.stamps.begin(), w.stamps.end());
    return w.files;
  };
  std::unique_lock lock(watchMutex);
  auto found = watches.find(key);
  // The directory its watch has, still under that name: one renamed away and replaced sends no
  // notification. Else (or a watch that broke) the watch goes and a new one is made.
  if (found != watches.end())
  {
    Watch* old = found->second;
    bool same = !old->broken && identityOf(absolute) == old->identity;
    if (!same)
    {
      note += "watch: " + root.string() + (old->broken ? "'s watch ended" : " is another directory now") + ", watched anew; ";
      watches.erase(found);
      dropWatch(lock, old);
      found = watches.end();
    }
  }
  if (found == watches.end())
  {
    // Watched before it is listed: what changes while it is listed is seen at the next build.
    Watch* w = new Watch;
    w->directory = CreateFileW(absolute.wstring().c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    w->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->armed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (w->directory != INVALID_HANDLE_VALUE)
      w->identity = directoryIdentity(w->directory);
    bool started = w->directory != INVALID_HANDLE_VALUE && w->overlapped.hEvent && w->armed && !w->identity.empty();
    if (started)
    {
      std::thread(watchLoop, w).detach();
      lock.unlock(); // (the thread records under the lock)
      WaitForSingleObject(w->armed, INFINITE);
      lock.lock();
      started = !w->broken;
    }
    else
    {
      if (w->directory != INVALID_HANDLE_VALUE)
        CloseHandle(w->directory), w->directory = INVALID_HANDLE_VALUE;
      w->broken = true; // (no thread)
    }
    if (!started)
    {
      dropWatch(lock, w);
      note += "watch: " + root.string() + " cannot be watched, listed; ";
      return collectFiles(root, extensions, &stamps);
    }
    list(*w);
    watches[key] = w;
    note += "watch: " + root.string() + " listed and watched; ";
    return give(*w);
  }
  Watch& w = *found->second;
  // A read that completed (a write just before this build) is recorded before the listing is
  // brought up to date: its thread is waited for (thread A's review, 2026-11-05).
  while (!w.broken && HasOverlappedIoCompleted(&w.overlapped))
  {
    uint64_t seen = w.generation;
    watchRecorded.wait(lock, [&] { return w.generation != seen || w.broken; });
  }
  if (w.broken)
  {
    watches.erase(found);
    dropWatch(lock, &w);
    note += "watch: " + root.string() + "'s watch ended, listed; ";
    return collectFiles(root, extensions, &stamps);
  }
  if (getenv("KPP_WATCH_LOST"))
    w.relist = true;
  if (!w.relist)
  {
    // Each file written stamped again, from its directory's entry as the listing does; one of the
    // extensions that is not in the listing (it was not there when listed) lists again.
    size_t restamped = 0;
    for (const std::wstring& name : w.written)
    {
      std::filesystem::path path = root / name;
      std::string extension = path.extension().string();
      if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
        continue; // (a directory, or a file of no interest)
      auto at = std::lower_bound(w.files.begin(), w.files.end(), path);
      if (at == w.files.end() || *at != path || !listedStamp(path, w.stamps[size_t(at - w.files.begin())]))
      {
        w.relist = true;
        break;
      }
      ++restamped;
    }
    if (!w.relist)
    {
      w.written.clear();
      if (restamped)
        note += "watch: " + root.string() + ": " + std::to_string(restamped) + " file(s) stamped again; ";
      return give(w);
    }
  }
  list(w);
  note += "watch: " + root.string() + " listed again (a file or directory added, removed or renamed, or notifications lost); ";
  return give(w);
}

namespace
{
  std::mutex overlayMutex;
  std::unordered_map<std::string, std::string> overlays;
  std::atomic<bool> anyOverlay{false};
}

std::string overlayKey(const std::filesystem::path& path)
{
  std::error_code ec;
  std::wstring w = std::filesystem::absolute(path, ec).lexically_normal().wstring();
  CharLowerBuffW(w.data(), DWORD(w.size()));
  return std::filesystem::path(w).string();
}

void setFileOverlay(const std::filesystem::path& path, const std::string* text)
{
  std::lock_guard lock(overlayMutex);
  if (text)
    overlays[overlayKey(path)] = *text;
  else
    overlays.erase(overlayKey(path));
  anyOverlay = !overlays.empty();
}

bool hasFileOverlay(const std::filesystem::path& path)
{
  if (!anyOverlay)
    return false;
  std::string key = overlayKey(path);
  std::lock_guard lock(overlayMutex);
  return overlays.count(key) != 0;
}

bool readFile(const std::filesystem::path& path, std::string& out)
{
  if (anyOverlay)
  {
    std::string key = overlayKey(path);
    std::lock_guard lock(overlayMutex);
    if (auto found = overlays.find(key); found != overlays.end())
    {
      out = found->second;
      return true;
    }
  }
  FILE* file = _wfopen(path.c_str(), L"rb");
  if (!file)
    return false;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  out.resize(size_t(size));
  size_t read = fread(out.data(), 1, size_t(size), file);
  fclose(file);
  out.resize(read);
  out.reserve(read + 1); // std::string guarantees the trailing '\0' sentinel.
  return true;
}

bool msvcDiagnostics = false, diagnosticColumns = false;

namespace
{
  bool markedLocations = false;

  // The mark at text[at] ('\1'): its path, line and column (0: none) and its end (past '\3'); false
  // when it is not a whole mark.
  bool readMark(std::string_view text, size_t at, std::string_view& path, std::string_view& line, std::string_view& column, size_t& end)
  {
    size_t close = text.find('\3', at + 1);
    if (close == std::string_view::npos)
      return false;
    std::string_view body = text.substr(at + 1, close - at - 1);
    size_t columnAt = body.find('\2');
    std::string_view place = body.substr(0, columnAt);
    column = columnAt == std::string_view::npos ? std::string_view() : body.substr(columnAt + 1);
    size_t colon = place.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == place.size() || place.find('\1') != std::string_view::npos)
      return false;
    path = place.substr(0, colon);
    line = place.substr(colon + 1);
    auto digits = [](std::string_view s) { return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    if (!digits(line) || !digits(column) || (columnAt != std::string_view::npos && column.empty()))
      return false;
    end = close + 1;
    return true;
  }

  template <class Write>
  std::string rewriteMarks(std::string_view text, const Write& write)
  {
    std::string out;
    out.reserve(text.size());
    size_t from = 0;
    for (size_t at = text.find('\1'); at != std::string_view::npos; at = text.find('\1', at + 1))
    {
      std::string_view path, line, column;
      size_t end = 0;
      if (!readMark(text, at, path, line, column, end))
        continue;
      out.append(text.substr(from, at - from));
      write(out, path, line, column);
      from = end;
      at = end - 1;
    }
    out.append(text.substr(from));
    return out;
  }
}

void markDiagnosticLocations() { markedLocations = true; }

std::string renderDiagnostics(std::string_view text, bool msvc)
{
  if (text.find('\1') == std::string_view::npos)
    return std::string(text);
  return rewriteMarks(text, [&](std::string& out, std::string_view path, std::string_view line, std::string_view column)
  {
    out.append(path);
    out += msvc ? '(' : ':';
    out.append(line);
    if (!column.empty())
    {
      out += msvc ? ',' : ':';
      out.append(column);
    }
    if (msvc)
      out += ')';
  });
}

std::string plainDiagnostic(std::string_view text) { return renderDiagnostics(text, false); }

void sortDiagnostics(std::vector<std::string>& diagnostics)
{
  std::vector<std::pair<std::string, std::string>> keyed; // (plain, as written)
  keyed.reserve(diagnostics.size());
  for (std::string& d : diagnostics)
    keyed.emplace_back(plainDiagnostic(d), std::move(d));
  std::sort(keyed.begin(), keyed.end());
  for (size_t i = 0; i < keyed.size(); ++i)
    diagnostics[i] = std::move(keyed[i].second);
}

std::string sourceLocation(const std::string& path, size_t line, size_t column)
{
  if (markedLocations)
    return '\1' + path + ':' + std::to_string(line) + (diagnosticColumns && column ? '\2' + std::to_string(column) : std::string()) + '\3';
  if (diagnosticColumns && column)
    return msvcDiagnostics ? path + "(" + std::to_string(line) + "," + std::to_string(column) + ")"
                           : path + ":" + std::to_string(line) + ":" + std::to_string(column);
  return msvcDiagnostics ? path + "(" + std::to_string(line) + ")" : path + ":" + std::to_string(line);
}

namespace
{
  std::vector<std::pair<std::string, std::string>> filePrefixMap; // FROM absolute, '/' between its names, none at its end; TO

  std::string comparablePath(const std::string& path)
  {
    std::error_code ec;
    std::string p = std::filesystem::absolute(path, ec).lexically_normal().generic_string();
    while (p.size() > 1 && p.back() == '/')
      p.pop_back();
    return p;
  }
}

void setFilePrefixMap(const std::vector<std::pair<std::string, std::string>>& map)
{
  filePrefixMap.clear();
  for (const auto& [from, to] : map)
    filePrefixMap.push_back({comparablePath(from), to});
  std::stable_sort(filePrefixMap.begin(), filePrefixMap.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

std::string programPath(const std::string& path)
{
  if (filePrefixMap.empty())
    return path;
  std::string full = comparablePath(path);
  for (const auto& [from, to] : filePrefixMap)
    if (full.size() > from.size() && full[from.size()] == '/' && _strnicmp(full.c_str(), from.c_str(), from.size()) == 0)
    {
      std::string rest = full.substr(from.size() + 1);
      return to.empty() ? rest : to + "/" + rest;
    }
  return path;
}

std::filesystem::path compilerPath()
{
  wchar_t buffer[MAX_PATH];
  DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return std::filesystem::path(std::wstring(buffer, length));
}

std::filesystem::path defaultStdlib(const std::filesystem::path& compiler)
{
  std::filesystem::path directory = compiler.parent_path();
  std::error_code ec;
  if (std::filesystem::is_directory(directory / "stdlib", ec))
    return directory / "stdlib";
  return (directory / ".." / ".." / "stdlib").lexically_normal();
}

std::filesystem::path defaultLibraries(const std::filesystem::path& compiler)
{
  return defaultStdlib(compiler).parent_path() / "libraries";
}

std::filesystem::path defaultSysroot(const std::filesystem::path& compiler, const char* name, std::string* what)
{
  std::filesystem::path root = compiler.parent_path() / "sysroots" / name;
  std::error_code ec;
  if (!std::filesystem::is_directory(root / "usr" / "include", ec))
    return {};
  if (what)
  {
    std::string text;
    if (readFile(root / "kpp-sysroot.txt", text))
      *what = text.substr(0, text.find_first_of("\r\n"));
  }
  return root;
}

namespace
{
  std::string chosenClang; // --toolchain's (selectToolchain), for this build
  bool chosenBundled = false;
  std::string skippedToolchains; // why a list's toolchains before the one taken are not here (toolchainSkipped)

  std::filesystem::path bundledClang(const std::filesystem::path& compiler) { return compiler.parent_path() / "bin" / "clang.exe"; }

  std::string visualStudioClang();
  std::string visualStudioMissing();
}

std::string selectToolchain(const std::filesystem::path& compiler, const std::string& toolchain)
{
  chosenClang.clear();
  chosenBundled = false;
  skippedToolchains.clear();
  std::error_code ec;
  // A list (`msvc,bundled`): the first one of it this machine has, as if it alone were named. A
  // build description that prefers Visual Studio's still builds where only the bundled one is.
  if (toolchain.find(',') != std::string::npos)
  {
    std::vector<std::string> names;
    for (size_t at = 0; at <= toolchain.size();)
    {
      size_t end = std::min(toolchain.find(',', at), toolchain.size());
      names.push_back(toolchain.substr(at, end - at));
      at = end + 1;
    }
    for (const std::string& one : names)
      if (one != "bundled" && one != "msvc")
        return "--toolchain takes bundled, msvc or a list of them in the order preferred (msvc,bundled), not '" + toolchain + "'";
    std::string missing;
    for (const std::string& one : names)
    {
      std::string error = selectToolchain(compiler, one);
      if (error.empty())
      {
        skippedToolchains = missing; // (what kpp says of a build that did not take the first one)
        return "";
      }
      missing += (missing.empty() ? "" : "; ") + error;
    }
    return "--toolchain " + toolchain + ": none of them is here (" + missing + ")";
  }
  if (toolchain == "bundled")
  {
    std::filesystem::path clang = bundledClang(compiler);
    if (!std::filesystem::exists(clang, ec))
      return "--toolchain bundled: this installation has no " + clang.string();
    chosenClang = clang.string();
    chosenBundled = true;
  }
  else if (toolchain == "msvc")
  {
    chosenClang = visualStudioClang();
    if (chosenClang.empty())
      return "--toolchain msvc " + visualStudioMissing();
  }
  else if (!toolchain.empty())
    return "--toolchain takes bundled, msvc or a list of them in the order preferred (msvc,bundled), not '" + toolchain + "'";
  else if (getenv("KPP_CLANG"))
    ;
  else if (std::filesystem::exists(bundledClang(compiler), ec))
  {
    chosenClang = bundledClang(compiler).string();
    chosenBundled = true;
  }
  else if (visualStudioClang().empty())
    return "this kpp has no toolchain of its own (" + bundledClang(compiler).parent_path().string() + "), and building with Visual Studio's " +
           visualStudioMissing();
  return "";
}

bool bundledToolchain()
{
  if (!chosenClang.empty())
    return chosenBundled;
  std::error_code ec;
  return !getenv("KPP_CLANG") && std::filesystem::exists(bundledClang(compilerPath()), ec);
}

std::string toolchainName()
{
  if (bundledToolchain())
    return "bundled";
  return chosenClang.empty() && getenv("KPP_CLANG") ? "KPP_CLANG" : "msvc";
}

std::string toolchainSkipped() { return skippedToolchains; }

std::string visualStudioToolchain(std::string& missing)
{
  std::string clang = visualStudioClang();
  missing = clang.empty() ? visualStudioMissing() : "";
  return clang;
}

void setThreadBelowNormal(bool below)
{
  SetThreadPriority(GetCurrentThread(), below ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
}

std::string clangPath()
{
  if (!chosenClang.empty())
    return chosenClang;
  if (const char* clangEnv = getenv("KPP_CLANG"))
    return clangEnv;
  std::error_code ec;
  if (std::filesystem::path bundled = bundledClang(compilerPath()); std::filesystem::exists(bundled, ec))
    return bundled.string();
  return visualStudioClang();
}

namespace
{
// vswhere's lines for these arguments: the Visual Studio installer's own (KPP_VSWHERE names another,
// tests\toolchain.ps1's ones that find no Clang component or nothing).
std::vector<std::string> vswhereLines(const std::string& arguments)
{
  const char* own = getenv("KPP_VSWHERE");
  std::string program = own ? own : "%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe";
  std::vector<std::string> lines;
  FILE* pipe = _popen(("\"\"" + program + "\" " + arguments + " 2>nul\"").c_str(), "r");
  if (!pipe)
    return lines;
  char line[1024] = {};
  while (fgets(line, sizeof(line), pipe))
  {
    std::string out = line;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
      out.pop_back();
    if (!out.empty())
      lines.push_back(out);
  }
  _pclose(pipe);
  return lines;
}

std::string vswhere(const std::string& arguments)
{
  std::vector<std::string> lines = vswhereLines(arguments);
  return lines.empty() ? std::string() : lines[0];
}

// The clang of the newest Visual Studio with its Clang component, any edition, a prerelease one
// (Preview, Insiders) only when no other has it; empty when there is none (never another one guessed
// in its place). Asked once per vswhere.
namespace
{
  // What the installed Visual Studios are as vswhere reads them: each instance's state.json under
  // %ProgramData%\Microsoft\VisualStudio\Packages\_Instances, by its size and time of writing (an
  // install, a modification adding the Clang component, an update or a removal changes it). Empty
  // when the directory cannot be read.
  std::string visualStudioInstancesStamp()
  {
    const char* data = getenv("ProgramData");
    if (!data)
      return "";
    std::error_code ec;
    std::filesystem::path instances = std::filesystem::path(data) / "Microsoft" / "VisualStudio" / "Packages" / "_Instances";
    std::vector<std::string> parts;
    for (std::filesystem::directory_iterator i(instances, ec), end; !ec && i != end; i.increment(ec))
    {
      std::filesystem::path state = i->path() / "state.json";
      std::error_code e2;
      auto size = std::filesystem::file_size(state, e2);
      auto time = std::filesystem::last_write_time(state, e2);
      if (e2)
        return "";
      parts.push_back(i->path().filename().string() + " " + std::to_string(size) + " " + std::to_string(time.time_since_epoch().count()));
    }
    if (ec)
      return "";
    std::sort(parts.begin(), parts.end());
    std::string stamp;
    for (const std::string& p : parts)
      stamp += p + "\n";
    return stamp;
  }

  // vswhere's answer (visualStudioClang) kept in %TEMP%\kpp-visual-studio-clang.txt while the
  // instances are as they were: running vswhere was 50 to 60 ms of every kppc process, every spec
  // snippet's included (2026-11-04). Not with KPP_VSWHERE (a test's own vswhere).
  std::string cachedVisualStudioClangPath()
  {
    char temp[MAX_PATH + 1];
    DWORD n = GetTempPathA(DWORD(sizeof temp), temp);
    return n ? std::string(temp, n) + "kpp-visual-studio-clang.txt" : std::string();
  }
}

std::string visualStudioClang()
{
  static std::mutex mutex;
  static std::map<std::string, std::string> found;
  const char* own = getenv("KPP_VSWHERE");
  std::lock_guard lock(mutex);
  auto [it, added] = found.try_emplace(own ? own : "");
  std::string stamp, cachePath;
  if (added && !own)
  {
    stamp = visualStudioInstancesStamp();
    cachePath = cachedVisualStudioClangPath();
    std::string cached;
    if (!stamp.empty() && !cachePath.empty() && readFile(cachePath, cached))
    {
      size_t split = cached.rfind('\n', cached.size() > 1 ? cached.size() - 2 : 0);
      std::string clang = split == std::string::npos ? "" : cached.substr(split + 1);
      while (!clang.empty() && (clang.back() == '\n' || clang.back() == '\r'))
        clang.pop_back();
      std::error_code ec;
      if (split != std::string::npos && cached.compare(0, split + 1, stamp) == 0 && !clang.empty() && std::filesystem::exists(clang, ec))
      {
        it->second = clang;
        return it->second;
      }
    }
  }
  if (added)
    for (const char* prerelease : {"", " -prerelease"})
    {
      std::string installation =
        vswhere(std::string("-latest") + prerelease + " -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath");
      std::error_code ec;
      std::filesystem::path clang = std::filesystem::path(installation) / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "clang.exe";
      if (!installation.empty() && std::filesystem::exists(clang, ec))
      {
        it->second = clang.string();
        break;
      }
    }
  if (added && !own && !stamp.empty() && !cachePath.empty() && !it->second.empty())
  {
    // (written whole under another name and renamed: another kppc reading it meanwhile sees the
    // old file or the new one)
    std::string part = cachePath + "." + std::to_string(GetCurrentProcessId());
    if (FILE* f = fopen(part.c_str(), "wb"))
    {
      std::string text = stamp + it->second + "\n";
      bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
      written = fclose(f) == 0 && written;
      if (!written || !MoveFileExA(part.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(part.c_str());
    }
  }
  return it->second;
}

// What a build that needs Visual Studio's clang lacks, and what to install (the end of a sentence
// that names the build): every Visual Studio here, prerelease ones too, lacks its Clang component.
std::string visualStudioMissing()
{
  std::vector<std::string> installations = vswhereLines("-all -prerelease -products * -property installationPath");
  std::string which;
  for (size_t i = 0; i < installations.size(); ++i)
    which += (i == 0 ? "" : i + 1 == installations.size() ? " and " : ", ") + installations[i];
  if (installations.size() == 1)
    return "needs Visual Studio's \"C++ Clang Compiler for Windows\" (with \"MSVC v143 - VS 2022 C++ x64/x86 build tools\" and a Windows "
           "SDK), which " + which + " does not have: Visual Studio Installer > Modify > Individual components";
  if (!installations.empty())
    return "needs Visual Studio's \"C++ Clang Compiler for Windows\" (with \"MSVC v143 - VS 2022 C++ x64/x86 build tools\" and a Windows "
           "SDK), which none of " + which + " has: Visual Studio Installer > Modify > Individual components, in one of them";
  return "needs Visual Studio 2022 or its Build Tools, and none is installed: install it with \"Desktop development with C++\" and, "
         "under Individual components, \"C++ Clang Compiler for Windows\"";
}
}

namespace
{
  std::atomic<bool> zeroNullWarningsEnabled{false};
  std::mutex zeroNullMutex;
  std::set<std::string> zeroNullNotes; // (sorted, each token once)
}

std::set<std::pair<const void*, uint32_t>> zeroNullSeen;

bool firstZeroNull(const void* file, uint32_t token)
{
  std::lock_guard lock(zeroNullMutex);
  return zeroNullSeen.insert({file, token}).second;
}

void setZeroNullWarnings(bool on)
{
  std::lock_guard lock(zeroNullMutex);
  zeroNullNotes.clear();
  zeroNullSeen.clear();
  zeroNullWarningsEnabled = on;
}

bool zeroNullWarningsOn() { return zeroNullWarningsEnabled; }

void noteZeroNullWarning(const std::string& warning)
{
  std::lock_guard lock(zeroNullMutex);
  zeroNullNotes.insert(warning);
}

std::vector<std::string> takeZeroNullWarnings()
{
  std::lock_guard lock(zeroNullMutex);
  std::vector<std::string> out(zeroNullNotes.begin(), zeroNullNotes.end());
  zeroNullNotes.clear();
  sortDiagnostics(out); // (as a build without a server orders them)
  return out;
}
