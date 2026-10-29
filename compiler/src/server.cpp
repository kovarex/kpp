#include "server.hpp"

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The protocol (docs/compile-speed.md, "The compile server: protocol"). One named pipe per key,
// one client at a time. A request: "KPS1", a u32 kind (1 build, 2 stop), then for a build the
// kppc path the client ran, its working directory, its arguments (a u32 count, then each) and its
// environment (UTF-16, as GetEnvironmentStringsW gives it); a string is a u32 byte count and the
// bytes. The answer: frames of a u8 type (1 stdout, 2 stderr, 3 exit code, 4 the server's process
// id, sent first), a u32 byte count and the bytes, ending with the exit code's frame. The server
// logs what it does to %TEMP%\kppc-server\KEY.log: a client that loses its server says why.

namespace
{
  const uint32_t buildRequest = 1, stopRequest = 2;
  const uint8_t stdoutFrame = 1, stderrFrame = 2, exitFrame = 3, processFrame = 4;
  const int64_t idleSeconds = 3600;

  std::string serverDirectory()
  {
    char temp[MAX_PATH * 4];
    DWORD n = GetTempPathA(DWORD(sizeof temp), temp);
    return std::string(temp, n) + "kppc-server\\";
  }

  std::string logPath(const std::string& key) { return serverDirectory() + key + ".log"; }

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
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (FILE* f = fopen(serverLog.c_str(), "ab"))
    {
      fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
              GetCurrentProcessId(), text);
      fclose(f);
    }
  }

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

  void putU32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
  void putString(std::string& out, const std::string& s)
  {
    putU32(out, uint32_t(s.size()));
    out += s;
  }
  bool readU32(HANDLE h, uint32_t& v) { return readAll(h, &v, 4); }
  bool readString(HANDLE h, std::string& s)
  {
    uint32_t n = 0;
    if (!readU32(h, n) || n > (1u << 28))
      return false;
    s.resize(n);
    return n == 0 || readAll(h, s.data(), n);
  }

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

  // The key of a server: the kppc binary (path, size and time: a rebuilt kppc is another server),
  // the working directory and the arguments. A server only ever builds its own key's arguments,
  // so a changed flag or input list goes to another server, never to one holding older state.
  std::string serverKey(const std::string& exe, const std::string& cwd, const std::vector<std::string>& args)
  {
    std::string text = "kppc server 1\n" + exe + "\n";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExA(exe.c_str(), GetFileExInfoStandard, &a))
      text += std::to_string(a.nFileSizeLow) + " " + std::to_string(a.nFileSizeHigh) + " " + std::to_string(a.ftLastWriteTime.dwLowDateTime) + " " +
              std::to_string(a.ftLastWriteTime.dwHighDateTime);
    text += "\n";
    for (char c : cwd)
      text += char(tolower(static_cast<unsigned char>(c)));
    for (const std::string& arg : args)
      text += std::string(1, '\0') + arg;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text)
      h = (h ^ c) * 1099511628211ull;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    return hex;
  }

  std::string pipeName(const std::string& key) { return "\\\\.\\pipe\\kppc-server-" + key; }

  // Connects to the server's pipe, waiting while another client is being served.
  HANDLE connectPipe(const std::string& name, DWORD waitMs)
  {
    auto start = std::chrono::steady_clock::now();
    for (;;)
    {
      HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE)
        return h;
      DWORD error = GetLastError();
      if (error == ERROR_PIPE_BUSY)
        WaitNamedPipeA(name.c_str(), NMPWAIT_WAIT_FOREVER); // a build is running: as long as it takes
      else if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(waitMs))
        return INVALID_HANDLE_VALUE;
      else
        Sleep(20);
    }
  }

  // Starts a server for the key. It runs from a copy of kppc in the temp directory, so that kppc
  // itself can be rebuilt while a server runs (Windows keeps a running executable locked).
  bool startServer(const std::string& exe, const std::string& key, const std::string& cwd)
  {
    std::string dir = serverDirectory();
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string copy = dir + key + ".exe";
    if (GetFileAttributesA(copy.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
      std::string part = copy + "." + std::to_string(GetCurrentProcessId());
      if (!CopyFileA(exe.c_str(), part.c_str(), FALSE))
        return false;
      if (!MoveFileA(part.c_str(), copy.c_str()))
        DeleteFileA(part.c_str()); // another client put it there first
    }
    std::string command = "\"" + copy + "\" --serve " + key;
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Out of the client's job if it may leave it: whatever waits for the client's process tree
    // (PowerShell's Start-Process -Wait) must not wait for the server too.
    const DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessA(copy.c_str(), command.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, cwd.c_str(), &si, &pi) &&
        !CreateProcessA(copy.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, cwd.c_str(), &si, &pi))
      return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
  }

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
    HANDLE client = INVALID_HANDLE_VALUE;
    std::mutex writeLock;
    HANDLE outRead = nullptr, errRead = nullptr;
    std::thread outForward, errForward;
    bool answered = false;
    bool clientLost = false; // a write to the client failed: it went away (the build goes on, unseen)
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
    if (s.clientLost)
      return false;
    char head[5];
    head[0] = char(type);
    memcpy(head + 1, &size, 4);
    if (writeAll(s.client, head, 5) && (size == 0 || writeAll(s.client, data, size)))
      return true;
    s.clientLost = true;
    logLine("build %zu: the client went away (write failed, error %lu)", builds, GetLastError());
    return false;
  }

  void forward(Session& s, HANDLE from, uint8_t type)
  {
    char buffer[1 << 16];
    for (;;)
    {
      DWORD n = 0;
      if (!ReadFile(from, buffer, DWORD(sizeof buffer), &n, nullptr) || n == 0)
        return;
      sendFrame(s, type, buffer, n);
    }
  }

  // Points stdout at descriptor out and stderr at err: the streams' own descriptors, descriptors 1
  // and 2 (which the CRT hands to the processes system() starts) and the process's standard
  // handles (which those processes inherit).
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

  // Points the process's stdout and stderr (the CRT's descriptors 1 and 2 and the handles child
  // processes inherit, clang's included) at new pipes, and starts forwarding them.
  bool redirectOutput(Session& s)
  {
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
  }

  // Sends the build's exit code once everything it printed is through, and lets the client go.
  void answer(Session& s, int exitCode)
  {
    if (s.answered)
      return;
    s.answered = true;
    fflush(stdout);
    fflush(stderr);
    int nul = _open("NUL", _O_WRONLY | _O_BINARY);
    pointStreams(nul, nul);
    _close(nul);
    // The pipes' last write ends are closed now (clang's closed with clang): the forwarders end.
    if (s.outForward.joinable())
      s.outForward.join();
    if (s.errForward.joinable())
      s.errForward.join();
    CloseHandle(s.outRead);
    CloseHandle(s.errRead);
    sendFrame(s, exitFrame, &exitCode, 4);
    FlushFileBuffers(s.client);
    DisconnectNamedPipe(s.client);
  }

  // The client's environment for the build (TMP, which holds the C import cache, LIB and INCLUDE
  // for clang's link, KPP_CLANG...): what the server has and the client has not is removed.
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

  struct BuildJob
  {
    CompileFunction compile;
    std::vector<std::string> args;
    int result = 1;
  };

  // One build, on a thread of its own: thread-local state (the resolver's, the evaluator's) starts
  // fresh as in a new process, and the stack is the 8 MB the executable is linked with.
  int runBuild(CompileFunction compile, std::vector<std::string> args)
  {
    BuildJob job{compile, std::move(args)};
    HANDLE thread = CreateThread(nullptr, size_t(8) << 20, [](void* p) -> DWORD
    {
      BuildJob* j = static_cast<BuildJob*>(p);
      std::vector<char*> argv;
      for (std::string& a : j->args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      j->result = j->compile(int(argv.size() - 1), argv.data());
      return 0;
    }, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread)
      return 1;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return job.result;
  }

  // Serves one client. Returns false when the client asked the server to stop.
  bool serveClient(HANDLE client, CompileFunction compile)
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
    DWORD self = GetCurrentProcessId();
    sendFrame(s, processFrame, &self, 4); // so that a client that loses the server can wait for it to end
    if (kind == stopRequest)
    {
      logLine("stopped by a client");
      int zero = 0;
      sendFrame(s, exitFrame, &zero, 4);
      FlushFileBuffers(client);
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
    logLine("build %zu: %u arguments, in %s", ++builds, argc, cwd.c_str());
    applyEnvironment(environment);
    SetCurrentDirectoryA(cwd.c_str());
    if (!redirectOutput(s))
    {
      const char* message = "kppc server: cannot redirect the build's output\n";
      sendFrame(s, stderrFrame, message, uint32_t(strlen(message)));
      int one = 1;
      sendFrame(s, exitFrame, &one, 4);
      return true;
    }
    session = &s;
    int result = runBuild(compile, std::move(args));
    answer(s, result); // if the build did not answer itself (usage errors, --dump)
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

void buildDone(int exitCode)
{
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
  HANDLE pipe = connectPipe(name, 0);
  if (pipe == INVALID_HANDLE_VALUE && startServer(exe, key, cwd))
    pipe = connectPipe(name, 20000);
  if (pipe == INVALID_HANDLE_VALUE)
  {
    fprintf(stderr, "kppc: no compile server could be reached; building in this process\n");
    return compile(argc, argv);
  }
  std::string request = requestHeader(buildRequest);
  putString(request, exe);
  putString(request, cwd);
  putU32(request, uint32_t(args.size()));
  for (const std::string& a : args)
    putString(request, a);
  std::string environment;
  if (wchar_t* block = GetEnvironmentStringsW())
  {
    const wchar_t* q = block;
    while (*q)
      q += wcslen(q) + 1;
    environment.assign(reinterpret_cast<const char*>(block), (q - block + 1) * sizeof(wchar_t));
    FreeEnvironmentStringsW(block);
  }
  putString(request, environment);
  DWORD serverProcess = 0;
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
        CloseHandle(pipe);
        return code;
      }
      if (head[0] == processFrame && size == 4)
      {
        memcpy(&serverProcess, data.data(), 4);
        continue;
      }
      FILE* to = head[0] == stderrFrame ? stderr : stdout;
      fwrite(data.data(), 1, size, to);
      fflush(to);
    }
  }
  DWORD readError = GetLastError();
  CloseHandle(pipe);
  // The server is gone or going: before building here, wait until it has ended (its job ends
  // the clang and link it started with it), so that nothing of its build still writes the outputs;
  // one that does not end is ended.
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
  std::string why = lastLogLine(logPath(key));
  fprintf(stderr, "kppc: the compile server ended without finishing the build (read error %lu; its log, %s, ends: %s); building in this process\n",
          readError, logPath(key).c_str(), why.empty() ? "(nothing)" : why.c_str());
  return compile(argc, argv);
}

int runServer(const char* key, CompileFunction compile)
{
  std::string name = pipeName(key);
  HANDLE pipe = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE)
    return 1; // another server has the key
  serverLog = logPath(key);
  logLine("started");
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
    return EXCEPTION_EXECUTE_HANDLER;
  });
  signal(SIGABRT, [](int) { logLine("aborted in build %zu", builds); });
  // A detached process has no console, so its streams have no descriptors: give them some.
  freopen("NUL", "wb", stdout);
  freopen("NUL", "wb", stderr);
  setvbuf(stdout, nullptr, _IONBF, 0);
  lastActivity = nowSeconds();
  static std::mutex idleMutex; // the idle check and the start of a client's service exclude each other
  std::thread([]
  {
    for (;;)
    {
      Sleep(60 * 1000);
      std::lock_guard lock(idleMutex);
      if (!busy && nowSeconds() - lastActivity > idleSeconds)
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
    if (!goOn)
      return 0;
  }
}

int stopServer(int argc, char** argv)
{
  std::string exe = modulePath(), cwd = currentDirectory();
  std::string name = pipeName(serverKey(exe, cwd, argumentsOf(argc, argv)));
  HANDLE pipe = connectPipe(name, 0);
  if (pipe == INVALID_HANDLE_VALUE)
  {
    printf("no compile server runs for these arguments\n");
    return 0;
  }
  std::string request = requestHeader(stopRequest);
  writeAll(pipe, request.data(), request.size());
  // Its process id, then the exit code: read up to the exit code, so that the server's last write
  // finds the client still there.
  for (char frame[9]; readAll(pipe, frame, 9) && frame[0] != char(exitFrame);)
    ;
  CloseHandle(pipe);
  printf("compile server stopped\n");
  return 0;
}
