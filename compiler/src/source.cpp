#include "source.hpp"
#include <algorithm>
#include <cstdio>
#include <windows.h>

std::vector<std::filesystem::path> collectFiles(const std::filesystem::path& root,
                                                const std::vector<std::string>& extensions)
{
  std::vector<std::filesystem::path> result;
  if (std::filesystem::is_regular_file(root))
  {
    result.push_back(root);
    return result;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
         root, std::filesystem::directory_options::skip_permission_denied))
  {
    if (!entry.is_regular_file())
      continue;
    std::string extension = entry.path().extension().string();
    if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end())
      result.push_back(entry.path());
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool readFile(const std::filesystem::path& path, std::string& out)
{
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

bool msvcDiagnostics = false;

std::string sourceLocation(const std::string& path, size_t line)
{
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
