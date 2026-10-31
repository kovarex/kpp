#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SourceFile
{
  std::filesystem::path path;
  std::string text; // Always followed by a '\0' sentinel so the lexer needs no bounds checks.
};

// Recursively collects files with the given extensions under root.
std::vector<std::filesystem::path> collectFiles(const std::filesystem::path& root,
                                                const std::vector<std::string>& extensions);

bool readFile(const std::filesystem::path& path, std::string& out);

// Where a diagnostic is: `path:line`, or with --msvc-diagnostics `path(line)`, the form Visual
// Studio's Error List reads (docs/build.md, "Visual Studio"). Every diagnostic's place is made here.
extern bool msvcDiagnostics;
std::string sourceLocation(const std::string& path, size_t line);

// The running compiler's executable, and the standard library a compiler at `compiler` uses
// unless --std names one: stdlib\ next to it in an installation (docs/build.md, "Installing"),
// ..\..\stdlib from compiler\build\ in this repository. A compile server runs from a copy of kppc
// elsewhere, so it is given its client's path (main makes argv[0] that).
std::filesystem::path compilerPath();
std::filesystem::path defaultStdlib(const std::filesystem::path& compiler);

// The clang kppc compiles and links with: KPP_CLANG, else the clang of the newest Visual Studio
// with its Clang component, any edition (vswhere), else Community 2022's. clang-cl, llvm-lib and
// llvm-rc are next to it.
std::string clangPath();
