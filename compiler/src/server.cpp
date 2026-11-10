#include "server.hpp"
#include "source.hpp"

#include "host.hpp"

// The compile server on Windows (a named pipe per key) and on POSIX hosts (a Unix domain socket per
// key, design.md "Targets and hosts" 5): one protocol, one session, each OS's primitives beside each
// other below, Windows' as they were.
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <condition_variable>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <memory>
extern char** environ;
#endif
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <regex>
#include <sys/stat.h>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

// The protocol (docs/compile-speed.md, "The compile server: protocol"). One named pipe per key,
// one client at a time. A request: "KPS1", a u32 kind (1 build, 2 stop), then for a build the
// kpp path the client ran, its working directory, its arguments (a u32 count, then each) and its
// environment (UTF-16, as GetEnvironmentStringsW gives it); a string is a u32 byte count and the
// bytes. The answer: frames of a u8 type (1 stdout, 2 stderr, 3 exit code, 4 the server's process
// id, sent first), a u32 byte count and the bytes, ending with the exit code's frame. The server
// logs what it does to %TEMP%\kpp-server\KEY.log: a client that loses its server says why.

namespace
{
  const uint32_t buildRequest = 1, stopRequest = 2;
  const uint8_t stdoutFrame = 1, stderrFrame = 2, exitFrame = 3, processFrame = 4;
  const int64_t idleSeconds = 3600;

  // A channel to a server or to a client: Windows' pipe HANDLE, POSIX's socket descriptor.
#ifdef _WIN32
  using Channel = HANDLE;
  const Channel noChannel = INVALID_HANDLE_VALUE;
#else
  using Channel = int;
  const Channel noChannel = -1;
#endif

  std::string serverDirectory()
  {
#ifdef _WIN32
    char temp[MAX_PATH * 4];
    DWORD n = GetTempPathA(DWORD(sizeof temp), temp);
    return std::string(temp, n) + "kpp-server\\";
#else
    // (the user's own, not others': its sockets take requests to build and to run what a build runs)
    std::error_code ec;
    return (std::filesystem::temp_directory_path(ec) / ("kpp-server-" + std::to_string(getuid()))).string() + "/";
#endif
  }

  std::string logPath(const std::string& key) { return serverDirectory() + key + ".log"; }

  // KEY.args: what a server builds, its working directory and arguments, each ended by a zero byte,
  // written by the client that starts it; stopServers finds a project's servers by them.
  std::string argumentsPath(const std::string& key) { return serverDirectory() + key + ".args"; }

  // The server's log: a line per event, with the time. Opened and closed per line, so that a crash
  // loses nothing (and the line about the crash is written from the crash handler).
  std::string serverLog;
  void logLine(const char* format, ...)
  {
    if (serverLog.empty())
      return;
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
#ifdef _WIN32
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (FILE* f = fopen(serverLog.c_str(), "ab"))
    {
      fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
              GetCurrentProcessId(), text);
      fclose(f);
    }
#else
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    tm t{};
    localtime_r(&now.tv_sec, &t);
    if (FILE* f = fopen(serverLog.c_str(), "ab"))
    {
      fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%d] %s\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
              int(now.tv_nsec / 1000000), host::processId(), text);
      fclose(f);
    }
#endif
  }

#ifdef _WIN32
  // The stack of a crash, a frame a line: the module and offset (resolvable with the module's map or
  // PDB later) and the function's name when the module has symbols.
  void logStack(CONTEXT* context)
  {
    HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT c = *context;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = c.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = c.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = c.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int n = 0; n < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &c, nullptr, SymFunctionTableAccess64,
                                          SymGetModuleBase64, nullptr) && frame.AddrPC.Offset; ++n)
    {
      DWORD64 address = frame.AddrPC.Offset;
      char module[MAX_PATH] = "?";
      HMODULE handle = nullptr;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &handle))
        GetModuleFileNameA(handle, module, sizeof module);
      const char* base = strrchr(module, '\\');
      char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {};
      auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = 255;
      DWORD64 displacement = 0;
      bool named = SymFromAddr(process, address, &displacement, symbol);
      logLine("  %s+0x%llx %s", base ? base + 1 : module, (unsigned long long)(address - DWORD64(handle)), named ? symbol->Name : "");
    }
  }
#endif

  // The last line of a log, for a client whose server ended.
  std::string lastLogLine(const std::string& path)
  {
    std::string text;
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
      char buffer[1 << 16];
      fseek(f, 0, SEEK_END);
      long size = ftell(f);
      fseek(f, std::max(0L, size - long(sizeof buffer)), SEEK_SET);
      size_t n = fread(buffer, 1, sizeof buffer, f);
      fclose(f);
      text.assign(buffer, n);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.pop_back();
    size_t start = text.rfind('\n');
    return start == std::string::npos ? text : text.substr(start + 1);
  }

#ifdef _WIN32
  bool writeAll(HANDLE h, const void* data, size_t size)
  {
    const char* p = static_cast<const char*>(data);
    while (size)
    {
      DWORD n = 0;
      if (!WriteFile(h, p, DWORD(std::min<size_t>(size, 1 << 20)), &n, nullptr) || n == 0)
        return false;
      p += n;
      size -= n;
    }
    return true;
  }

  bool readAll(HANDLE h, void* data, size_t size)
  {
    char* p = static_cast<char*>(data);
    while (size)
    {
      DWORD n = 0;
      if (!ReadFile(h, p, DWORD(std::min<size_t>(size, 1 << 20)), &n, nullptr) || n == 0)
        return false;
      p += n;
      size -= n;
    }
    return true;
  }

  void closeChannel(Channel c) { CloseHandle(c); }
  int lastError() { return int(GetLastError()); }
#else
  // (MSG_NOSIGNAL: a client gone is a failed write, not SIGPIPE's end of the server)
  bool writeAll(int fd, const void* data, size_t size)
  {
    const char* p = static_cast<const char*>(data);
    while (size)
    {
      ssize_t n = send(fd, p, std::min<size_t>(size, 1 << 20), MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      p += n;
      size -= size_t(n);
    }
    return true;
  }

  bool readAll(int fd, void* data, size_t size)
  {
    char* p = static_cast<char*>(data);
    while (size)
    {
      ssize_t n = read(fd, p, std::min<size_t>(size, 1 << 20));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      p += n;
      size -= size_t(n);
    }
    return true;
  }

  void closeChannel(Channel c) { close(c); }
  int lastError() { return errno; }
#endif

  void putU32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
  void putString(std::string& out, const std::string& s)
  {
    putU32(out, uint32_t(s.size()));
    out += s;
  }
  bool readU32(Channel h, uint32_t& v) { return readAll(h, &v, 4); }
  bool readString(Channel h, std::string& s)
  {
    uint32_t n = 0;
    if (!readU32(h, n) || n > (1u << 28))
      return false;
    s.resize(n);
    return n == 0 || readAll(h, s.data(), n);
  }

#ifdef _WIN32
  std::string modulePath()
  {
    char path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameA(nullptr, path, DWORD(sizeof path));
    return std::string(path, n);
  }

  std::string currentDirectory()
  {
    char path[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryA(DWORD(sizeof path), path);
    return std::string(path, n);
  }
#else
  std::string modulePath() { return host::executablePath().string(); }

  std::string currentDirectory()
  {
    std::error_code ec;
    return std::filesystem::current_path(ec).string();
  }
#endif

  // The key of a server: the kpp binary (path, size and time: a rebuilt kpp is another server),
  // the working directory and the arguments. A server only ever builds its own key's arguments,
  // so a changed flag or input list goes to another server, never to one holding older state.
  // --msvc-diagnostics is not in it: it changes no build, only the form the client prints a
  // diagnostic's place in (renderDiagnostics, source.hpp), so Visual Studio's builds and a terminal's
  // share one server and its kept build. Nor is --packages, a listing, whose build the server never
  // keeps or replays (main.cpp's compile).
  // A file's size and time of writing (or that it is missing): what tells a rebuilt or reinstalled
  // kpp from the one before, a changed output of a kept build from the one it left.
  std::string stampOf(const std::string& path)
  {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
      return "missing";
    return std::to_string(a.nFileSizeLow) + " " + std::to_string(a.nFileSizeHigh) + " " + std::to_string(a.ftLastWriteTime.dwLowDateTime) + " " +
           std::to_string(a.ftLastWriteTime.dwHighDateTime);
#else
    struct stat a{};
    if (stat(path.c_str(), &a) != 0)
      return "missing";
    return std::to_string(a.st_size) + " " + std::to_string(a.st_mtim.tv_sec) + " " + std::to_string(a.st_mtim.tv_nsec);
#endif
  }

  std::string serverKey(const std::string& exe, const std::string& cwd, const std::vector<std::string>& args)
  {
    std::string text = "kpp server 1\n" + exe + "\n" + stampOf(exe) + "\n";
    for (char c : cwd) // (Windows' file names ignore case; POSIX's do not: two directories there)
      text += host::pathsIgnoreCase ? char(tolower(static_cast<unsigned char>(c))) : c;
    for (const std::string& arg : args)
      if (arg != "--verify-incremental" && arg != "--msvc-diagnostics" && arg != "--packages") // a check of the server's build; the client's form; a listing
        text += std::string(1, '\0') + arg;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text)
      h = (h ^ c) * 1099511628211ull;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    return hex;
  }

#ifdef _WIN32
  std::string pipeName(const std::string& key) { return "\\\\.\\pipe\\kpp-server-" + key; }

  // A Windows error in words, on one line.
  std::string errorText(DWORD error)
  {
    char text[256] = "";
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, text, sizeof text, nullptr);
    while (n > 0 && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == '.'))
      text[--n] = 0;
    return "error " + std::to_string(error) + (n ? ", " + std::string(text) : std::string());
  }

  // Connects to the server's pipe, waiting while another client is being served. why, when given: the
  // reason it could not. server, when given: the process just started for it, whose early end (other
  // than losing the key to another server, exit 1) is the answer without waiting for the pipe.
  HANDLE connectPipe(const std::string& name, DWORD waitMs, std::string* why = nullptr, HANDLE server = nullptr)
  {
    auto start = std::chrono::steady_clock::now();
    for (;;)
    {
      HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE)
        return h;
      DWORD error = GetLastError();
      if (error == ERROR_PIPE_BUSY)
      {
        WaitNamedPipeA(name.c_str(), NMPWAIT_WAIT_FOREVER); // a build is running: as long as it takes
        continue;
      }
      if (error == ERROR_ACCESS_DENIED && waitMs != 0)
      {
        // (no wait makes this one go away)
        if (why)
          *why = "access to the pipe " + name + " is denied: a server for these arguments runs as another user or elevated (a Visual Studio run as administrator?), and only it may use the pipe";
        return INVALID_HANDLE_VALUE;
      }
      if (server && WaitForSingleObject(server, 0) == WAIT_OBJECT_0)
      {
        DWORD code = 0;
        GetExitCodeProcess(server, &code);
        if (code != 1)
        {
          if (why)
          {
            char text[64];
            snprintf(text, sizeof text, "0x%lX", (unsigned long)code);
            *why = std::string("the server ended as it started, with exit code ") + text;
          }
          return INVALID_HANDLE_VALUE;
        }
        server = nullptr;
      }
      if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(waitMs))
      {
        if (why)
          *why = "the pipe " + name + " could not be opened in " + std::to_string(waitMs / 1000) + " s (" + errorText(error) + "): the server did not create it";
        return INVALID_HANDLE_VALUE;
      }
      Sleep(20);
    }
  }

  // The server directory stays bounded: every key had its copy of kpp (KEY.exe, 42 MB with LLVM's
  // back end), and every rebuilt kpp and every set of arguments is a key, so nothing ever went: 4,044
  // copies, 53.3 GB, and 6,300 other files on 2026-11-04. Each server start removes what is more than
  // ten minutes old and in no one's use: a copy whose server has ended (a running server's copy is its
  // image, which Windows does not delete), then the files of a key whose copy is gone (KEY.log,
  // KEY.args) and what a verify or a copy left (verify.PID.out and .err, KEY.exe.PID). A copy that a
  // client is about to start is held open against deletion (startServer), and keep's files stay.
  void pruneServerDirectory(const std::string& dir, const std::string& keep)
  {
    const uint64_t margin = 10ull * 60 * 10000000; // ten minutes, in FILETIME's 100 ns
    FILETIME nowTime;
    GetSystemTimeAsFileTime(&nowTime);
    auto value = [](const FILETIME& t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
    uint64_t now = value(nowTime);
    std::vector<std::string> exes, others;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA((dir + "*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
      return;
    do
    {
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        continue;
      std::string name = found.cFileName;
      std::string stem = name.substr(0, name.find('.'));
      // (A copy's time of writing is its kpp's; when it was made is its creation.)
      if (stem == keep || now - std::max(value(found.ftCreationTime), value(found.ftLastWriteTime)) < margin)
        continue;
      bool exe = name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0;
      (exe ? exes : others).push_back(name);
    } while (FindNextFileA(search, &found));
    FindClose(search);
    for (const std::string& name : exes)
      DeleteFileA((dir + name).c_str());
    for (const std::string& name : others)
    {
      std::string stem = name.substr(0, name.find('.'));
      if (GetFileAttributesA((dir + stem + ".exe").c_str()) == INVALID_FILE_ATTRIBUTES) // (its server runs otherwise)
        DeleteFileA((dir + name).c_str());
    }
  }

  // Starts a server for the key. It runs from a copy of kpp in the temp directory, so that kpp
  // itself can be rebuilt while a server runs (Windows keeps a running executable locked).
  // why: the reason it could not start one. process: the server's, to be closed by the caller.
  bool startServer(const std::string& exe, const std::string& key, const std::string& cwd, std::string& why, HANDLE& process)
  {
    std::string dir = serverDirectory();
    CreateDirectoryA(dir.c_str(), nullptr);
    pruneServerDirectory(dir, key);
    std::string copy = dir + key + ".exe";
    // The copy is held open, without sharing its deletion, until its server runs from it: another
    // client's pruneServerDirectory cannot delete it in between.
    auto hold = [&] { return CreateFileA(copy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); };
    HANDLE held = hold();
    if (held == INVALID_HANDLE_VALUE)
    {
      std::string part = copy + "." + std::to_string(GetCurrentProcessId());
      if (!CopyFileA(exe.c_str(), part.c_str(), FALSE))
      {
        why = "kpp could not be copied to " + part + " (" + errorText(GetLastError()) + ")";
        return false;
      }
      if (!MoveFileA(part.c_str(), copy.c_str()))
        DeleteFileA(part.c_str()); // another client put it there first
      held = hold();
    }
    // The kpp it was started from and that kpp's stamp, as the key has it: the server exits once
    // they differ (runServer).
    std::string command = "\"" + copy + "\" --serve " + key + " \"" + exe + "\" \"" + stampOf(exe) + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Out of the client's job if it may leave it: whatever waits for the client's process tree
    // (PowerShell's Start-Process -Wait) must not wait for the server too. A console of its own
    // without a window, not none (DETACHED_PROCESS): every process a build starts through
    // system() (cmd, clang, the linker) then shares it, where without one each was given a new
    // console, about 30 ms apiece (a small program's edit: clang 61 to 69 ms instead of 35 to 39,
    // the link 102 to 107 instead of 70 to 74).
    const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    bool started = CreateProcessA(copy.c_str(), command.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, cwd.c_str(), &si, &pi) ||
                   CreateProcessA(copy.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, cwd.c_str(), &si, &pi);
    DWORD startError = GetLastError();
    if (held != INVALID_HANDLE_VALUE)
      CloseHandle(held);
    if (!started)
    {
      why = copy + " could not be started (" + errorText(startError) + ")";
      return false;
    }
    CloseHandle(pi.hThread);
    process = pi.hProcess;
    return true;
  }
#else
  // KEY.sock: the server's socket. KEY.lock: held (flock, exclusive) by the key's server for as long
  // as it runs: the one server of a key, as Windows' first pipe instance is, and how a stop or a prune
  // tells a running server's files from an ended one's. KEY.starting: held by the client that starts
  // the server until it answers, so that a stop that comes meanwhile waits for it (stopKey).
  std::string pipeName(const std::string& key) { return serverDirectory() + key + ".sock"; }
  std::string lockPath(const std::string& key) { return serverDirectory() + key + ".lock"; }
  std::string startingPath(const std::string& key) { return serverDirectory() + key + ".starting"; }

  // Whether someone holds path's lock (a server running, a client starting one).
  bool lockHeld(const std::string& path)
  {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return false;
    bool held = flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    close(fd);
    return held;
  }

  // Connects to the server's socket. A busy server accepts the next client when its build is done:
  // connect() is queued till then (listen's backlog), as Windows' WaitNamedPipe waits.
  int connectPipe(const std::string& name, int waitMs, std::string* why = nullptr)
  {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (name.size() >= sizeof address.sun_path)
      return -1;
    memcpy(address.sun_path, name.c_str(), name.size() + 1);
    auto start = std::chrono::steady_clock::now();
    for (;;)
    {
      int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0)
        return -1;
      if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0)
        return fd;
      close(fd);
      if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(waitMs))
        return -1;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  // What a prune removes (Windows' pruneServerDirectory): a key's files more than ten minutes old whose
  // server has ended (its KEY.lock held by no one), and what a verify left (verify.PID.*); keep's stay.
  void pruneServerDirectory(const std::string& dir, const std::string& keep)
  {
    std::error_code ec;
    auto now = std::filesystem::file_time_type::clock::now();
    std::vector<std::filesystem::path> gone;
    for (std::filesystem::directory_iterator i(dir, ec), end; !ec && i != end; i.increment(ec))
    {
      std::string name = i->path().filename().string(), stem = name.substr(0, name.find('.'));
      std::error_code e2;
      if (stem == keep || now - i->last_write_time(e2) < std::chrono::minutes(10) || e2)
        continue;
      if (stem != "verify" && lockHeld(lockPath(stem)))
        continue;
      gone.push_back(i->path());
    }
    for (const std::filesystem::path& path : gone)
      std::filesystem::remove(path, ec);
  }

  // Starts a server for the key: kpp itself, in a session of its own (no copy: a running executable
  // can be replaced on POSIX), its streams the null device. The client holds KEY.starting meanwhile.
  bool startServer(const std::string& exe, const std::string& key, const std::string& cwd, std::string& why)
  {
    std::string dir = serverDirectory();
    mkdir(dir.c_str(), 0700);
    pruneServerDirectory(dir, key);
    std::vector<std::string> argv{exe, "--serve", key, exe, stampOf(exe)};
    std::vector<char*> args;
    for (std::string& a : argv)
      args.push_back(a.data());
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd < 3; ++fd)
      posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
    posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    pid_t pid = 0;
    int error = posix_spawn(&pid, exe.c_str(), &actions, &attributes, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (error != 0)
    {
      why = exe + " could not be started (" + strerror(error) + ")";
      return false;
    }
    // (reaped by a thread of its own: the client may live on, and the server's end is no zombie)
    std::thread([pid] { int status = 0; waitpid(pid, &status, 0); }).detach();
    return true;
  }

  // The crash report's stack (Windows' logStack): a frame a line, its module and offset (resolvable
  // with addr2line later) and the function's name, demangled, where the module exports it (kpp is
  // linked with -rdynamic on POSIX). On an alternate stack each thread sets up (useCrashStack), so
  // that an overflowing build's stack is reported too.
  void useCrashStack()
  {
    static thread_local std::vector<char> alternate(size_t(1) << 16);
    stack_t s{};
    s.ss_sp = alternate.data();
    s.ss_size = alternate.size();
    sigaltstack(&s, nullptr);
  }

#endif

}

// A crash on purpose, for the test of the crash report (a request whose environment has
// KPP_SERVER_CRASH_TEST=1): a write through a null pointer in a function of its own, which the
// logged stack names (outside the anonymous namespace: its name is exported, -rdynamic).
[[gnu::noinline]] void kppServerCrashForTest()
{
  volatile int* nowhere = nullptr;
  *nowhere = 1;
}

namespace
{

#ifndef _WIN32
  // Waits up to ms for process pid to end (another process's: polled); true when it has.
  bool waitForEnd(pid_t pid, int ms)
  {
    auto start = std::chrono::steady_clock::now();
    while (kill(pid, 0) == 0 || errno != ESRCH)
    {
      if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms))
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
  }
#endif

  std::string requestHeader(uint32_t kind)
  {
    std::string out = "KPS1";
    putU32(out, kind);
    return out;
  }

  // What the server is doing for its current client: the pipe, and the build's stdout and stderr
  // redirected into pipes whose contents two threads forward as frames.
  struct Session
  {
#ifdef _WIN32
    ~Session() { CloseHandle(answeredEvent); }

    HANDLE client = INVALID_HANDLE_VALUE;
    std::mutex writeLock;
    HANDLE outRead = nullptr, errRead = nullptr;
#else
    int client = -1;
    std::mutex writeLock;
    int outRead = -1, errRead = -1;
    // answer()'s signal and the build thread's end, which runBuild waits for, whichever comes first;
    // shared with the thread, which may end after this session (a build's freeing goes on).
    struct Done
    {
      std::mutex mutex;
      std::condition_variable signal;
      bool answered = false;
    };
    std::shared_ptr<Done> done = std::make_shared<Done>();
#endif
    std::thread outForward, errForward;
    bool answered = false;
    bool clientLost = false; // a write to the client failed: it went away (the build goes on, unseen)
#ifdef _WIN32
    HANDLE answeredEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr); // set by answer()
#endif
    int exitCode = 0;
    std::string captured[2];     // what the build printed (stdout, stderr), kept for a replay (step 3a)
    bool muted = false;          // --verify-incremental's own build: captured, not sent
    bool verifying = false;      // that build is running: its end does not answer the client
    bool replayed = false;       // this request was answered from the kept build
    bool incremental = false;    // built from the kept build, its changed bodies checked again (step 3b)
    bool verifyRequested = false; // --verify-incremental: the build's end does not answer, the check does
    bool drained = false;        // its output is all captured (the forwarders have ended)
    bool keep = false;           // this build's result is kept (replayBuild found nothing to replay)
    bool local = false;          // a build of this process's own (buildInThisProcess): no client
    std::vector<std::string> notes; // its logBuildNote lines (a local build has no log of its own)
    uint64_t fingerprint = 0;
    std::vector<std::string> outputs;
    std::function<std::vector<std::string>()> left; // the files the build leaves (replayBuild's)
  };
  Session* session = nullptr;
  size_t builds = 0;
  std::atomic<bool> busy{false};
  std::atomic<int64_t> lastActivity{0};

  int64_t nowSeconds()
  {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  bool sendFrame(Session& s, uint8_t type, const void* data, uint32_t size)
  {
    std::lock_guard lock(s.writeLock);
    if (s.local)
      return true;
    if (s.clientLost)
      return false;
    char head[5];
    head[0] = char(type);
    memcpy(head + 1, &size, 4);
    if (writeAll(s.client, head, 5) && (size == 0 || writeAll(s.client, data, size)))
      return true;
    s.clientLost = true;
    logLine("build %zu: the client went away (write failed, error %lu)", builds, (unsigned long)lastError());
    return false;
  }

#ifdef _WIN32
  void forward(Session& s, HANDLE from, uint8_t type)
  {
    char buffer[1 << 16];
    for (;;)
    {
      DWORD n = 0;
      if (!ReadFile(from, buffer, DWORD(sizeof buffer), &n, nullptr) || n == 0)
        return;
#else
  void forward(Session& s, int from, uint8_t type)
  {
    char buffer[1 << 16];
    for (;;)
    {
      ssize_t n = read(from, buffer, sizeof buffer);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return;
#endif
      {
        std::lock_guard lock(s.writeLock);
        s.captured[type == stderrFrame].append(buffer, n);
        if (s.muted)
          continue;
      }
      sendFrame(s, type, buffer, n);
    }
  }

  // Points stdout at descriptor out and stderr at err: the streams' own descriptors, descriptors 1
  // and 2 (which the CRT hands to the processes system() starts) and the process's standard
  // handles (which those processes inherit).
#ifdef _WIN32
  void pointStreams(int out, int err)
  {
    int outFd = _fileno(stdout), errFd = _fileno(stderr);
    _dup2(out, outFd);
    _dup2(err, errFd);
    if (outFd != 1 && errFd != 1)
      _dup2(out, 1);
    if (outFd != 2 && errFd != 2)
      _dup2(err, 2);
    SetStdHandle(STD_OUTPUT_HANDLE, HANDLE(_get_osfhandle(outFd)));
    SetStdHandle(STD_ERROR_HANDLE, HANDLE(_get_osfhandle(errFd)));
  }
  int openNull() { return _open("NUL", _O_WRONLY | _O_BINARY); }
#else
  // (descriptors 1 and 2: stdout's and stderr's, and what the processes system() starts inherit)
  void pointStreams(int out, int err)
  {
    dup2(out, 1);
    dup2(err, 2);
  }
  int openNull() { return open("/dev/null", O_WRONLY | O_CLOEXEC); }
#endif

  // Points the process's stdout and stderr (the CRT's descriptors 1 and 2 and the handles child
  // processes inherit, clang's included) at new pipes, and starts forwarding them.
  bool redirectOutput(Session& s)
  {
#ifndef _WIN32
    int outPipe[2], errPipe[2];
    if (pipe2(outPipe, O_CLOEXEC) != 0)
      return false;
    if (pipe2(errPipe, O_CLOEXEC) != 0)
    {
      close(outPipe[0]), close(outPipe[1]);
      return false;
    }
    s.outRead = outPipe[0], s.errRead = errPipe[0];
    pointStreams(outPipe[1], errPipe[1]); // (dup2's copies are inherited; the pipes' own ends are not)
    close(outPipe[1]);
    close(errPipe[1]);
    s.outForward = std::thread(forward, std::ref(s), s.outRead, stdoutFrame);
    s.errForward = std::thread(forward, std::ref(s), s.errRead, stderrFrame);
    return true;
#else
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE outWrite = nullptr, errWrite = nullptr;
    if (!CreatePipe(&s.outRead, &outWrite, &inherit, 1 << 16) || !CreatePipe(&s.errRead, &errWrite, &inherit, 1 << 16))
      return false;
    SetHandleInformation(s.outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(s.errRead, HANDLE_FLAG_INHERIT, 0);
    int out = _open_osfhandle(intptr_t(outWrite), _O_BINARY | _O_WRONLY);
    int err = _open_osfhandle(intptr_t(errWrite), _O_BINARY | _O_WRONLY);
    pointStreams(out, err);
    _close(out);
    _close(err);
    s.outForward = std::thread(forward, std::ref(s), s.outRead, stdoutFrame);
    s.errForward = std::thread(forward, std::ref(s), s.errRead, stderrFrame);
    return true;
#endif
  }

  // The previous build of this server's arguments, for a request whose inputs are as they were
  // (step 3a): what it printed, its exit code, and the stamps of the files it wrote.
  struct KeptBuild
  {
    bool valid = false;
    uint64_t fingerprint = 0;
    std::string out, err;
    int exitCode = 0;
    std::vector<std::string> outputs;
    std::vector<std::string> left, stamps; // every file it left, and each one's stamp then
  };
  KeptBuild kept;
  bool replayOff = false; // --verify-incremental's build from scratch

  void keepBuild(Session& s, int exitCode)
  {
    kept.valid = true;
    kept.fingerprint = s.fingerprint;
    kept.out = s.captured[0];
    kept.err = s.captured[1];
    kept.exitCode = exitCode;
    kept.outputs = s.outputs;
    kept.left = s.left ? s.left() : s.outputs;
    kept.stamps.clear();
    for (const std::string& o : kept.left)
      kept.stamps.push_back(stampOf(o));
  }

  // Ends the forwarding of the build's output once all of it is through (captured and sent).
  void drain(Session& s)
  {
    if (s.drained)
      return;
    s.drained = true;
    fflush(stdout);
    fflush(stderr);
    int nul = openNull();
    pointStreams(nul, nul);
#ifdef _WIN32
    _close(nul);
#else
    close(nul);
#endif
    // The pipes' last write ends are closed now (clang's closed with clang): the forwarders end.
    if (s.outForward.joinable())
      s.outForward.join();
    if (s.errForward.joinable())
      s.errForward.join();
#ifdef _WIN32
    CloseHandle(s.outRead);
    CloseHandle(s.errRead);
#else
    close(s.outRead);
    close(s.errRead);
#endif
  }

  // Sends the build's exit code once everything it printed is through, and lets the client go.
  void answer(Session& s, int exitCode)
  {
    if (s.answered)
      return;
    s.answered = true;
    drain(s);
    if (s.keep)
      keepBuild(s, exitCode);
    sendFrame(s, exitFrame, &exitCode, 4);
#ifdef _WIN32
    if (!s.local)
    {
      FlushFileBuffers(s.client);
      DisconnectNamedPipe(s.client);
    }
    s.exitCode = exitCode;
    SetEvent(s.answeredEvent);
#else
    if (!s.local)
      shutdown(s.client, SHUT_RDWR); // (the client has its exit code; the socket closes once served)
    std::lock_guard lock(s.done->mutex);
    s.exitCode = exitCode;
    s.done->answered = true;
    s.done->signal.notify_all();
#endif
  }

  // The client's environment for the build (TMP, which holds the C import cache, LIB and INCLUDE
  // for clang's link, KPP_CLANG...): what the server has and the client has not is removed.
#ifndef _WIN32
  // POSIX: the block is NAME=VALUE entries, each ended by a zero byte (clientEnvironment).
  void applyEnvironment(const std::string& block)
  {
    std::vector<std::string> wanted;
    for (size_t at = 0, end; at < block.size() && (end = block.find('\0', at)) != std::string::npos; at = end + 1)
      if (end > at && block[at] != '=')
        wanted.push_back(block.substr(at, end - at));
    std::vector<std::string> present;
    for (char** e = environ; e && *e; ++e)
      present.push_back(std::string(*e).substr(0, std::string(*e).find('=')));
    for (const std::string& name : present)
    {
      bool kept = false;
      for (const std::string& w : wanted)
        kept = kept || w.compare(0, w.find('='), name) == 0;
      if (!kept)
        unsetenv(name.c_str());
    }
    for (const std::string& entry : wanted)
    {
      size_t eq = entry.find('=');
      if (eq != std::string::npos)
        setenv(entry.substr(0, eq).c_str(), entry.c_str() + eq + 1, 1);
    }
  }

  // This process's environment as a request carries it.
  std::string clientEnvironment()
  {
    std::string out;
    for (char** e = environ; e && *e; ++e)
      out.append(*e).push_back('\0');
    return out;
  }
#else
  void applyEnvironment(const std::string& block)
  {
    std::vector<std::wstring> wanted;
    const wchar_t* p = reinterpret_cast<const wchar_t*>(block.data());
    const wchar_t* end = p + block.size() / sizeof(wchar_t);
    while (p < end && *p)
    {
      std::wstring entry(p);
      p += entry.size() + 1;
      if (!entry.empty() && entry[0] != L'=')
        wanted.push_back(entry);
    }
    std::vector<std::wstring> present;
    if (wchar_t* own = GetEnvironmentStringsW())
    {
      for (const wchar_t* q = own; *q; q += wcslen(q) + 1)
        if (*q != L'=')
          present.push_back(q);
      FreeEnvironmentStringsW(own);
    }
    auto nameOf = [](const std::wstring& entry) { return entry.substr(0, entry.find(L'=')); };
    for (const std::wstring& entry : present)
    {
      std::wstring name = nameOf(entry);
      bool kept = false;
      for (const std::wstring& w : wanted)
        kept = kept || _wcsicmp(nameOf(w).c_str(), name.c_str()) == 0;
      if (!kept)
      {
        _wputenv_s(name.c_str(), L"");
        SetEnvironmentVariableW(name.c_str(), nullptr);
      }
    }
    for (const std::wstring& entry : wanted)
    {
      size_t eq = entry.find(L'=');
      if (eq == std::wstring::npos)
        continue;
      std::wstring name = entry.substr(0, eq), value = entry.substr(eq + 1);
      _wputenv_s(name.c_str(), value.c_str());
      SetEnvironmentVariableW(name.c_str(), value.c_str());
    }
  }

  std::string clientEnvironment()
  {
    std::string environment;
    if (wchar_t* block = GetEnvironmentStringsW())
    {
      const wchar_t* q = block;
      while (*q)
        q += wcslen(q) + 1;
      environment.assign(reinterpret_cast<const char*>(block), (q - block + 1) * sizeof(wchar_t));
      FreeEnvironmentStringsW(block);
    }
    return environment;
  }
#endif

  struct BuildJob
  {
    CompileFunction compile;
    std::vector<std::string> args;
    int result = 1;
    std::atomic<bool> released{false};
  };

  // One build, on a thread of its own: thread-local state (the resolver's, the evaluator's) starts
  // fresh as in a new process, and the stack is the compiler's (host::compilerStack), as every thread's.
  // When the build has answered its client (buildDone), what is left is freeing what it built
  // (about 10 s on the full game): the next request is served meanwhile, the thread finishing on
  // its own. Whoever of the two lets go of the job last deletes it.
#ifndef _WIN32
  // POSIX: a pthread of the compiler's stack; the build's end and answer() each signal the session.
  int runBuild(CompileFunction compile, std::vector<std::string> args, Session& s)
  {
    struct Run
    {
      BuildJob* job;
      std::shared_ptr<Session::Done> done;
      std::shared_ptr<bool> finished; // (this run's own: --verify-incremental runs a second build in the session)
    };
    BuildJob* job = new BuildJob{compile, std::move(args)};
    auto finished = std::make_shared<bool>(false);
    auto* run = new Run{job, s.done, finished};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, host::compilerStack);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int error = pthread_create(&thread, &attributes, [](void* p) -> void*
    {
      Run* r = static_cast<Run*>(p);
      useCrashStack(); // (its own: a build's stack overflow is reported)
      BuildJob* j = r->job;
      std::vector<char*> argv;
      for (std::string& a : j->args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      j->result = j->compile(int(argv.size() - 1), argv.data());
      {
        std::lock_guard lock(r->done->mutex);
        *r->finished = true;
        r->done->signal.notify_all();
      }
      if (j->released.exchange(true))
        delete j;
      delete r;
      return nullptr;
    }, run);
    pthread_attr_destroy(&attributes);
    if (error != 0)
    {
      delete run;
      delete job;
      return 1;
    }
    int result;
    {
      std::unique_lock lock(s.done->mutex);
      s.done->signal.wait(lock, [&] { return *finished || (!s.verifying && s.done->answered); });
      result = *finished ? job->result : s.exitCode;
    }
    if (job->released.exchange(true))
      delete job;
    return result;
  }
#else
  int runBuild(CompileFunction compile, std::vector<std::string> args, Session& s)
  {
    BuildJob* job = new BuildJob{compile, std::move(args)};
    HANDLE thread = CreateThread(nullptr, host::compilerStack, [](void* p) -> DWORD
    {
      BuildJob* j = static_cast<BuildJob*>(p);
      std::vector<char*> argv;
      for (std::string& a : j->args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      j->result = j->compile(int(argv.size() - 1), argv.data());
      if (j->released.exchange(true))
        delete j;
      return 0;
    }, job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread)
    {
      delete job;
      return 1;
    }
    HANDLE both[2] = {thread, s.answeredEvent};
    DWORD which = WaitForMultipleObjects(s.verifying ? 1 : 2, both, FALSE, INFINITE);
    int result = which == WAIT_OBJECT_0 ? job->result : s.exitCode;
    CloseHandle(thread);
    if (job->released.exchange(true))
      delete job;
    return result;
  }
#endif

  // What a build printed, without what differs from run to run by design (timings and rates, wait
  // counts, the interned types' count, whether the C import came from its cache, how many objects the
  // back end made while codegen ran, the AST's memory: a kept file reports the bytes of its parse, a
  // few more or less than a fresh one's, and the sum then rounds the other way).
  std::string normalized(const std::string& text)
  {
    static const std::regex seconds("[0-9]+\\.[0-9]+ s\\b"), rates("\\([0-9.]+ [MG]?B/s[^)]*\\)"), lines("\\([0-9.]+ M lines/s\\)"),
      waits("\\([0-9]+ requests, [0-9]+ waits\\)"), unchanged("[0-9]+ unchanged"), during(", [0-9]+ during codegen"), types("^check: +[0-9]+ distinct types"),
      cached(" \\(cached\\)$"), astMemory("AST memory: [0-9.]+ MB");
    std::string out;
    size_t start = 0;
    while (start < text.size())
    {
      size_t end = text.find('\n', start);
      std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
      start = end == std::string::npos ? text.size() : end + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      line = std::regex_replace(line, seconds, "_ s");
      line = std::regex_replace(line, rates, "(_)");
      line = std::regex_replace(line, lines, "(_)");
      line = std::regex_replace(line, waits, "(_)");
      line = std::regex_replace(line, unchanged, "_ unchanged"); // the object cache's state, not the build's
      line = std::regex_replace(line, during, ""); // (step 4's P2; a server's incremental build streams none)
      line = std::regex_replace(line, types, "check:");
      line = std::regex_replace(line, cached, "");
      line = std::regex_replace(line, astMemory, "AST memory: _ MB");
      out += line + "\n";
    }
    return out;
  }

  // What a build wrote, for comparing two builds: an IR file's bytes, an executable's modules'
  // hashes (the executable itself carries the link's time).
  std::string writtenContents(const std::vector<std::string>& outputs)
  {
    std::string out;
    auto read = [](const std::string& path, std::string& into)
    {
      FILE* f = fopen(path.c_str(), "rb");
      if (!f)
        return false;
      char buffer[1 << 16];
      for (size_t n; (n = fread(buffer, 1, sizeof buffer, f)) > 0;)
        into.append(buffer, n);
      fclose(f);
      return true;
    };
    for (const std::string& o : outputs)
    {
      out += o + "\n";
      if (o.size() > 3 && o.compare(o.size() - 3, 3, ".ll") == 0)
        read(o, out);
      else
        for (int k = 0; read(o + "." + std::to_string(k) + ".hash", out); ++k)
          out += "\n";
    }
    return out;
  }

  // The first line two texts differ in, for the report.
  std::string firstDifference(const std::string& a, const std::string& b)
  {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i])
      ++i;
    size_t from = a.rfind('\n', i == 0 ? 0 : i - 1);
    from = from == std::string::npos ? 0 : from + 1;
    auto lineAt = [&](const std::string& t) { size_t e = t.find('\n', from); return t.substr(from, e == std::string::npos ? std::string::npos : e - from); };
    return "kept: \"" + lineAt(a) + "\", from scratch: \"" + lineAt(b) + "\"";
  }

  // --verify-incremental after a replayed build: builds the same arguments from scratch in this
  // server (its output into files, not to the client) and compares what both printed and wrote.
  bool verifyReplay(Session& s, CompileFunction compile, std::vector<std::string> args, int builtExit)
  {
    // What the request's build printed: the kept build's when it was replayed, else its own (drained).
    const std::string& printedOut = s.replayed ? kept.out : s.captured[0];
    const std::string& printedErr = s.replayed ? kept.err : s.captured[1];
    std::vector<std::string> outputs = s.replayed ? kept.outputs : s.outputs;
    std::string before = writtenContents(outputs);
    fflush(stdout);
    fflush(stderr);
    // (Named by the server's process: other servers verify at the same time, in other threads' builds.)
    std::string own = serverDirectory() + "verify." + std::to_string(host::processId());
    std::string outPath = own + ".out", errPath = own + ".err";
#ifdef _WIN32
    int savedOut = _dup(_fileno(stdout)), savedErr = _dup(_fileno(stderr));
    int out = _open(outPath.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    int err = _open(errPath.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    pointStreams(out, err);
    _close(out);
    _close(err);
#else
    int savedOut = fcntl(1, F_DUPFD_CLOEXEC, 3), savedErr = fcntl(2, F_DUPFD_CLOEXEC, 3);
    int out = open(outPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    int err = open(errPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    pointStreams(out, err);
    close(out);
    close(err);
#endif
    replayOff = true;
    s.verifying = true;
    int fresh = runBuild(compile, std::move(args), s);
    s.verifying = false;
    replayOff = false;
    fflush(stdout);
    fflush(stderr);
    pointStreams(savedOut, savedErr);
#ifdef _WIN32
    _close(savedOut);
    _close(savedErr);
#else
    close(savedOut);
    close(savedErr);
#endif
    auto slurp = [](const std::string& path)
    {
      std::string text;
      if (FILE* f = fopen(path.c_str(), "rb"))
      {
        char buffer[1 << 16];
        for (size_t n; (n = fread(buffer, 1, sizeof buffer, f)) > 0;)
          text.append(buffer, n);
        fclose(f);
      }
      return text;
    };
    std::string problem;
    std::string keptOut = normalized(printedOut), freshOut = normalized(slurp(outPath));
    std::string keptErr = normalized(printedErr), freshErr = normalized(slurp(errPath));
    remove(outPath.c_str()); // (read: 868 pairs were left in the server directory)
    remove(errPath.c_str());
    std::string after = writtenContents(outputs);
    if (fresh != builtExit)
      problem = "exit code " + std::to_string(builtExit) + " kept, " + std::to_string(fresh) + " from scratch";
    else if (keptOut != freshOut)
      problem = "stdout differs: " + firstDifference(keptOut, freshOut);
    else if (keptErr != freshErr)
      problem = "stderr differs: " + firstDifference(keptErr, freshErr);
    else if (before != after)
      problem = "what it wrote differs (" + std::to_string(before.size()) + " and " + std::to_string(after.size()) + " bytes compared)";
    logLine("build %zu: --verify-incremental: %s", builds, problem.empty() ? "the same as from scratch" : problem.c_str());
    if (problem.empty())
      return true;
    std::string message = "kpp: --verify-incremental: the server's build is not what a build from scratch gives: " + problem + "\n";
    sendFrame(s, stderrFrame, message.data(), uint32_t(message.size())); // (the build's streams are drained)
    return false;
  }

  // Serves one client. Returns false when the client asked the server to stop.
  bool serveClient(Channel client, CompileFunction compile)
  {
    Session s;
    s.client = client;
    char magic[4];
    uint32_t kind = 0;
    if (!readAll(client, magic, 4) || memcmp(magic, "KPS1", 4) != 0 || !readU32(client, kind))
    {
      logLine("a client sent no request");
      return true;
    }
    uint32_t self = uint32_t(host::processId());
    sendFrame(s, processFrame, &self, 4); // so that a client that loses the server can wait for it to end
    if (kind == stopRequest)
    {
      logLine("stopped by a client");
      int zero = 0;
      sendFrame(s, exitFrame, &zero, 4);
#ifdef _WIN32
      FlushFileBuffers(client);
#endif
      return false;
    }
    std::string exe, cwd, environment;
    uint32_t argc = 0;
    if (!readString(client, exe) || !readString(client, cwd) || !readU32(client, argc) || argc > 100000)
      return true;
    std::vector<std::string> args{exe};
    for (uint32_t i = 0; i < argc; ++i)
    {
      std::string a;
      if (!readString(client, a))
        return true;
      args.push_back(std::move(a));
    }
    if (!readString(client, environment))
    {
      logLine("a client's request was cut short");
      return true;
    }
    logLine("build %zu: %u arguments, in %s, the last %s", ++builds, argc, cwd.c_str(), args.back().c_str());
    applyEnvironment(environment);
    if (const char* crash = getenv("KPP_SERVER_CRASH_TEST"); crash && strcmp(crash, "1") == 0)
    {
      logLine("build %zu: KPP_SERVER_CRASH_TEST: crashing on purpose", builds);
      kppServerCrashForTest();
    }
#ifdef _WIN32
    SetCurrentDirectoryA(cwd.c_str());
#else
    if (chdir(cwd.c_str()) != 0) // (never a build in another directory)
    {
      logLine("build %zu: cannot enter %s", builds, cwd.c_str());
      std::string message = "kpp server: cannot enter " + cwd + ": " + strerror(errno) + "\n";
      sendFrame(s, stderrFrame, message.data(), uint32_t(message.size()));
      int one = 1;
      sendFrame(s, exitFrame, &one, 4);
      return true;
    }
#endif
    if (!redirectOutput(s))
    {
      const char* message = "kpp server: cannot redirect the build's output\n";
      sendFrame(s, stderrFrame, message, uint32_t(strlen(message)));
      int one = 1;
      sendFrame(s, exitFrame, &one, 4);
      return true;
    }
    session = &s;
    bool verify = std::find(args.begin(), args.end(), "--verify-incremental") != args.end();
    s.verifyRequested = verify;
    std::vector<std::string> again = args;
    int result = runBuild(compile, std::move(args), s);
    if (verify && (s.replayed || s.incremental))
    {
      drain(s);
      if (!verifyReplay(s, compile, std::move(again), result))
        result = 1;
    }
    answer(s, result); // if the build did not answer itself (usage errors, --dump, a replay)
    session = nullptr;
    logLine("build %zu done, exit %d%s", builds, result, s.clientLost ? " (its client went away before the answer)" : "");
    return true;
  }

  std::vector<std::string> argumentsOf(int argc, char** argv)
  {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
      args.push_back(argv[i]);
    return args;
  }
}

bool servingBuild() { return session != nullptr; }

int buildInThisProcess(CompileFunction compile, std::vector<std::string> args, std::string& out, std::string& err, std::vector<std::string>* notes)
{
  Session s;
  s.local = true;
  s.muted = true; // what it prints is captured for the caller, sent to no client
  if (!redirectOutput(s))
  {
    err = "kpp: cannot redirect the build's output\n";
    return 1;
  }
  session = &s;
  ++builds;
  int result = runBuild(compile, std::move(args), s);
  answer(s, result); // if the build did not answer itself (usage errors, a replay)
  session = nullptr;
  out = s.captured[0];
  err = s.captured[1];
  if (notes)
  {
    std::lock_guard lock(s.writeLock);
    *notes = s.notes;
  }
  return result;
}

bool replayDisabled() { return replayOff; }

void noteIncrementalBuild()
{
  if (session)
    session->incremental = true;
}

void logBuildNote(const std::string& note)
{
  if (session && session->local)
  {
    std::lock_guard lock(session->writeLock);
    session->notes.push_back(note);
  }
  else if (session)
    logLine("build %zu: %s", builds, plainDiagnostic(note).c_str()); // (a place in it, marked: as a terminal writes it)
}

// The variables no build reads that change between builds anyway: Visual Studio's own, which it sets
// for each build (VSTEL_CurrentSolutionBuildID) or each devenv (VSTEL_SolutionSessionID,
// ServiceHub*, VsPerMonitorDpiAwarenessEnabled.<pid>). With them in the fingerprint, no build from
// Visual Studio was ever a replay: each one with nothing changed checked and linked again, about 0.5 s
// instead of 0.08 s (thread K on PC-OBYVAK, 2026-11-05). Every other variable counts (TMP, PATH, LIB,
// INCLUDE, KPP_*...): one left out wrongly would make a replay of a build that differs.
#ifdef _WIN32
static bool readByNoBuild(const wchar_t* entry)
{
  auto startsWith = [&](const wchar_t* prefix) { return _wcsnicmp(entry, prefix, wcslen(prefix)) == 0; };
  return startsWith(L"VSTEL_") || startsWith(L"ServiceHub") || startsWith(L"VsPerMonitorDpiAwarenessEnabled");
}

std::string environmentBlock()
{
  std::string out;
  if (wchar_t* block = GetEnvironmentStringsW())
  {
    for (const wchar_t* q = block; *q; q += wcslen(q) + 1)
      if (!readByNoBuild(q))
        out.append(reinterpret_cast<const char*>(q), (wcslen(q) + 1) * sizeof(wchar_t));
    FreeEnvironmentStringsW(block);
  }
  return out;
}
#else
std::string environmentBlock() { return clientEnvironment(); } // (every variable counts)
#endif

bool replayBuild(uint64_t fingerprint, const std::vector<std::string>& outputs, const std::function<std::vector<std::string>()>& left,
                 bool headersUnchanged, int& exitCode)
{
  if (!session || replayOff)
    return false;
  // Every file the kept build left, as it left it: one removed (an output directory deleted under a
  // live server, one module's object) is built again, never reported built (kovarex, 2026-11-02).
  bool same = kept.valid && kept.fingerprint == fingerprint && headersUnchanged && kept.outputs == outputs;
  if (same)
    same = (left ? left() : outputs) == kept.left;
  for (size_t i = 0; same && i < kept.left.size(); ++i)
    same = stampOf(kept.left[i]) == kept.stamps[i];
  if (!same)
  {
    session->keep = true; // this build's result is what the next one compares with
    session->fingerprint = fingerprint;
    session->outputs = outputs;
    session->left = left;
    return false;
  }
  fwrite(kept.out.data(), 1, kept.out.size(), stdout);
  fwrite(kept.err.data(), 1, kept.err.size(), stderr);
  fflush(stdout);
  fflush(stderr);
  session->replayed = true;
  logLine("build %zu: no input changed, the previous build's output sent again", builds);
  exitCode = kept.exitCode;
  return true;
}

void forgetReplay() { kept.valid = false; }

bool leftAsKept(const std::string& path)
{
  if (!kept.valid)
    return false;
  std::string want = std::filesystem::absolute(path).lexically_normal().string();
  for (size_t i = 0; i < kept.left.size(); ++i)
    if (std::filesystem::path(kept.left[i]).lexically_normal().string() == want)
      return stampOf(kept.left[i]) == kept.stamps[i];
  return false;
}

void buildDone(int exitCode)
{
  if (session && (session->verifying || session->verifyRequested))
  {
    fflush(stdout); // --verify-incremental's build from scratch: the request's own build answers
    fflush(stderr);
    return;
  }
  if (session)
  {
    answer(*session, exitCode);
    return;
  }
  fflush(stdout);
  fflush(stderr);
  _exit(exitCode);
}

int runClient(int argc, char** argv, CompileFunction compile)
{
  std::string exe = modulePath(), cwd = currentDirectory();
  std::vector<std::string> args = argumentsOf(argc, argv);
  std::string key = serverKey(exe, cwd, args), name = pipeName(key);
  Channel pipe = connectPipe(name, 0);
  std::string noServer; // why no server could be reached
  if (pipe == noChannel)
  {
    // KEY.args before the server starts: a stop of its project that comes meanwhile finds it (and
    // waits for its pipe, stopKey); it goes again when no server could be started.
    std::string record = cwd + '\0';
    for (const std::string& a : args)
      record += a + '\0';
#ifdef _WIN32
    CreateDirectoryA(serverDirectory().c_str(), nullptr);
#else
    mkdir(serverDirectory().c_str(), 0700);
    // KEY.starting, held until the server answers: a stop meanwhile waits for it (stopKey).
    int starting = open(startingPath(key).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (starting >= 0)
      flock(starting, LOCK_EX);
#endif
    if (FILE* f = fopen(argumentsPath(key).c_str(), "wb"))
      fwrite(record.data(), 1, record.size(), f), fclose(f);
#ifdef _WIN32
    HANDLE started = nullptr;
    if (startServer(exe, key, cwd, noServer, started))
    {
      pipe = connectPipe(name, 20000, &noServer, started);
      CloseHandle(started);
    }
#else
    if (startServer(exe, key, cwd, noServer))
      pipe = connectPipe(name, 20000, &noServer);
#endif
    else
      remove(argumentsPath(key).c_str());
#ifndef _WIN32
    if (starting >= 0)
      close(starting);
#endif
  }
  if (pipe == noChannel)
  {
    fprintf(stderr, "kpp: no compile server could be reached (%s; its log, if it wrote one: %s); building in this process\n", noServer.empty() ? "no reason known" : noServer.c_str(), logPath(key).c_str());
    return compile(argc, argv);
  }
  std::string request = requestHeader(buildRequest);
  putString(request, exe);
  putString(request, cwd);
  putU32(request, uint32_t(args.size()));
  for (const std::string& a : args)
    putString(request, a);
  putString(request, clientEnvironment());
  uint32_t serverProcess = 0;
  // The server marks each diagnostic's place (source.hpp, markDiagnosticLocations): written here in
  // this client's form. A mark that a frame cuts is held until its end comes.
  bool msvc = std::find(args.begin(), args.end(), "--msvc-diagnostics") != args.end();
  std::string held[2]; // stdout's, stderr's
  auto print = [&](int stream, const char* data, size_t size, bool last)
  {
    std::string& h = held[stream];
    h.append(data, size);
    size_t keep = h.size();
    if (!last)
      if (size_t mark = h.rfind('\1'); mark != std::string::npos && h.find('\3', mark) == std::string::npos && h.size() - mark < (1 << 16))
        keep = mark;
    if (keep == 0)
      return;
    std::string text = renderDiagnostics(std::string_view(h).substr(0, keep), msvc);
    FILE* to = stream ? stderr : stdout;
    fwrite(text.data(), 1, text.size(), to);
    fflush(to);
    h.erase(0, keep);
  };
  if (writeAll(pipe, request.data(), request.size()))
  {
    std::string data;
    for (;;)
    {
      char head[5];
      uint32_t size = 0;
      if (!readAll(pipe, head, 5))
        break;
      memcpy(&size, head + 1, 4);
      data.resize(size);
      if (size && !readAll(pipe, data.data(), size))
        break;
      if (head[0] == exitFrame && size == 4)
      {
        int code = 0;
        memcpy(&code, data.data(), 4);
        closeChannel(pipe);
        print(0, nullptr, 0, true);
        print(1, nullptr, 0, true);
        return code;
      }
      if (head[0] == processFrame && size == 4)
      {
        memcpy(&serverProcess, data.data(), 4);
        continue;
      }
      print(head[0] == stderrFrame, data.data(), size, false);
    }
  }
  print(0, nullptr, 0, true); // (what came before the server went)
  print(1, nullptr, 0, true);
  unsigned long readError = (unsigned long)lastError();
  closeChannel(pipe);
  // The server is gone or going: before building here, wait until it has ended (its job ends
  // the clang and link it started with it), so that nothing of its build still writes the outputs;
  // one that does not end is ended.
#ifdef _WIN32
  if (serverProcess)
    if (HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, serverProcess))
    {
      if (WaitForSingleObject(process, 30000) != WAIT_OBJECT_0)
      {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 30000);
      }
      CloseHandle(process);
    }
#else
  if (serverProcess && !waitForEnd(pid_t(serverProcess), 30000))
  {
    kill(pid_t(serverProcess), SIGKILL);
    waitForEnd(pid_t(serverProcess), 30000);
  }
#endif
  std::string why = lastLogLine(logPath(key));
  fprintf(stderr, "kpp: the compile server ended without finishing the build (read error %lu; its log, %s, ends: %s); building in this process\n",
          readError, logPath(key).c_str(), why.empty() ? "(nothing)" : why.c_str());
  return compile(argc, argv);
}

#ifndef _WIN32
static void crashLogged(int sig, siginfo_t* info, void*)
{
  logLine("crashed in build %zu: signal %d (%s) at %p", builds, sig, strsignal(sig), info ? info->si_addr : nullptr);
  void* frames[48];
  int n = backtrace(frames, 48);
  for (int i = 1; i < n; ++i) // (from the handler's caller: the frame that faulted)
  {
    Dl_info where{};
    if (!dladdr(frames[i], &where))
    {
      logLine("  ? %p", frames[i]);
      continue;
    }
    const char* module = where.dli_fname ? strrchr(where.dli_fname, '/') : nullptr;
    int status = 0;
    char* demangled = where.dli_sname ? abi::__cxa_demangle(where.dli_sname, nullptr, nullptr, &status) : nullptr;
    logLine("  %s+0x%llx %s", module ? module + 1 : (where.dli_fname ? where.dli_fname : "?"),
            (unsigned long long)(static_cast<char*>(frames[i]) - static_cast<char*>(where.dli_fbase)),
            demangled ? demangled : where.dli_sname ? where.dli_sname : "");
    free(demangled);
  }
  raise(sig); // (SA_RESETHAND: the default action now, the process ends as it would have)
}

// POSIX: KEY.lock held for the server's life makes it the key's one server; the socket bound after
// it (a stale one from a server that crashed is removed first: the lock says no one serves it).
int runServer(const char* key, const char* origin, const char* stamp, CompileFunction compile)
{
  std::string dir = serverDirectory();
  mkdir(dir.c_str(), 0700);
  int lock = open(lockPath(key).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0)
    return 1; // another server has the key
  std::string name = pipeName(key);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (name.size() >= sizeof address.sun_path)
    return 1;
  memcpy(address.sun_path, name.c_str(), name.size() + 1);
  unlink(name.c_str());
  int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(listener, 64) != 0)
    return 1;
  serverLog = logPath(key);
  logLine("started");
  markDiagnosticLocations(); // each client prints them in its own form
  // Why it ended, for the client that loses it: the signal, the build, the address and the stack
  // (crashLogged; an alternate stack per thread, so that a stack overflow is reported too).
  useCrashStack();
  for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
  {
    struct sigaction action{};
    action.sa_sigaction = crashLogged;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
  }
  signal(SIGPIPE, SIG_IGN); // (a client gone is a failed write)
  setvbuf(stdout, nullptr, _IONBF, 0);
  lastActivity = nowSeconds();
  static std::mutex idleMutex; // the checks and the start of a client's service exclude each other
  // Between builds: exits once idle for an hour, or once the kpp it was started from has changed.
  std::thread([origin = std::string(origin), stamp = std::string(stamp), name]
  {
    for (;;)
    {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::lock_guard lock(idleMutex);
      if (busy)
        continue;
      if (stampOf(origin) != stamp)
      {
        logLine("the kpp it was started from (%s) changed: exiting", origin.c_str());
        unlink(name.c_str());
        _exit(0);
      }
      if (nowSeconds() - lastActivity > idleSeconds)
      {
        logLine("idle for an hour: exiting");
        unlink(name.c_str());
        _exit(0);
      }
    }
  }).detach();
  for (;;)
  {
    int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0)
    {
      if (errno == EINTR)
        continue;
      logLine("accept failed (error %d): exiting", errno);
      _exit(1);
    }
    {
      std::lock_guard lock(idleMutex);
      busy = true;
    }
    bool goOn = serveClient(client, compile);
    close(client);
    lastActivity = nowSeconds();
    busy = false;
    // Stopped: it ends at once (its socket's name first: the next build of the key starts another).
    if (!goOn)
    {
      logLine("exiting");
      unlink(name.c_str());
      _exit(0);
    }
  }
}
#else
int runServer(const char* key, const char* origin, const char* stamp, CompileFunction compile)
{
  std::string name = pipeName(key);
  HANDLE pipe = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE)
  {
    DWORD error = GetLastError();
    serverLog = logPath(key);
    logLine("another process has this key's pipe (%s): this server ends", errorText(error).c_str());
    return 1; // another server has the key
  }
  serverLog = logPath(key);
  logLine("started");
  markDiagnosticLocations(); // each client prints them in its own form
  // The processes a build starts (clang, its link) end with the server, whatever ends it: a client
  // that lost its server must not find the server's link still writing the executable.
  if (HANDLE job = CreateJobObjectA(nullptr, nullptr))
  {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    if (!AssignProcessToJobObject(job, GetCurrentProcess()))
      logLine("cannot put itself in a job (error %lu): a crash may leave a build's clang running", GetLastError());
  }
  // Why it ended, for the client that loses it.
  SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG
  {
    logLine("crashed in build %zu: exception 0x%08lX at %p", builds, e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    logStack(e->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;
  });
  signal(SIGABRT, [](int) { logLine("aborted in build %zu", builds); });
  // A detached process has no console, so its streams have no descriptors: give them some.
  freopen("NUL", "wb", stdout);
  freopen("NUL", "wb", stderr);
  setvbuf(stdout, nullptr, _IONBF, 0);
  lastActivity = nowSeconds();
  static std::mutex idleMutex; // the checks and the start of a client's service exclude each other
  // Between builds: exits once idle for an hour, or once the kpp it was started from has changed (a
  // rebuild, an install), whose clients have another key: nothing would reach this server again.
  std::thread([origin = std::string(origin), stamp = std::string(stamp)]
  {
    for (;;)
    {
      Sleep(2000);
      std::lock_guard lock(idleMutex);
      if (busy)
        continue;
      if (stampOf(origin) != stamp)
      {
        logLine("the kpp it was started from (%s) changed: exiting", origin.c_str());
        ExitProcess(0);
      }
      if (nowSeconds() - lastActivity > idleSeconds)
      {
        logLine("idle for an hour: exiting");
        ExitProcess(0);
      }
    }
  }).detach();
  for (;;)
  {
    bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
    {
      std::lock_guard lock(idleMutex);
      busy = true;
    }
    bool goOn = !connected || serveClient(pipe, compile);
    DisconnectNamedPipe(pipe);
    lastActivity = nowSeconds();
    busy = false;
    // Stopped: it ends at once, its pipe with it. Returning freed the kept program first, minutes
    // for the game's, while the pipe kept its name: the next build of the key found no server to
    // start (the pipe is the first instance's) and waited for this one to end (2026-11-03).
    if (!goOn)
    {
      logLine("exiting");
      CloseHandle(pipe);
      ExitProcess(0);
    }
  }
}

#endif
namespace
{
  // Stops the server of key if one runs (its KEY.args goes with it); false when none does.
  bool stopKey(const std::string& key)
  {
    Channel pipe = connectPipe(pipeName(key), 0);
    // A server still starting (its client wrote KEY.args before starting it, runClient) has no pipe
    // yet: its copy of kpp, KEY.exe, is in use (running, or held by the client that starts it), so
    // that it cannot be opened for deletion. It is waited for as its client waits, never left
    // running unseen (thread H, 2026-11-05: a stop could miss a server that came up a moment later).
#ifdef _WIN32
    if (pipe == INVALID_HANDLE_VALUE)
    {
      HANDLE copy = CreateFileA((serverDirectory() + key + ".exe").c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (copy != INVALID_HANDLE_VALUE)
        CloseHandle(copy);
      else if (GetLastError() == ERROR_SHARING_VIOLATION)
        pipe = connectPipe(pipeName(key), 20000);
    }
#else
    // (POSIX: its KEY.lock held, or KEY.starting by the client that starts it)
    if (pipe == noChannel && (lockHeld(lockPath(key)) || lockHeld(startingPath(key))))
      pipe = connectPipe(pipeName(key), 20000);
#endif
    remove(argumentsPath(key).c_str());
    if (pipe == noChannel)
      return false;
    std::string request = requestHeader(stopRequest);
    writeAll(pipe, request.data(), request.size());
    // Its process id, then the exit code: read up to the exit code, so that the server's last write
    // finds the client still there. Then it waits for the process to end: stopped means gone, and a
    // build right after starts a server of its own.
    uint32_t server = 0;
    for (char frame[9]; readAll(pipe, frame, 9) && frame[0] != char(exitFrame);)
      if (frame[0] == char(processFrame))
        memcpy(&server, frame + 5, 4);
    closeChannel(pipe);
#ifdef _WIN32
    if (HANDLE process = server ? OpenProcess(SYNCHRONIZE, FALSE, server) : nullptr)
    {
      WaitForSingleObject(process, 60000);
      CloseHandle(process);
    }
#else
    if (server)
      waitForEnd(pid_t(server), 60000);
#endif
    return true;
  }
}

bool stopServerQuietly(int argc, char** argv)
{
  return stopKey(serverKey(modulePath(), currentDirectory(), argumentsOf(argc, argv)));
}

int stopServer(int argc, char** argv)
{
  printf(stopServerQuietly(argc, argv) ? "compile server stopped\n" : "no compile server runs for these arguments\n");
  return 0;
}

int stopAllServers()
{
  // Every server runs from its copy, KEY.exe (startServer), whatever kpp started it and whether its
  // KEY.args is there or not.
  std::vector<std::string> keys;
#ifdef _WIN32
  WIN32_FIND_DATAA found;
  HANDLE search = FindFirstFileA((serverDirectory() + "*.exe").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE)
  {
    do
    {
      std::string name = found.cFileName;
      if (name.size() == 20 && name.compare(16, 4, ".exe") == 0)
        keys.push_back(name.substr(0, 16));
    } while (FindNextFileA(search, &found));
    FindClose(search);
  }
#else
  // (POSIX: every server holds its KEY.lock)
  std::error_code ec;
  for (std::filesystem::directory_iterator i(serverDirectory(), ec), end; !ec && i != end; i.increment(ec))
  {
    std::string name = i->path().filename().string();
    if (name.size() == 21 && name.compare(16, 5, ".lock") == 0 && lockHeld(i->path().string()))
      keys.push_back(name.substr(0, 16));
  }
#endif
  size_t stopped = 0;
  for (const std::string& key : keys)
    stopped += stopKey(key);
  if (stopped == 0)
    printf("no compile server runs\n");
  else
    printf("%zu compile server%s stopped\n", stopped, stopped == 1 ? "" : "s");
  return 0;
}

std::vector<std::vector<std::string>> stopServers(const std::function<bool(const std::vector<std::string>& args)>& whose)
{
  std::vector<std::vector<std::string>> stopped;
  std::vector<std::string> keys;
#ifdef _WIN32
  WIN32_FIND_DATAA found;
  HANDLE search = FindFirstFileA((serverDirectory() + "*.args").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE)
    return stopped;
  do
    keys.push_back(std::string(found.cFileName, strlen(found.cFileName) - 5));
  while (FindNextFileA(search, &found));
  FindClose(search);
#else
  std::error_code ec;
  for (std::filesystem::directory_iterator i(serverDirectory(), ec), end; !ec && i != end; i.increment(ec))
  {
    std::string name = i->path().filename().string();
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".args") == 0)
      keys.push_back(name.substr(0, name.size() - 5));
  }
#endif
  for (const std::string& key : keys)
  {
    std::string record;
    if (FILE* f = fopen(argumentsPath(key).c_str(), "rb"))
    {
      char buffer[4096];
      for (size_t n; (n = fread(buffer, 1, sizeof buffer, f)) > 0;)
        record.append(buffer, n);
      fclose(f);
    }
    std::vector<std::string> args; // (the working directory first, then the arguments)
    for (size_t at = 0, end; (end = record.find('\0', at)) != std::string::npos; at = end + 1)
      args.push_back(record.substr(at, end - at));
    if (args.empty())
      continue;
    args.erase(args.begin());
    if (whose(args) && stopKey(key))
      stopped.push_back(std::move(args));
  }
  return stopped;
}
