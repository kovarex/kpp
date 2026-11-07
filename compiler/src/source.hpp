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

// collectFiles for a compile server: the listing of a directory is kept between its builds and
// brought up to date by the directory's change notifications (ReadDirectoryChangesW): a file written
// is stamped again alone, and a file or directory added, removed or renamed, lost notifications
// (an overflow) or a watch that cannot be made list the directory again. An edit build of the game
// listed its 3,500 files every time, 37 to 68 ms (2026-11-05). note says what it did, for the log.
// KPP_WATCH_LOST=1 takes every notification as lost (tests/incremental.ps1); KPP_NO_WATCH=1 lists every
// build (Driver/Compile.inl).
// KPP_WATCH_CHECK=1 lists every time too and names each difference in missed (the listing is what it
// gives then): the tests' check that the watch misses nothing.
std::vector<std::filesystem::path> collectFilesWatched(const std::filesystem::path& root, const std::vector<std::string>& extensions,
                                                       std::vector<FileStamp>& stamps, std::string& note, std::string& missed);

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

// --zero-null-warnings: the literal 0s that would be pointers (design.md 4, an error) are warnings
// instead, "PATH:LINE[:COL]: warning: the literal 0 is no pointer: write nullptr (design.md 4)", each token
// once, printed after codegen (takeZeroNullWarnings): convert.cmd's nullptr pass (fix-null.py) rewrites
// what they name in the C++ it converted. set turns the mode on and clears the notes.
// --override-warnings: a class with a virtual function whose destructor is not virtual, which kpp's rule
// makes virtual (design.md 23b), "PATH:LINE: warning: X has a virtual function and an implicit destructor,
// which becomes virtual", each once: the count before that rule lands. (Required `override` is an error.)
void setOverrideWarnings(bool on);
bool overrideWarningsOn();
void noteOverrideWarning(const std::string& warning);
std::vector<std::string> takeOverrideWarnings();
// --this-warnings: an unqualified name that finds a data member or a member function, "PATH:LINE: warning:
// NAME is a data member: write this.NAME" (a member function; a static member's `This::NAME`), each once per
// place; one beside a global of its spelling says so. The count before required `this.`
// (the readme's Planned, design.md 23c) becomes an error.
void setThisWarnings(bool on);
bool thisWarningsOn();
void noteThisWarning(const std::string& warning);
std::vector<std::string> takeThisWarnings();
void setZeroNullWarnings(bool on);
bool zeroNullWarningsOn();
void noteZeroNullWarning(const std::string& warning);
std::vector<std::string> takeZeroNullWarnings();
// Whether the literal 0 at token of file is reported for the first time in this build (the walk
// reports what it sees, codegen the rest, a brace list's members: each once, an instance's copy too).
bool firstZeroNull(const void* file, uint32_t token);

// In a compile server, a diagnostic's place is written marked and the client that prints it gives
// it its form (renderDiagnostics): Visual Studio's builds (--msvc-diagnostics) and a terminal's share
// one server, whose kept errors and replayed output were written for one of them (server.hpp,
// serverKey). The mark: '\1' path ':' line [ '\2' column ] '\3', the column with --diagnostic-columns
// alone (which stays in the server's key: it decides which errors are one error). Set once, by the
// server before its first build.
void markDiagnosticLocations();
// text with each marked place written as sourceLocation writes it for msvc (`path(line)`) or not
// (`path:line`); what is not a whole mark stays as it is.
std::string renderDiagnostics(std::string_view text, bool msvc);
// A diagnostic as it sorts: a marked place as `path:line[:column]`, as a build outside a server
// writes it without --msvc-diagnostics, so that a server's diagnostics come in the same order as that
// build's (text without a mark is returned as it is).
std::string plainDiagnostic(std::string_view text);
// Sorts diagnostics as their texts outside a server sort (by plainDiagnostic).
void sortDiagnostics(std::vector<std::string>& diagnostics);

// --file-prefix-map FROM=TO (each one): the source paths a program's own text holds (an assertion's
// message, a CallerLocation's file) start with TO in place of the directory FROM, '/' between their
// names, so that a binary does not hold this machine's paths (kpp's release and final builds: the
// standard library's directory as stdlib, each kpp library's as its name, the project's as nothing).
// The longest FROM a path is under is taken; an empty TO leaves the path relative to FROM.
// Diagnostics and debug info keep the paths as they are: an IDE and a debugger open them.
void setFilePrefixMap(const std::vector<std::pair<std::string, std::string>>& map);
std::string programPath(const std::string& path);

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
// What is wrong with sdk as the macOS SDK of a build for a Mac (arch "arm64" or "x86_64"), "" when
// nothing: a directory whose SDKSettings.json says version 11.0 or newer, with usr/include/stdio.h and a
// usr/lib/libSystem.tbd that lists the architecture (design.md, "Targets and hosts", Q7: each user
// supplies one, and kppc says what to have when it is missing or wrong).
std::string macSdkProblem(const std::string& sdk, const char* arch);
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
// For a Windows target (triple: clang's --target) on a host whose file names have case (Linux,
// FreeBSD): ` -ivfsoverlay "FILE"`, an overlay in which the toolchain's system include directories are
// found without case, as on Windows: MinGW-w64's headers are lower case, and Windows code includes
// <Windows.h> and <WinSock2.h>. Made once per toolchain and target, in the imports' cache; "" on a
// Windows host or for another target.
std::string headerCaseOverlay(const std::string& clang, const std::string& triple);
// The toolchain a build takes, as --toolchain names it ("bundled", "msvc"), or "KPP_CLANG".
std::string toolchainName();
// Why the last selectToolchain's list did not take its toolchains before the one it took (`--toolchain
// msvc needs Visual Studio's "C++ Clang Compiler for Windows" ...`), what kpp says of such a build;
// empty when it took its first one, or was no list.
std::string toolchainSkipped();
// Visual Studio's clang (visualStudioClang: of the newest Visual Studio with its Clang component, a
// prerelease one if no other has it), or why there is none: what kpp --version says.
std::string visualStudioToolchain(std::string& missing);

