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

// A file's last write time and size, as a directory listing gives them: a compile server takes a
// file whose stamp is its last build's as unchanged (main.cpp, compile), as build tools do.
struct FileStamp
{
  int64_t time = 0;
  uint64_t size = 0;
  bool operator==(const FileStamp&) const = default;
};

// Recursively collects files with the given extensions under root (sorted), and with stamps each
// one's stamp (the listing's: no file opened).
std::vector<std::filesystem::path> collectFiles(const std::filesystem::path& root,
                                                const std::vector<std::string>& extensions, std::vector<FileStamp>* stamps = nullptr);

bool readFile(const std::filesystem::path& path, std::string& out);
// Whether readFile gives an overlay's text for path (setFileOverlay), not the file's.
bool hasFileOverlay(const std::filesystem::path& path);

// A text that stands for a file on disk, as an editor holds it unsaved (`kppc --lsp`, lsp.hpp):
// readFile gives it instead of the file's contents. Null removes it. Paths compare as Windows
// does (absolute, normalised, case-insensitive).
void setFileOverlay(const std::filesystem::path& path, const std::string* text);
std::string overlayKey(const std::filesystem::path& path);

// Where a diagnostic is: `path:line`, or with --msvc-diagnostics `path(line)`, the form Visual
// Studio's Error List reads (docs/build.md, "Visual Studio"). Every diagnostic's place is made here.
// diagnosticColumns (--diagnostic-columns): the column too where the caller knows it,
// `path:line:column` or `path(line,column)`; columns count bytes from 1.
extern bool msvcDiagnostics, diagnosticColumns;
std::string sourceLocation(const std::string& path, size_t line, size_t column = 0);

// The running compiler's executable, and the standard library a compiler at `compiler` uses
// unless --std names one: stdlib\ next to it in an installation (docs/build.md, "Installing"),
// ..\..\stdlib from compiler\build\ in this repository. A compile server runs from a copy of kppc
// elsewhere, so it is given its client's path (main makes argv[0] that).
std::filesystem::path compilerPath();
// The calling thread below normal priority (true) or at normal priority (false): step 4's P2 runs the
// back end below codegen's threads (main.cpp, BackEndPool).
void setThreadBelowNormal(bool below);
std::filesystem::path defaultStdlib(const std::filesystem::path& compiler);
// A target's sysroot (its C headers and libraries) that comes with kppc: sysroots\NAME next to it,
// NAME freebsd-x86_64 or linux-x86_64 (a release brings FreeBSD's; tools\sysroot\freebsd.ps1 and
// tests\targets.ps1 make them in compiler\build\sysroots for a build of kppc). "" when there is none.
// what is the sysroot's own description (its kpp-sysroot.txt: "FreeBSD 14.3-RELEASE ..."), if any.
std::filesystem::path defaultSysroot(const std::filesystem::path& compiler, const char* name, std::string* what = nullptr);
// The official libraries kpp brings (libraries\ beside stdlib\: a release's, or a checkout's for a
// build of kppc), where use("NAME") looks last (docs/build.md, "kpp libraries").
std::filesystem::path defaultLibraries(const std::filesystem::path& compiler);

// kpp's version, MAJOR.MINOR.PATCH with "+dirty" for a build of uncommitted changes, or "unknown"
// outside git, and the commit it was built from ("" when unknown): compiler/version.cmake writes them
// at every build (docs/build.md, "Versions").
const char* kppVersion();
const char* kppCommit();

// The clang kppc compiles and links with (docs/build.md, "The toolchain"): --toolchain's choice
// (selectToolchain), else KPP_CLANG, else an installation's own (bin\clang.exe next to kpp.exe, the
// bundled toolchain: clang, lld-link, ld.lld, llvm-lib and llvm-rc of LLVM 19.1.7), else the clang of
// the newest Visual Studio with its Clang component, any edition (vswhere), else Community 2022's.
// lld-link, llvm-lib and llvm-rc are next to it.
std::string clangPath();
// --toolchain bundled|msvc for the compiler at `compiler` (argv[0]: the client's, in a compile
// server): bundled is bin\ next to it, msvc Visual Studio's clang; empty is the default above. The
// error, if the one named is not there (never the other one in its place). A list, `msvc,bundled`,
// takes the first of them that is there, and is an error when none is.
std::string selectToolchain(const std::filesystem::path& compiler, const std::string& toolchain);
// Whether clangPath() is the bundled toolchain's: its links are lld-link's (Visual Studio's link.exe
// is not part of it).
bool bundledToolchain();
// The toolchain a build takes, as --toolchain names it ("bundled", "msvc"), or "KPP_CLANG".
std::string toolchainName();
// Why the last selectToolchain's list did not take its toolchains before the one it took (`--toolchain
// msvc needs Visual Studio's "C++ Clang Compiler for Windows" ...`), what kpp says of such a build;
// empty when it took its first one, or was no list.
std::string toolchainSkipped();
// Visual Studio's clang (visualStudioClang: of the newest Visual Studio with its Clang component, a
// prerelease one if no other has it), or why there is none: what kpp --version says.
std::string visualStudioToolchain(std::string& missing);
