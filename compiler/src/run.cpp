#include "run.hpp"
#include "host.hpp"
#include "target.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// The build is a kpp process of its own (`kpp OPTIONS --server --quiet --exe EXE SOURCE`: a client
// of the compile server, which it starts when none runs; --no-server builds in that process instead),
// so that it ends the way every build ends (a build
// in this process exits on the spot when done, server.hpp's buildDone). The executable goes to
// %TEMP%\kpp-run\<name>-<hash>\, one directory per source and options, where the objects of the
// modules the next build finds unchanged stay. The cache keeps the most recently used directories
// (keptRunDirectories); a directory in use is never deleted (its lock file, below).

namespace
{
  // Every thread's and worktree's runs share %TEMP%\kpp-run: 734 directories and 191,078 files on
  // 2026-11-03, when the kernel's record of each cached file filled its pool. A directory takes some
  // 15 files at four modules.
  constexpr size_t keptRunDirectories = 100;

  // A run holds <dir>\lock open, shared for reading and writing but not for deleting, from before
  // its build until its program has ended, and sets its last write time, the time of last use the
  // cleanup orders by. The cleanup deletes a directory only while it holds the lock file alone, so a
  // run waits here while one is being deleted, and then makes it again.
  host::FileLock lockRunDirectory(const std::filesystem::path& dir)
  {
    for (int attempt = 0; attempt < 400; ++attempt)
    {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      host::FileLock lock;
      if (lock.shared(dir / "lock"))
      {
        lock.touch();
        return lock;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return host::FileLock();
  }

  // Deletes the directories of root beyond the keptRunDirectories most recently used, each while
  // holding its lock file alone (one a run holds is skipped). A directory without a lock file was
  // made by a kpp from before the lock, whose build may still run in it: it goes only once it has
  // been an hour unchanged.
  void trimRunDirectories(const std::filesystem::path& root)
  {
    struct Entry
    {
      std::filesystem::path dir;
      std::filesystem::file_time_type used;
      bool locked;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
      if (!it->is_directory(ec))
        continue;
      std::error_code lockEc;
      std::filesystem::file_time_type lockTime = std::filesystem::last_write_time(it->path() / "lock", lockEc);
      if (!lockEc)
        entries.push_back({it->path(), lockTime, true});
      else
        entries.push_back({it->path(), std::filesystem::last_write_time(it->path(), lockEc), false});
    }
    if (entries.size() <= keptRunDirectories)
      return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used > b.used; });
    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now();
    for (size_t i = keptRunDirectories; i < entries.size(); ++i)
    {
      const Entry& entry = entries[i];
      if (!entry.locked && now - entry.used < std::chrono::hours(1))
        continue;
      std::filesystem::path lockPath = entry.dir / "lock";
      host::FileLock lock;
      if (!lock.alone(lockPath))
        continue;
      std::vector<std::filesystem::path> contents;
      std::error_code listEc;
      for (std::filesystem::directory_iterator it(entry.dir, listEc), end; !listEc && it != end; it.increment(listEc))
        if (it->path().filename() != "lock")
          contents.push_back(it->path());
      for (const std::filesystem::path& path : contents)
        std::filesystem::remove_all(path, listEc);
      lock.release();
      std::filesystem::remove(entry.dir, listEc); // (RemoveDirectory's: only when empty)
    }
  }

  // Runs the program with the arguments, its standard handles this process's, and returns its exit
  // code (-1 when it cannot be started).
  // With reportedError, the program's output and errors pass through pipes on their way, and
  // reportedError says whether a line of them had ": error:".
  int runAndWait(const std::string& program, const std::vector<std::string>& args, bool* reportedError = nullptr)
  {
    std::string seen[2];
    host::Run how;
    if (reportedError)
      how.seen = [&](int k, const char* data, size_t size) { seen[k].append(data, size); };
    std::string why;
    int code = host::run(program, args, how, why);
    if (code == -1 && !why.empty())
    {
      fprintf(stderr, "kpp --run: cannot start %s (%s)\n", program.c_str(), why.c_str());
      return -1;
    }
    if (reportedError)
      *reportedError = seen[0].find(": error:") != std::string::npos || seen[1].find(": error:") != std::string::npos;
    return code;
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
  // (UTF-8, as the program's argv is: on Windows kpp's own argv is the ANSI code page's)
  std::string notText;
  if (!host::argumentsAsUtf8(argc, argv, i, programArgs, notText))
  {
    fprintf(stderr, "kpp %s: the program's %s\n", argv[runIndex], notText.c_str());
    return 2;
  }
  if (sources.empty())
  {
    fprintf(stderr, test ? "usage: kpp [options] --test <file-or-dir>... [-- runner arguments]\n"
                         : "usage: kpp [options] --run <file-or-dir> [program arguments]\n");
    return 2;
  }

  // --no-run: build only, and name the executable. A program for another --target does not run here:
  // without --no-run that is an error before the build, never a build that silently runs nothing.
  auto noRunAt = std::find(options.begin(), options.end(), "--no-run");
  bool noRun = noRunAt != options.end();
  if (noRun)
    options.erase(noRunAt);
  // --stop-server: the compile server these arguments' build goes through is stopped, nothing built (its
  // key is the build's arguments, the executable's path in them, which this option must not change).
  auto stopAt = std::find(options.begin(), options.end(), "--stop-server");
  bool stop = stopAt != options.end();
  if (stop)
    options.erase(stopAt);
  Target target = hostTarget();
  for (size_t k = 0; k + 1 < options.size(); ++k)
    if (options[k] == "--target")
    {
      std::string why;
      if (!targetNamed(options[k + 1], target, why))
      {
        fprintf(stderr, "--target %s %s\n", options[k + 1].c_str(), why.c_str());
        return 2;
      }
    }
  if ((platformOf(target.os).name != std::string(host::platformName()) || target.arch != hostTarget().arch) && !noRun && !stop)
  {
    fprintf(stderr, "kpp %s: a %s program does not run on this machine; --no-run builds it and names the executable\n",
            argv[runIndex], target.name.c_str());
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
      fprintf(stderr, "kpp %s: %s not found\n", argv[runIndex], source.c_str());
      return 2;
    }
    mix(absolute.string());
    // A project's directory (a library's): its build.kpp describes the build and is not one of
    // its sources.
    if (std::filesystem::is_regular_file(absolute / "build.kpp", ec))
    {
      options.push_back("--exclude");
      options.push_back((absolute / "build.kpp").string());
    }
  }
  if (test)
    options.push_back("--tests");
  // Four modules unless the options say otherwise, as the spec runner builds a snippet: kpp's default
  // (four per thread) made some 385 files of objects and IR a build, every one kept in the cache.
  if (std::find(options.begin(), options.end(), "--modules") == options.end())
  {
    options.push_back("--modules");
    options.push_back("4");
  }
  // Through the compile server unless --no-server: it is faster after an edit too since 6812a62
  // (docs/design.md, "Running and testing kpp code").
  auto noServer = std::find(options.begin(), options.end(), "--no-server");
  if (noServer != options.end())
    options.erase(noServer);
  else if (std::find(options.begin(), options.end(), "--server") == options.end() && host::compileServer)
    options.push_back("--server"); // (a host without the compile server yet builds in the kpp it starts)
  for (const std::string& o : options)
    mix(o);
  char hex[17];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
  std::string name = std::filesystem::absolute(sources[0], ec).lexically_normal().stem().string();
  if (name.empty())
    name = "program";
  if (test)
    name += "-tests";
  std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "kpp-run";
  std::filesystem::path dir = root / (name + "-" + hex);
  if (stop)
  {
    std::vector<std::string> args;
    for (const std::string& o : options)
      args.push_back(o == "--server" ? "--stop-server" : o);
    if (std::find(args.begin(), args.end(), "--stop-server") == args.end())
      args.push_back("--stop-server"); // (--no-server: none runs, and the server says so)
    for (const char* a : {"--quiet", "--exe"})
      args.push_back(a);
    args.push_back((dir / (name + (target.windows() ? ".exe" : ""))).string());
    args.insert(args.end(), sources.begin(), sources.end());
    int code = runAndWait(host::executablePath().string(), args);
    return code < 0 ? 1 : code;
  }
  host::FileLock lock = lockRunDirectory(dir);
  if (!lock.held())
  {
    fprintf(stderr, "kpp %s: cannot lock %s\n", argv[runIndex], (dir / "lock").string().c_str());
    return 1;
  }
  // Once the program has ended (or the build failed): the cache is trimmed, this directory among the
  // most recent, and its lock let go.
  struct Unlock
  {
    ~Unlock()
    {
      trimRunDirectories(root);
      lock.release();
    }

    host::FileLock& lock;
    std::filesystem::path root;
  } unlock{lock, root};
  std::string exe = (dir / (name + (target.windows() ? ".exe" : ""))).string(); // (NAME alone for Linux and FreeBSD)

  std::vector<std::string> build = options;
  for (const char* a : {"--quiet", "--exe"})
    build.push_back(a);
  build.push_back(exe);
  build.insert(build.end(), sources.begin(), sources.end());
  // A build that printed an error does not run, whatever its exit code said: a declaration the
  // parser could not read is left out of the program, and a test in it would silently not run.
  bool reportedError = false;
  int built = runAndWait(host::executablePath().string(), build, &reportedError);
  if (built != 0)
    return built < 0 ? 1 : built;
  if (reportedError)
  {
    fprintf(stderr, "kpp %s: the build reported an error; not running it\n", argv[runIndex]);
    return 1;
  }
  if (noRun)
  {
    printf("kpp %s: built %s (not run: --no-run)\n", argv[runIndex], exe.c_str());
    return 0;
  }
  return runAndWait(exe, programArgs);
}
