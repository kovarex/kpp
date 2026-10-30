#include "run.hpp"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// The build is a kppc process of its own (`kppc OPTIONS --quiet --exe EXE SOURCE`, or a client of
// the compile server when OPTIONS has --server), so that it ends the way every build ends (a build
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
  int runAndWait(const std::string& program, const std::vector<std::string>& args)
  {
    std::string command = quoted(program);
    for (const std::string& a : args)
      command += " " + quoted(a);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(program.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
    {
      fprintf(stderr, "kppc --run: cannot start %s (error %lu)\n", program.c_str(), GetLastError());
      return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
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
  int built = runAndWait(std::string(kppc, n), build);
  if (built != 0)
    return built < 0 ? 1 : built;
  return runAndWait(exe, programArgs);
}
