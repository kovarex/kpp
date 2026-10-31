#include "source.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
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

std::string sourceLocation(const std::string& path, size_t line, size_t column)
{
  if (diagnosticColumns && column)
    return msvcDiagnostics ? path + "(" + std::to_string(line) + "," + std::to_string(column) + ")"
                           : path + ":" + std::to_string(line) + ":" + std::to_string(column);
  return msvcDiagnostics ? path + "(" + std::to_string(line) + ")" : path + ":" + std::to_string(line);
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

std::string clangPath()
{
  if (const char* clangEnv = getenv("KPP_CLANG"))
    return clangEnv;
  static const std::string found = []
  {
    std::string fallback = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang.exe";
    FILE* pipe = _popen("\"\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\" -latest -products * "
                        "-requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath 2>nul\"", "r");
    if (!pipe)
      return fallback;
    char line[1024] = {};
    bool read = fgets(line, sizeof(line), pipe) != nullptr;
    _pclose(pipe);
    std::string installation = read ? line : "";
    while (!installation.empty() && (installation.back() == '\n' || installation.back() == '\r'))
      installation.pop_back();
    std::error_code ec;
    std::filesystem::path clang = std::filesystem::path(installation) / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "clang.exe";
    return !installation.empty() && std::filesystem::exists(clang, ec) ? clang.string() : fallback;
  }();
  return found;
}
