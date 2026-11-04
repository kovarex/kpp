#include "source.hpp"
#include <algorithm>
#include <atomic>
#include <map>
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
std::string visualStudioClang()
{
  static std::mutex mutex;
  static std::map<std::string, std::string> found;
  const char* own = getenv("KPP_VSWHERE");
  std::lock_guard lock(mutex);
  auto [it, added] = found.try_emplace(own ? own : "");
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
