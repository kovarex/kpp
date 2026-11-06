#pragma once

// The host kppc runs on (docs/design.md, "Targets and hosts", 5): what Windows and POSIX (Linux,
// FreeBSD, macOS) do differently, behind one interface, so that the rest of kppc has no #ifdef. The
// Windows side is what kppc did before, moved here unchanged; host.cpp has both.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace host
{
  // The platform kppc runs on as `--target` names it (windows, linux, freebsd): the default target
  // (design.md, "Targets and hosts", 1: the host's own platform).
  const char* platformName();

  // Whether kppc has its compile server on this host (server.cpp: Windows' named pipes, Linux's and
  // FreeBSD's Unix sockets). A build that would go through it builds in its own process instead
  // (kpp, --run).
#if defined(_WIN32) || defined(__linux__) || defined(__FreeBSD__)
  constexpr bool compileServer = true;
#else
  constexpr bool compileServer = false;
#endif

  // This executable's path.
  std::filesystem::path executablePath();
  int processId();

  // An executable's file name suffix: ".exe", "".
  const char* executableSuffix();
  // The program name finds on PATH (name with executableSuffix); empty when none.
  std::filesystem::path findOnPath(const std::string& name);
  // The calling thread's priority below normal (the compile server's background work), or normal.
  void setThreadBelowNormal(bool below);

  // Whether the host's file names ignore case (Windows'), and the key two spellings of one path share
  // (lower-cased there; its text as it is on POSIX).
#ifdef _WIN32
  constexpr bool pathsIgnoreCase = true;
#else
  constexpr bool pathsIgnoreCase = false;
#endif
  std::string pathKey(const std::filesystem::path& path);

  // A path from its UTF-8 text (Windows' paths are UTF-16; POSIX's are the bytes).
  std::filesystem::path pathOfUtf8(const std::string& text);

  // This process's arguments from first on as UTF-8 text, as a kpp program's argv is (kppc --run's
  // program arguments): on Windows the C runtime's wide argv, split from the command line as the
  // narrow argv was (that one is the ANSI code page's: U+017E as 9e in 1250, Japanese as `??`); POSIX's
  // as they are. False, with why, for an argument that has no UTF-8 form (a lone surrogate), named by
  // its place counted from first (1).
  bool argumentsAsUtf8(int argc, char** argv, int first, std::vector<std::string>& out, std::string& why);

  // fopen of a path of any characters (Windows: its UTF-16 form; POSIX: its bytes).
  FILE* openFile(const std::filesystem::path& path, const char* mode);
  // A command line run by the shell (cmd.exe, /bin/sh), read or written through a pipe: popen and
  // pclose (pclose's result is the command's exit code, as Windows' _pclose gives it). On Windows
  // the command line is quoted whole for cmd /c, which takes those quotes off.
  FILE* openPipe(const std::string& command, const char* mode);
  int closePipe(FILE* pipe);

  // A command line run by the shell to its end, its exit code (system's, the wait status decoded on
  // POSIX): on Windows cmd /c with the whole line quoted (so that one starting with a quoted program
  // path keeps its quotes), in directory first when given (cd /d DIR && ...; POSIX: cd DIR && ...).
  int shell(const std::string& command, const std::filesystem::path& directory = {});
  // A command line as given (a build step build.kpp wrote): system's, the line not quoted again.
  int shellAsIs(const std::string& line);

  // The null device as a shell command line names it: NUL, /dev/null.
  const char* nullDevice();

  // A stream of bytes the language server talks through (lsp.cpp): a pipe, a file. readStream gives
  // what came, up to size: 0 at the end of the input or on an error.
  using Stream = intptr_t;
  constexpr Stream noStream = -1;
  size_t readStream(Stream stream, char* buffer, size_t size);
  void writeStream(Stream stream, const char* data, size_t size);
  // A file written from its start (--lsp-log), shared for reading; noStream when it cannot be made.
  Stream createLog(const std::filesystem::path& path);
  // stdin, stdout and stderr for the caller alone: duplicated into in, out and err, and the
  // process's own replaced by the null device, so that what a build prints reaches no protocol.
  void takeStandardStreams(Stream& in, Stream& out, Stream& err);

  // Whether a person answers on stdin: a terminal (on Windows also the pipe a Cygwin or MSYS
  // terminal gives its programs, which _isatty does not take for one).
  bool interactiveInput();

  // A process run to its end: program with its arguments (no shell between; Windows quotes them as
  // CommandLineToArgvW reads them back), in directory ("": this one's). Its stdout and stderr:
  // appended to appendTo when given (and no stdin), else this process's (its stdin too), passed through
  // seen too (each piece as it comes; k 0 stdout, 1 stderr) when given. The exit code; -1, with why
  // set, when it could not be started.
  struct Run
  {
    std::filesystem::path directory;
    std::filesystem::path appendTo;
    std::function<void(int k, const char* data, size_t size)> seen;
  };
  int run(const std::string& program, const std::vector<std::string>& arguments, const Run& how, std::string& why);
  // A command line run as run runs a program: on Windows CreateProcess reads it (its first word the
  // program, no cmd.exe between), on POSIX /bin/sh -c.
  int runCommandLine(const std::string& commandLine, const Run& how, std::string& why);

  // fn on Ctrl+C (and on a closed console window on Windows), before the process ends.
  void onInterrupt(void (*fn)());

  // The machine as `kpp measure`'s table names it: the processor, its cores and threads, the memory.
  std::string machineDescription();
  // How busy the machine is, every core, over the next second, in percent; -1 where kppc cannot
  // tell (macOS so far).
  int machineLoad();

  // A file held open: shared by every holder (a run in its directory, run.cpp), or alone (the
  // cleanup, which deletes the directory only then). release() lets it go; the destructor too.
  class FileLock
  {
  public:
    FileLock() = default;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    ~FileLock() { release(); }
    // Opens (making) path, shared with other shared holders: false while one holds it alone.
    bool shared(const std::filesystem::path& path);
    // Opens path alone: false while anyone else holds it. Windows deletes it when let go.
    bool alone(const std::filesystem::path& path);
    // Sets the file's last write time to now (the time of last use run.cpp's cleanup orders by).
    void touch();
    void release();
    bool held() const { return handle_ != -1; }

  private:
    std::filesystem::path path_;
    intptr_t handle_ = -1;
  };
}
