#include "run.hpp"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// The build is a kppc process of its own (`kppc OPTIONS --server --quiet --exe EXE SOURCE`: a client
// of the compile server, which it starts when none runs; --no-server builds in that process instead),
// so that it ends the way every build ends (a build
// in this process exits on the spot when done, server.hpp's buildDone). The executable goes to
// %TEMP%\kpp-run\<name>-<hash>\, one directory per source and options, where the objects of the
// modules the next build finds unchanged stay.

namespace
{
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

  // Runs the program with the arguments, its standard handles this process's, and returns its exit
  // code (-1 when it cannot be started).
  // With reportedError, the program's output and errors pass through pipes on their way, and
  // reportedError says whether a line of them had ": error:".
  int runAndWait(const std::string& program, const std::vector<std::string>& args, bool* reportedError = nullptr)
  {
    std::string command = quoted(program);
    for (const std::string& a : args)
      command += " " + quoted(a);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    HANDLE reads[2] = {nullptr, nullptr}, writes[2] = {nullptr, nullptr};
    std::string seen[2];
    std::thread readers[2];
    if (reportedError)
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
    bool started = CreateProcessA(program.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    DWORD startError = GetLastError();
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
            seen[k].append(buffer, n);
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
      fprintf(stderr, "kppc --run: cannot start %s (error %lu)\n", program.c_str(), startError);
      return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    for (std::thread& t : readers)
      if (t.joinable())
        t.join();
    if (reportedError)
      *reportedError = seen[0].find(": error:") != std::string::npos || seen[1].find(": error:") != std::string::npos;
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
  }
}

int runProgram(int argc, char** argv, int runIndex)
{
  // --run SOURCE ARGS..., or --test SOURCE... [-- ARGS...]
  bool test = std::string(argv[runIndex]) == "--test";
  std::vector<std::string> options(argv + 1, argv + runIndex), sources, programArgs;
  int i = runIndex + 1;
  if (test)
  {
    for (; i < argc && std::string(argv[i]) != "--"; ++i)
      sources.push_back(argv[i]);
    i += i < argc; // the "--"
  }
  else if (i < argc)
    sources.push_back(argv[i++]);
  programArgs.assign(argv + i, argv + argc);
  if (sources.empty())
  {
    fprintf(stderr, test ? "usage: kppc [options] --test <file-or-dir>... [-- runner arguments]\n"
                         : "usage: kppc [options] --run <file-or-dir> [program arguments]\n");
    return 2;
  }

  std::error_code ec;
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const std::string& bytes)
  {
    for (unsigned char c : bytes)
      h = (h ^ c) * 1099511628211ull;
    h = (h ^ 0xff) * 1099511628211ull;
  };
  for (const std::string& source : sources)
  {
    std::filesystem::path absolute = std::filesystem::absolute(source, ec).lexically_normal();
    if (!std::filesystem::exists(absolute, ec))
    {
      fprintf(stderr, "kppc %s: %s not found\n", argv[runIndex], source.c_str());
      return 2;
    }
    mix(absolute.string());
  }
  if (test)
    options.push_back("--tests");
  // Through the compile server unless --no-server: it is faster after an edit too since 6812a62
  // (docs/design.md, "Running and testing Kpp code").
  auto noServer = std::find(options.begin(), options.end(), "--no-server");
  if (noServer != options.end())
    options.erase(noServer);
  else if (std::find(options.begin(), options.end(), "--server") == options.end())
    options.push_back("--server");
  for (const std::string& o : options)
    mix(o);
  char hex[17];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
  std::string name = std::filesystem::absolute(sources[0], ec).lexically_normal().stem().string();
  if (name.empty())
    name = "program";
  if (test)
    name += "-tests";
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "kpp-run" / (name + "-" + hex);
  std::filesystem::create_directories(dir, ec);
  std::string exe = (dir / (name + ".exe")).string();

  char kppc[MAX_PATH * 4];
  DWORD n = GetModuleFileNameA(nullptr, kppc, DWORD(sizeof kppc));
  std::vector<std::string> build = options;
  for (const char* a : {"--quiet", "--exe"})
    build.push_back(a);
  build.push_back(exe);
  build.insert(build.end(), sources.begin(), sources.end());
  // A build that printed an error does not run, whatever its exit code said: a declaration the
  // parser could not read is left out of the program, and a test in it would silently not run.
  bool reportedError = false;
  int built = runAndWait(std::string(kppc, n), build, &reportedError);
  if (built != 0)
    return built < 0 ? 1 : built;
  if (reportedError)
  {
    fprintf(stderr, "kppc %s: the build reported an error; not running it\n", argv[runIndex]);
    return 1;
  }
  return runAndWait(exe, programArgs);
}
