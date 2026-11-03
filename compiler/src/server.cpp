#include "server.hpp"

#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <fcntl.h>
#include <io.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (FILE* f = fopen(serverLog.c_str(), "ab"))
    {
      fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
              GetCurrentProcessId(), text);
      fclose(f);
    }
  }

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
  // A file's size and time of writing (or that it is missing): what tells a rebuilt or reinstalled
  // kppc from the one before, a changed output of a kept build from the one it left.
  std::string stampOf(const std::string& path)
  {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
      return "missing";
    return std::to_string(a.nFileSizeLow) + " " + std::to_string(a.nFileSizeHigh) + " " + std::to_string(a.ftLastWriteTime.dwLowDateTime) + " " +
           std::to_string(a.ftLastWriteTime.dwHighDateTime);
  }

  std::string serverKey(const std::string& exe, const std::string& cwd, const std::vector<std::string>& args)
  {
    std::string text = "kppc server 1\n" + exe + "\n" + stampOf(exe) + "\n";
    for (char c : cwd)
      text += char(tolower(static_cast<unsigned char>(c)));
    for (const std::string& arg : args)
      if (arg != "--verify-incremental") // a check of the server's build, not another build
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
    // The kppc it was started from and that kppc's stamp, as the key has it: the server exits once
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
    HANDLE answeredEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr); // set by answer()
    int exitCode = 0;
    ~Session() { CloseHandle(answeredEvent); }
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
    if (!s.local)
    {
      FlushFileBuffers(s.client);
      DisconnectNamedPipe(s.client);
    }
    s.exitCode = exitCode;
    SetEvent(s.answeredEvent);
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
    std::atomic<bool> released{false};
  };

  // One build, on a thread of its own: thread-local state (the resolver's, the evaluator's) starts
  // fresh as in a new process, and the stack is the 8 MB the executable is linked with.
  // When the build has answered its client (buildDone), what is left is freeing what it built
  // (about 10 s on the full game): the next request is served meanwhile, the thread finishing on
  // its own. Whoever of the two lets go of the job last deletes it.
  int runBuild(CompileFunction compile, std::vector<std::string> args, Session& s)
  {
    BuildJob* job = new BuildJob{compile, std::move(args)};
    HANDLE thread = CreateThread(nullptr, size_t(8) << 20, [](void* p) -> DWORD
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

  // What a build printed, without what differs from run to run by design (timings and rates, wait
  // counts, the interned types' count, whether the C import came from its cache, how many objects the
  // back end made while codegen ran).
  std::string normalized(const std::string& text)
  {
    static const std::regex seconds("[0-9]+\\.[0-9]+ s\\b"), rates("\\([0-9.]+ [MG]?B/s[^)]*\\)"), lines("\\([0-9.]+ M lines/s\\)"),
      waits("\\([0-9]+ requests, [0-9]+ waits\\)"), unchanged("[0-9]+ unchanged"), during(", [0-9]+ during codegen"), types("^check: +[0-9]+ distinct types"),
      cached(" \\(cached\\)$");
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
    std::string own = serverDirectory() + "verify." + std::to_string(GetCurrentProcessId());
    std::string outPath = own + ".out", errPath = own + ".err";
    int savedOut = _dup(_fileno(stdout)), savedErr = _dup(_fileno(stderr));
    int out = _open(outPath.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    int err = _open(errPath.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    pointStreams(out, err);
    _close(out);
    _close(err);
    replayOff = true;
    s.verifying = true;
    int fresh = runBuild(compile, std::move(args), s);
    s.verifying = false;
    replayOff = false;
    fflush(stdout);
    fflush(stderr);
    pointStreams(savedOut, savedErr);
    _close(savedOut);
    _close(savedErr);
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
    std::string message = "kppc: --verify-incremental: the server's build is not what a build from scratch gives: " + problem + "\n";
    sendFrame(s, stderrFrame, message.data(), uint32_t(message.size())); // (the build's streams are drained)
    return false;
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
    logLine("build %zu: %u arguments, in %s, the last %s", ++builds, argc, cwd.c_str(), args.back().c_str());
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
    err = "kppc: cannot redirect the build's output\n";
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
    logLine("build %zu: %s", builds, note.c_str());
}

std::string environmentBlock()
{
  std::string out;
  if (wchar_t* block = GetEnvironmentStringsW())
  {
    const wchar_t* q = block;
    while (*q)
      q += wcslen(q) + 1;
    out.assign(reinterpret_cast<const char*>(block), (q - block + 1) * sizeof(wchar_t));
    FreeEnvironmentStringsW(block);
  }
  return out;
}

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
  HANDLE pipe = connectPipe(name, 0);
  if (pipe == INVALID_HANDLE_VALUE && startServer(exe, key, cwd))
  {
    std::string record = cwd + '\0';
    for (const std::string& a : args)
      record += a + '\0';
    if (FILE* f = fopen(argumentsPath(key).c_str(), "wb"))
      fwrite(record.data(), 1, record.size(), f), fclose(f);
    pipe = connectPipe(name, 20000);
  }
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

int runServer(const char* key, const char* origin, const char* stamp, CompileFunction compile)
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
  // Between builds: exits once idle for an hour, or once the kppc it was started from has changed (a
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
        logLine("the kppc it was started from (%s) changed: exiting", origin.c_str());
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

namespace
{
  // Stops the server of key if one runs (its KEY.args goes with it); false when none does.
  bool stopKey(const std::string& key)
  {
    HANDLE pipe = connectPipe(pipeName(key), 0);
    DeleteFileA(argumentsPath(key).c_str());
    if (pipe == INVALID_HANDLE_VALUE)
      return false;
    std::string request = requestHeader(stopRequest);
    writeAll(pipe, request.data(), request.size());
    // Its process id, then the exit code: read up to the exit code, so that the server's last write
    // finds the client still there. Then it waits for the process to end: stopped means gone, and a
    // build right after starts a server of its own.
    DWORD server = 0;
    for (char frame[9]; readAll(pipe, frame, 9) && frame[0] != char(exitFrame);)
      if (frame[0] == char(processFrame))
        memcpy(&server, frame + 5, 4);
    CloseHandle(pipe);
    if (HANDLE process = server ? OpenProcess(SYNCHRONIZE, FALSE, server) : nullptr)
    {
      WaitForSingleObject(process, 60000);
      CloseHandle(process);
    }
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
  // Every server runs from its copy, KEY.exe (startServer), whatever kppc started it and whether its
  // KEY.args is there or not.
  std::vector<std::string> keys;
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
  WIN32_FIND_DATAA found;
  HANDLE search = FindFirstFileA((serverDirectory() + "*.args").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE)
    return stopped;
  std::vector<std::string> keys;
  do
    keys.push_back(std::string(found.cFileName, strlen(found.cFileName) - 5));
  while (FindNextFileA(search, &found));
  FindClose(search);
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
