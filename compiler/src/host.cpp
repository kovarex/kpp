#include "host.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <corecrt_startup.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__FreeBSD__) || defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace host
{
  const char* platformName()
  {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__APPLE__)
    return "macos";
#else
#error kpp does not know this host
#endif
  }

#ifdef _WIN32
  namespace
  {
    std::wstring wide(const std::string& text)
    {
      std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
      return out;
    }

    // A command line argument as CommandLineToArgvW and the C runtime read it back.
    std::string quoted(const std::string& arg)
    {
      if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;
      std::string out = "\"";
      size_t backslashes = 0;
      for (char c : arg)
      {
        if (c == '\\')
        {
          ++backslashes;
          continue;
        }
        out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        backslashes = 0;
        out += c;
      }
      out.append(backslashes * 2, '\\');
      return out + "\"";
    }

    // CreateProcessW of application (empty: the command line's first word) and the command line,
    // as run describes.
    int runProcess(const std::wstring& application, const std::string& commandLine, const Run& how, std::string& why)
    {
      STARTUPINFOW si{};
      si.cb = sizeof si;
      HANDLE log = nullptr, reads[2] = {nullptr, nullptr}, writes[2] = {nullptr, nullptr};
      std::string seen[2];
      std::thread readers[2];
      if (!how.appendTo.empty())
      {
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        log = CreateFileW(how.appendTo.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE)
        {
          why = "cannot open " + how.appendTo.string() + " (error " + std::to_string(GetLastError()) + ")";
          return -1;
        }
        si.dwFlags = STARTF_USESTDHANDLES; // (and no stdin)
        si.hStdOutput = si.hStdError = log;
      }
      else if (how.seen)
      {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE* outputs[2] = {&si.hStdOutput, &si.hStdError};
        for (int k = 0; k < 2; ++k)
        {
          *outputs[k] = GetStdHandle(k == 0 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
          if (CreatePipe(&reads[k], &writes[k], nullptr, 1 << 16))
          {
            SetHandleInformation(writes[k], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            *outputs[k] = writes[k];
          }
        }
      }
      PROCESS_INFORMATION pi{};
      std::wstring line = wide(commandLine);
      std::wstring directory = how.directory.empty() ? std::wstring() : how.directory.wstring();
      // (in a group: started suspended and put in its job before it runs, so that what it starts is in the job too)
      HANDLE job = how.group ? HANDLE(how.group->job) : nullptr;
      bool started = CreateProcessW(application.empty() ? nullptr : application.c_str(), line.data(), nullptr, nullptr, TRUE, job ? CREATE_SUSPENDED : 0,
                                    nullptr, directory.empty() ? nullptr : directory.c_str(), &si, &pi);
      DWORD startError = GetLastError();
      if (started && job)
      {
        AssignProcessToJobObject(job, pi.hProcess); // (failing, it runs at this process's priority)
        ResumeThread(pi.hThread);
      }
      for (int k = 0; k < 2; ++k)
        if (writes[k])
        {
          CloseHandle(writes[k]);
          readers[k] = std::thread([&, k]
          {
            char buffer[1 << 14];
            DWORD n;
            HANDLE to = GetStdHandle(k == 0 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
            DWORD written;
            while (ReadFile(reads[k], buffer, sizeof buffer, &n, nullptr) && n > 0)
            {
              how.seen(k, buffer, n);
              WriteFile(to, buffer, n, &written, nullptr);
            }
            CloseHandle(reads[k]);
          });
        }
      for (std::thread& t : readers)
        if (t.joinable() && !started)
          t.join();
      if (!started)
      {
        if (log)
          CloseHandle(log);
        why = "error " + std::to_string(startError);
        return -1;
      }
      CloseHandle(pi.hThread);
      WaitForSingleObject(pi.hProcess, INFINITE);
      for (std::thread& t : readers)
        if (t.joinable())
          t.join();
      DWORD code = 1;
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hProcess);
      if (log)
        CloseHandle(log);
      return int(code);
    }

    void (*interrupted)() = nullptr;
    BOOL WINAPI onConsoleControl(DWORD)
    {
      if (interrupted)
        interrupted();
      return FALSE; // and the process ends as it would have
    }
  }

  fs::path executablePath()
  {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(n);
    return fs::path(path);
  }

  int processId() { return int(GetCurrentProcessId()); }

  fs::path pathOfUtf8(const std::string& text) { return fs::path(wide(text)); }

  bool argumentsAsUtf8(int argc, char** argv, int first, std::vector<std::string>& out, std::string& why)
  {
    // Arguments another caller made (kpp's --test of a project) are taken as they are: they are not the
    // command line's.
    if (argv != __argv)
    {
      out.assign(argv + std::min(first, argc), argv + argc);
      return true;
    }
    // The C runtime parses the wide command line as it parsed the narrow one (and sets __argc again
    // to the same count).
    if (_configure_wide_argv(_crt_argv_unexpanded_arguments) != 0 || !__wargv || __argc != argc)
    {
      why = "the C runtime gave no wide arguments";
      return false;
    }
    out.clear();
    for (int i = first; i < argc; ++i)
    {
      // (WC_ERR_INVALID_CHARS: a lone surrogate fails, where it would be written as U+FFFD)
      int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, __wargv[i], -1, nullptr, 0, nullptr, nullptr);
      if (n <= 0)
      {
        why = "argument " + std::to_string(i - first + 1) + " holds a lone surrogate, which has no UTF-8 form";
        return false;
      }
      std::string text(size_t(n), '\0');
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, __wargv[i], -1, text.data(), n, nullptr, nullptr);
      text.resize(size_t(n) - 1);
      out.push_back(std::move(text));
    }
    return true;
  }

  const char* executableSuffix() { return ".exe"; }

  std::string pathKey(const fs::path& path)
  {
    std::wstring w = path.wstring();
    CharLowerBuffW(w.data(), DWORD(w.size()));
    return fs::path(w).string();
  }

  void setThreadBelowNormal(bool below) { SetThreadPriority(GetCurrentThread(), below ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL); }

  bool startOnLargeStack(size_t bytes, void (*fn)(void*), void* arg, LargeStackThread& thread)
  {
    struct Start
    {
      void (*fn)(void*);
      void* arg;
    };
    Start* start = new Start{fn, arg};
    HANDLE h = CreateThread(nullptr, bytes, [](void* p) -> DWORD
    {
      Start s = *static_cast<Start*>(p);
      delete static_cast<Start*>(p);
      s.fn(s.arg);
      return 0;
    }, start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h)
    {
      delete start;
      return false;
    }
    thread.handle = h;
    return true;
  }

  void joinThread(LargeStackThread& thread)
  {
    WaitForSingleObject(thread.handle, INFINITE);
    CloseHandle(thread.handle);
    thread.handle = nullptr;
  }

  void useCompilerStackForThreads() {} // (the executable's /STACK is every thread's default)
  int runOnCompilerStack(int (*fn)(int, char**), int argc, char** argv) { return fn(argc, argv); }

  FILE* openFile(const fs::path& path, const char* mode) { return _wfopen(path.c_str(), wide(mode).c_str()); }
  // (cmd /c takes the first and the last quote off a command line that starts with one: the whole is
  // quoted, so that one starting with a quoted program path keeps its quotes)
  FILE* openPipe(const std::string& command, const char* mode) { return _popen(("\"" + command + "\"").c_str(), mode); }
  int closePipe(FILE* pipe) { return _pclose(pipe); }
  const char* nullDevice() { return "NUL"; }

  PriorityGroup::PriorityGroup()
  {
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_BASIC_LIMIT_INFORMATION limits{};
    limits.LimitFlags = JOB_OBJECT_LIMIT_PRIORITY_CLASS;
    limits.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    if (job && !SetInformationJobObject(HANDLE(job), JobObjectBasicLimitInformation, &limits, sizeof limits))
    {
      CloseHandle(HANDLE(job));
      job = nullptr; // (then shell starts its processes as without a group)
    }
  }

  PriorityGroup::~PriorityGroup()
  {
    if (job)
      CloseHandle(HANDLE(job)); // (its processes run on: no kill-on-close)
  }

  void PriorityGroup::raise()
  {
    JOBOBJECT_BASIC_LIMIT_INFORMATION limits{};
    limits.LimitFlags = JOB_OBJECT_LIMIT_PRIORITY_CLASS;
    limits.PriorityClass = NORMAL_PRIORITY_CLASS; // (the job's running processes take it at once)
    if (job)
      SetInformationJobObject(HANDLE(job), JobObjectBasicLimitInformation, &limits, sizeof limits);
  }

  int shell(const std::string& command, const fs::path& directory, PriorityGroup* lowered)
  {
    std::string line = command;
    if (!directory.empty())
    {
      // (quoted as generate.cpp's quoteArg quotes an argument for cmd)
      std::string dir = directory.string();
      if (dir.empty() || dir.find_first_of(" \t&|<>^") != std::string::npos)
        dir = "\"" + dir + "\"";
      line = "cd /d " + dir + " && " + command;
    }
    if (!lowered || !lowered->job)
      return system(("\"" + line + "\"").c_str());
    // (cmd /c as system runs it, in the group's job, which cmd's children join)
    wchar_t comspec[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    std::wstring cmd = n > 0 && n < MAX_PATH ? std::wstring(comspec, n) : std::wstring(L"C:\\Windows\\System32\\cmd.exe");
    Run how;
    how.group = lowered;
    std::string why;
    return runProcess(cmd, "cmd /c \"" + line + "\"", how, why);
  }

  int shellAsIs(const std::string& line) { return system(line.c_str()); }

  size_t readStream(Stream stream, char* buffer, size_t size)
  {
    DWORD n = 0;
    return ReadFile(HANDLE(stream), buffer, DWORD(size), &n, nullptr) ? size_t(n) : 0;
  }

  void writeStream(Stream stream, const char* data, size_t size)
  {
    for (size_t done = 0; done < size;)
    {
      DWORD n = 0;
      if (!WriteFile(HANDLE(stream), data + done, DWORD(size - done), &n, nullptr) || n == 0)
        return;
      done += n;
    }
  }

  Stream createLog(const fs::path& path)
  {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? noStream : Stream(h);
  }

  void takeStandardStreams(Stream& in, Stream& out, Stream& err)
  {
    HANDLE process = GetCurrentProcess(), handles[3] = {};
    DuplicateHandle(process, GetStdHandle(STD_INPUT_HANDLE), process, &handles[0], 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(process, GetStdHandle(STD_OUTPUT_HANDLE), process, &handles[1], 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(process, GetStdHandle(STD_ERROR_HANDLE), process, &handles[2], 0, FALSE, DUPLICATE_SAME_ACCESS);
    in = Stream(handles[0]), out = Stream(handles[1]), err = Stream(handles[2]);
    fflush(stdout);
    fflush(stderr);
    int nul = _open("NUL", _O_RDWR | _O_BINARY);
    _dup2(nul, 0);
    _dup2(nul, 1);
    _dup2(nul, 2);
    _close(nul);
    SetStdHandle(STD_INPUT_HANDLE, HANDLE(_get_osfhandle(0)));
    SetStdHandle(STD_OUTPUT_HANDLE, HANDLE(_get_osfhandle(1)));
    SetStdHandle(STD_ERROR_HANDLE, HANDLE(_get_osfhandle(2)));
  }

  // A console, or the pipe a Cygwin or MSYS terminal gives its programs (Git Bash's mintty:
  // \msys-<hash>-pty0-from-master, Cygwin's \cygwin-<hash>-pty0-...), which _isatty does not take for
  // one (kovarex, 2026-11-04: `kpp` alone in Git Bash showed no menu).
  bool interactiveInput()
  {
    if (_isatty(_fileno(stdin)))
      return true;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == nullptr || in == INVALID_HANDLE_VALUE || GetFileType(in) != FILE_TYPE_PIPE)
      return false;
    std::vector<char> buffer(sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(wchar_t));
    FILE_NAME_INFO* info = reinterpret_cast<FILE_NAME_INFO*>(buffer.data());
    if (!GetFileInformationByHandleEx(in, FileNameInfo, info, DWORD(buffer.size())))
      return false;
    std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
    bool terminal = name.rfind(L"\\msys-", 0) == 0 || name.rfind(L"\\cygwin-", 0) == 0;
    return terminal && name.find(L"-pty") != std::wstring::npos && name.find(L"-from-master") != std::wstring::npos;
  }

  int run(const std::string& program, const std::vector<std::string>& arguments, const Run& how, std::string& why)
  {
    std::string command = quoted(program);
    for (const std::string& a : arguments)
      command += " " + quoted(a);
    return runProcess(wide(program), command, how, why);
  }

  int runCommandLine(const std::string& commandLine, const Run& how, std::string& why) { return runProcess(std::wstring(), commandLine, how, why); }

  void onInterrupt(void (*fn)())
  {
    interrupted = fn;
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
  }

  std::string machineDescription()
  {
    std::string cpu = "an unknown processor";
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
      char name[256] = {};
      DWORD size = sizeof name - 1, type = 0;
      if (RegQueryValueExA(key, "ProcessorNameString", nullptr, &type, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS && type == REG_SZ)
        cpu = name;
      RegCloseKey(key);
      while (!cpu.empty() && cpu.back() == ' ')
        cpu.pop_back();
    }
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<char> buffer(length);
    unsigned cores = 0;
    if (length && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length))
      for (DWORD at = 0; at < length; ++cores)
        at += reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + at)->Size;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    GlobalMemoryStatusEx(&memory);
    char line[512];
    snprintf(line, sizeof line, "%s, %u cores, %lu threads, %.0f GB", cpu.c_str(), cores, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
             double(memory.ullTotalPhys) / (1024.0 * 1024 * 1024));
    return line;
  }

  int machineLoad()
  {
    FILETIME idle[2], kernel[2], user[2];
    GetSystemTimes(&idle[0], &kernel[0], &user[0]);
    Sleep(1000);
    GetSystemTimes(&idle[1], &kernel[1], &user[1]);
    auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    uint64_t idled = ticks(idle[1]) - ticks(idle[0]), all = ticks(kernel[1]) - ticks(kernel[0]) + ticks(user[1]) - ticks(user[0]); // (kernel time includes idle)
    return all ? int(100.0 * double(all - idled) / double(all) + 0.5) : 0;
  }

  double machineBusySeconds()
  {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user))
      return -1;
    auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user) - ticks(idle)) / 1e7; // (kernel time includes idle; 100 ns ticks)
  }

  double groupCpuSeconds(const PriorityGroup& group)
  {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!group.job || !QueryInformationJobObject(HANDLE(group.job), JobObjectBasicAccountingInformation, &info, sizeof info, nullptr))
      return -1;
    return double(info.TotalUserTime.QuadPart + info.TotalKernelTime.QuadPart) / 1e7;
  }

  bool FileLock::shared(const fs::path& path)
  {
    release();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      return false;
    handle_ = intptr_t(h), path_ = path;
    return true;
  }

  bool FileLock::alone(const fs::path& path)
  {
    release();
    HANDLE h = CreateFileW(path.c_str(), DELETE | GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      return false;
    handle_ = intptr_t(h), path_ = path;
    return true;
  }

  void FileLock::touch()
  {
    if (handle_ == -1)
      return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(HANDLE(handle_), nullptr, nullptr, &now);
  }

  void FileLock::release()
  {
    if (handle_ != -1)
      CloseHandle(HANDLE(handle_));
    handle_ = -1;
  }
#else
  namespace
  {
    void (*interrupted)() = nullptr;
    void onSignal(int signal)
    {
      if (interrupted)
        interrupted();
      ::signal(signal, SIG_DFL); // and the process ends as it would have
      ::raise(signal);
    }

    // posix_spawn of argv, as run describes.
    int spawnAndWait(const std::vector<std::string>& argv, const Run& how, std::string& why)
    {
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      int pipes[2][2] = {{-1, -1}, {-1, -1}};
      if (!how.appendTo.empty())
      {
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, how.appendTo.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
      }
      else if (how.seen)
        for (int k = 0; k < 2; ++k)
          if (pipe(pipes[k]) == 0)
          {
            posix_spawn_file_actions_adddup2(&actions, pipes[k][1], k + 1);
            posix_spawn_file_actions_addclose(&actions, pipes[k][0]);
            posix_spawn_file_actions_addclose(&actions, pipes[k][1]);
          }
      if (!how.directory.empty())
        posix_spawn_file_actions_addchdir_np(&actions, how.directory.c_str());
      std::vector<char*> args;
      for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
      args.push_back(nullptr);
      pid_t pid = 0;
      int error = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      std::thread readers[2];
      for (int k = 0; k < 2; ++k)
        if (pipes[k][1] != -1)
        {
          close(pipes[k][1]);
          readers[k] = std::thread([&, k]
          {
            char buffer[1 << 14];
            ssize_t n;
            while ((n = read(pipes[k][0], buffer, sizeof buffer)) > 0)
            {
              how.seen(k, buffer, size_t(n));
              for (ssize_t at = 0; at < n;)
              {
                ssize_t w = write(k + 1, buffer + at, size_t(n - at));
                if (w <= 0)
                  break;
                at += w;
              }
            }
            close(pipes[k][0]);
          });
        }
      for (std::thread& t : readers)
        if (t.joinable() && error != 0)
          t.join();
      if (error != 0)
      {
        why = strerror(error);
        return -1;
      }
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
      {
      }
      for (std::thread& t : readers)
        if (t.joinable())
          t.join();
      if (WIFEXITED(status))
        return WEXITSTATUS(status);
      return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    }
  }

  fs::path executablePath()
  {
#if defined(__linux__)
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#elif defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    char path[4096];
    size_t size = sizeof path;
    return sysctl(mib, 4, path, &size, nullptr, 0) == 0 ? fs::path(path) : fs::path();
#elif defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof path;
    return _NSGetExecutablePath(path, &size) == 0 ? fs::weakly_canonical(path) : fs::path();
#endif
  }

  int processId() { return int(getpid()); }

  fs::path pathOfUtf8(const std::string& text) { return fs::path(text); }

  bool argumentsAsUtf8(int argc, char** argv, int first, std::vector<std::string>& out, std::string&)
  {
    out.assign(argv + std::min(first, argc), argv + argc);
    return true;
  }

  const char* executableSuffix() { return ""; }

  std::string pathKey(const fs::path& path) { return path.string(); }

  // (Linux's nice applies to a thread, its own task; elsewhere the call changes nothing)
  void setThreadBelowNormal(bool below)
  {
#if defined(__linux__)
    setpriority(PRIO_PROCESS, 0, below ? 5 : 0);
#else
    (void)below;
#endif
  }

  // (glibc's and FreeBSD's thread stacks are mappings that take memory as they are touched, as a
  // Windows reservation does: the default 8 MB is less than the game's deepest code needs)
  bool startOnLargeStack(size_t bytes, void (*fn)(void*), void* arg, LargeStackThread& thread)
  {
    struct Start
    {
      void (*fn)(void*);
      void* arg;
    };
    Start* start = new Start{fn, arg};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, bytes);
    pthread_t* t = new pthread_t;
    int error = pthread_create(t, &attributes, [](void* p) -> void*
    {
      Start s = *static_cast<Start*>(p);
      delete static_cast<Start*>(p);
      s.fn(s.arg);
      return nullptr;
    }, start);
    pthread_attr_destroy(&attributes);
    if (error != 0)
    {
      delete start;
      delete t;
      return false;
    }
    thread.handle = t;
    return true;
  }

  void joinThread(LargeStackThread& thread)
  {
    pthread_t* t = static_cast<pthread_t*>(thread.handle);
    pthread_join(*t, nullptr);
    delete t;
    thread.handle = nullptr;
  }

  void useCompilerStackForThreads()
  {
#if defined(__GLIBC__)
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, compilerStack);
    pthread_setattr_default_np(&attributes);
    pthread_attr_destroy(&attributes);
#endif
  }

  int runOnCompilerStack(int (*fn)(int, char**), int argc, char** argv)
  {
    struct Run
    {
      int (*fn)(int, char**);
      int argc;
      char** argv;
      int result;
    } run{fn, argc, argv, 0};
    LargeStackThread thread;
    if (!startOnLargeStack(compilerStack, [](void* p)
    {
      Run* r = static_cast<Run*>(p);
      r->result = r->fn(r->argc, r->argv);
    }, &run, thread))
      return fn(argc, argv); // (no thread: the main one's stack, which the evaluator's stack check still guards)
    joinThread(thread);
    return run.result;
  }

  FILE* openFile(const fs::path& path, const char* mode) { return fopen(path.c_str(), mode); }
  // (popen takes "r" or "w" alone: a pipe has no text mode to leave)
  FILE* openPipe(const std::string& command, const char* mode) { return popen(command.c_str(), std::string(mode).find('w') != std::string::npos ? "w" : "r"); }

  int closePipe(FILE* pipe)
  {
    int status = pclose(pipe);
    if (status == -1)
      return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  }

  const char* nullDevice() { return "/dev/null"; }

  namespace
  {
    int exitCodeOf(int status)
    {
      if (status == -1)
        return -1;
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
  }

  PriorityGroup::PriorityGroup() {}
  PriorityGroup::~PriorityGroup() {}
  void PriorityGroup::raise() {}

  int shell(const std::string& command, const fs::path& directory, PriorityGroup*)
  {
    if (directory.empty())
      return exitCodeOf(system(command.c_str()));
    std::string dir = directory.string(), quoted = "'";
    for (char c : dir)
      quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return exitCodeOf(system(("cd " + quoted + "' && " + command).c_str()));
  }

  int shellAsIs(const std::string& line) { return exitCodeOf(system(line.c_str())); }

  size_t readStream(Stream stream, char* buffer, size_t size)
  {
    for (;;)
    {
      ssize_t n = read(int(stream), buffer, size);
      if (n >= 0)
        return size_t(n);
      if (errno != EINTR)
        return 0;
    }
  }

  void writeStream(Stream stream, const char* data, size_t size)
  {
    for (size_t done = 0; done < size;)
    {
      ssize_t n = write(int(stream), data + done, size - done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return;
      done += size_t(n);
    }
  }

  Stream createLog(const fs::path& path)
  {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    return fd < 0 ? noStream : Stream(fd);
  }

  void takeStandardStreams(Stream& in, Stream& out, Stream& err)
  {
    fflush(stdout);
    fflush(stderr);
    in = Stream(fcntl(0, F_DUPFD_CLOEXEC, 3));
    out = Stream(fcntl(1, F_DUPFD_CLOEXEC, 3));
    err = Stream(fcntl(2, F_DUPFD_CLOEXEC, 3));
    int nul = open("/dev/null", O_RDWR);
    dup2(nul, 0);
    dup2(nul, 1);
    dup2(nul, 2);
    if (nul > 2)
      close(nul);
  }

  bool interactiveInput() { return isatty(fileno(stdin)); }

  int run(const std::string& program, const std::vector<std::string>& arguments, const Run& how, std::string& why)
  {
    std::vector<std::string> argv{program};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return spawnAndWait(argv, how, why);
  }

  int runCommandLine(const std::string& commandLine, const Run& how, std::string& why) { return spawnAndWait({"/bin/sh", "-c", commandLine}, how, why); }

  void onInterrupt(void (*fn)())
  {
    interrupted = fn;
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, onSignal);
  }

  std::string machineDescription()
  {
    std::string cpu = "an unknown processor";
    unsigned threads = std::thread::hardware_concurrency(), cores = 0;
    double memory = 0;
#if defined(__linux__)
    if (FILE* f = fopen("/proc/cpuinfo", "r"))
    {
      char line[1024];
      std::string physical, core;
      std::vector<std::string> seenCores;
      while (fgets(line, sizeof line, f))
      {
        std::string text = line;
        auto value = [&] { size_t colon = text.find(':'); std::string v = colon == std::string::npos ? "" : text.substr(colon + 1); while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1); while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back(); return v; };
        if (text.rfind("model name", 0) == 0 && cpu == "an unknown processor")
          cpu = value();
        else if (text.rfind("physical id", 0) == 0)
          physical = value();
        else if (text.rfind("core id", 0) == 0)
        {
          std::string id = physical + ":" + value();
          if (std::find(seenCores.begin(), seenCores.end(), id) == seenCores.end())
            seenCores.push_back(id);
        }
      }
      fclose(f);
      cores = unsigned(seenCores.size());
    }
    if (FILE* f = fopen("/proc/meminfo", "r"))
    {
      unsigned long long kb = 0;
      if (fscanf(f, "MemTotal: %llu kB", &kb) == 1)
        memory = double(kb) / (1024.0 * 1024);
      fclose(f);
    }
#elif defined(__FreeBSD__) || defined(__APPLE__)
    char name[256] = {};
    size_t size = sizeof name - 1;
    if (sysctlbyname("hw.model", name, &size, nullptr, 0) == 0)
      cpu = name;
    unsigned long long bytes = 0;
    size = sizeof bytes;
    if (sysctlbyname(
#if defined(__APPLE__)
          "hw.memsize",
#else
          "hw.physmem",
#endif
          &bytes, &size, nullptr, 0) == 0)
      memory = double(bytes) / (1024.0 * 1024 * 1024);
#endif
    char line[512];
    if (cores)
      snprintf(line, sizeof line, "%s, %u cores, %u threads, %.0f GB", cpu.c_str(), cores, threads, memory);
    else
      snprintf(line, sizeof line, "%s, %u threads, %.0f GB", cpu.c_str(), threads, memory);
    return line;
  }

  double machineBusySeconds()
  {
#if defined(__linux__)
    FILE* f = fopen("/proc/stat", "r");
    if (!f)
      return -1;
    unsigned long long v[8] = {};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
      return -1;
    unsigned long long busy = 0;
    for (int i = 0; i < n; ++i)
      if (i != 3 && i != 4) // (idle, iowait)
        busy += v[i];
    long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? double(busy) / double(hz) : -1;
#else
    return -1;
#endif
  }

  double groupCpuSeconds(const PriorityGroup&) { return -1; }

  int machineLoad()
  {
#if defined(__linux__)
    // /proc/stat's first line: the jiffies of user, nice, system, idle, iowait, irq, softirq, steal.
    auto sample = [](unsigned long long& idle, unsigned long long& all)
    {
      idle = all = 0;
      FILE* f = fopen("/proc/stat", "r");
      if (!f)
        return false;
      unsigned long long v[8] = {};
      int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
      fclose(f);
      if (n < 4)
        return false;
      for (int i = 0; i < n; ++i)
        all += v[i];
      idle = v[3] + (n > 4 ? v[4] : 0);
      return true;
    };
    unsigned long long idle[2], all[2];
    if (!sample(idle[0], all[0]))
      return -1;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!sample(idle[1], all[1]))
      return -1;
    unsigned long long total = all[1] - all[0], idled = idle[1] - idle[0];
    return total ? int(100.0 * double(total - idled) / double(total) + 0.5) : 0;
#elif defined(__FreeBSD__)
    // kern.cp_time: user, nice, system, interrupt, idle.
    auto sample = [](unsigned long long& idle, unsigned long long& all)
    {
      long times[5] = {};
      size_t size = sizeof times;
      if (sysctlbyname("kern.cp_time", times, &size, nullptr, 0) != 0)
        return false;
      all = 0;
      for (long t : times)
        all += (unsigned long long)t;
      idle = (unsigned long long)times[4];
      return true;
    };
    unsigned long long idle[2], all[2];
    if (!sample(idle[0], all[0]))
      return -1;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!sample(idle[1], all[1]))
      return -1;
    unsigned long long total = all[1] - all[0], idled = idle[1] - idle[0];
    return total ? int(100.0 * double(total - idled) / double(total) + 0.5) : 0;
#else
    return -1;
#endif
  }

  // flock: shared by every run, exclusive for the cleanup. A lock taken on a file the cleanup deleted
  // meanwhile (the path now another file, or none) is let go and taken again.
  namespace
  {
    intptr_t lockFile(const fs::path& path, int how)
    {
      for (int attempt = 0; attempt < 2; ++attempt)
      {
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
          return -1;
        if (flock(fd, how | LOCK_NB) != 0)
        {
          close(fd);
          return -1;
        }
        struct stat held{}, now{};
        if (fstat(fd, &held) == 0 && stat(path.c_str(), &now) == 0 && held.st_ino == now.st_ino && held.st_dev == now.st_dev)
          return fd;
        close(fd);
      }
      return -1;
    }
  }

  bool FileLock::shared(const fs::path& path)
  {
    release();
    handle_ = lockFile(path, LOCK_SH);
    if (handle_ == -1)
      return false;
    path_ = path;
    return true;
  }

  // Alone, the file goes when let go, as Windows' FILE_FLAG_DELETE_ON_CLOSE has it: remembered by a
  // negative handle.
  bool FileLock::alone(const fs::path& path)
  {
    release();
    intptr_t fd = lockFile(path, LOCK_EX);
    if (fd == -1)
      return false;
    handle_ = -2 - fd, path_ = path;
    return true;
  }

  void FileLock::touch()
  {
    if (handle_ >= 0)
      futimens(int(handle_), nullptr);
  }

  void FileLock::release()
  {
    if (handle_ >= 0)
      close(int(handle_));
    else if (handle_ < -1)
    {
      unlink(path_.c_str());
      close(int(-2 - handle_));
    }
    handle_ = -1;
  }
#endif

  fs::path findOnPath(const std::string& name)
  {
    const char* path = getenv("PATH");
    if (!path)
      return {};
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::string list = path, file = name + executableSuffix();
    for (size_t at = 0; at <= list.size();)
    {
      size_t end = std::min(list.find(separator, at), list.size());
      if (end > at)
      {
        std::error_code ec;
        fs::path candidate = fs::path(list.substr(at, end - at)) / file;
        if (fs::is_regular_file(candidate, ec))
          return candidate;
      }
      at = end + 1;
    }
    return {};
  }

  FileLock::FileLock(FileLock&& other) noexcept : path_(std::move(other.path_)), handle_(other.handle_) { other.handle_ = -1; }

  FileLock& FileLock::operator=(FileLock&& other) noexcept
  {
    if (this != &other)
    {
      release();
      path_ = std::move(other.path_);
      handle_ = other.handle_;
      other.handle_ = -1;
    }
    return *this;
  }
}
