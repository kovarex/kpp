// kpp measure (docs/build.md, "kpp measure"): a project's builds timed on this machine.
// (Moved out of project.cpp on 2026-11-05 without a change.)
#include "Project/Internal.hpp"

namespace projects
{
  // kpp measure (docs/build.md, "kpp measure"): the files it edits, as they were, put back at the end,
  // on an error and on Ctrl+C (the console's handler, on a thread of its own).
  std::mutex measuredLock;

  std::vector<std::pair<fs::path, std::string>> measuredFiles;

  void restoreMeasuredFiles()
  {
    std::lock_guard lock(measuredLock);
    for (const auto& [path, text] : measuredFiles)
      if (readText(path) != text)
        if (FILE* f = host::openFile(path, "wb"))
          fwrite(text.data(), 1, text.size(), f), fclose(f);
  }

  // The machine as the table's heading names it, and how busy it is (host.cpp).
  std::string machine() { return host::machineDescription(); }
  int machineLoad() { return host::machineLoad(); }

  // Runs a command line in directory, its output appended to log: its exit code, and its wall time
  // from start to exit.
  int timed(const std::string& command, const fs::path& directory, const fs::path& log, double& seconds)
  {
    FILE* heading = host::openFile(log, "ab");
    if (!heading)
      return -1;
    fprintf(heading, "\n> %s\n", command.c_str());
    fclose(heading);
    host::Run how;
    how.directory = directory;
    how.appendTo = log;
    std::string why;
    auto start = std::chrono::steady_clock::now();
    int code = host::runCommandLine(command, how, why);
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return code;
  }
  fs::file_time_type writtenAt(const fs::path& path)
  {
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : t;
  }

  // `kpp measure [--rounds N] [--compare DIR]`: the readme's rows on this machine (docs/build.md,
  // "kpp measure"). In rounds, each row in turn: build.kpp's edits (project.measure()) in the cell it
  // names, each made, built, put back and built through the compile server, as `kpp TARGET
  // CONFIGURATION` builds; the clean build of that target, the server stopped and its configuration's
  // outputs deleted, the C and C++ libraries kept; and per executable its whole build, the C and C++
  // libraries deleted too. With --compare, each row's other build (measure().compare) after it.
  int measureProject(const fs::path& root, int argc, char** argv)
  {
    int rounds = 3;
    fs::path compareDirectory;
    for (int i = 2; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--rounds" && i + 1 < argc && atoi(argv[i + 1]) > 0)
        rounds = atoi(argv[++i]);
      else if (arg == "--compare" && i + 1 < argc)
        compareDirectory = fs::absolute(argv[++i]);
      else
      {
        fprintf(stderr, "error: kpp measure takes --rounds N and --compare DIR, not %s (docs/build.md, \"kpp measure\")\n", arg.c_str());
        return 2;
      }
    }
    Request request;
    request.server = false; // (no server left running for the description: the measurement starts its own)
    Description d;
    if (!describe(root, request, d))
      return 1;
    const Measurement& m = d.measurement;
    std::vector<std::string> targets = targetNames(d);
    if (targets.empty())
    {
      fprintf(stderr, "error: %s describes no executable to measure\n", (root / "build.kpp").string().c_str());
      return 1;
    }
    std::string target = m.target.empty() ? targets[0] : m.target, configuration = m.configuration.empty() ? "debug" : m.configuration;
    const Program* cell = findCell(d, target, configuration);
    if (!cell)
    {
      fprintf(stderr, "error: build.kpp's measure() names %s %s, which it does not describe\n", target.c_str(), configuration.c_str());
      return 1;
    }
    // The files edited: each holding its text exactly once, and without changes of its own (git).
    std::vector<std::string> problems;
    std::vector<std::pair<fs::path, std::string>> files;
    for (const Measurement::Edit& e : m.edits)
    {
      fs::path path = (root / e.file).make_preferred();
      std::string text;
      if (!readFile(path, text))
      {
        problems.push_back(path.string() + " is not there");
        continue;
      }
      size_t at = text.find(e.from);
      if (e.from.empty() || at == std::string::npos || text.find(e.from, at + 1) != std::string::npos)
        problems.push_back(path.string() + " does not hold \"" + e.from + "\" exactly once");
      std::string status;
      if (runAndRead("git -C " + quoteArg(root.string()) + " status --porcelain -- " + quoteArg(e.file), status) != 0)
        problems.push_back("git could not tell whether " + path.string() + " has changes of its own (kpp measure needs git and a git checkout)");
      else if (!status.empty())
        problems.push_back(path.string() + " has changes of its own (git status): commit or restore it first");
      files.push_back({path, text});
    }
    for (const Measurement::Compare& c : m.compares)
      if (!compareDirectory.empty() && !c.touch.empty() && !fs::exists(compareDirectory / c.touch))
        problems.push_back((compareDirectory / c.touch).string() + " is not there (measure().compare's file to touch)");
    if (!compareDirectory.empty() && !fs::is_directory(compareDirectory))
      problems.push_back("--compare " + compareDirectory.string() + " is no directory");
    if (!problems.empty())
    {
      fprintf(stderr, "error: kpp measure built and changed nothing:\n");
      for (const std::string& p : problems)
        fprintf(stderr, "  %s\n", p.c_str());
      return 1;
    }
    auto compareOf = [&](const std::string& row) -> const Measurement::Compare*
    {
      if (compareDirectory.empty())
        return nullptr;
      for (const Measurement::Compare& c : m.compares)
        if (c.row == row)
          return &c;
      return nullptr;
    };

    std::string self = compilerPath().string(), commit;
    runAndRead("git -C " + quoteArg(root.string()) + " log -1 --format=\"%h (%cs)\"", commit);
    while (!commit.empty() && (commit.back() == '\n' || commit.back() == '\r'))
      commit.pop_back();
    std::string version = std::string("kpp ") + kppVersion();
    printf("%s, commit %s\n%s\n%s at %s, cell %s %s, %d round%s\n", version.c_str(), kppCommit(), machine().c_str(), root.string().c_str(),
           commit.c_str(), target.c_str(), configuration.c_str(), rounds, rounds == 1 ? "" : "s");
    for (const Measurement::Edit& e : m.edits)
      printf("edits, and puts back after each build: %s\n  \"%s\"\n  -> \"%s\"\n", (root / e.file).make_preferred().string().c_str(), e.from.c_str(), e.to.c_str());
    printf("clean build: the compile server stopped, build\\%s deleted, the C and C++ libraries (build\\clib) kept\n", configuration.c_str());
    printf("whole build: build\\%s and build\\clib deleted too\n", configuration.c_str());
    if (!compareDirectory.empty())
      printf("compared with: %s\n", compareDirectory.string().c_str());
    fs::path log = root / "build" / "measure.txt";
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    fs::remove(log, ec);
    printf("each build's output: %s\n\n", log.string().c_str());
    fflush(stdout);

    {
      std::lock_guard lock(measuredLock);
      measuredFiles = files;
    }
    host::onInterrupt(restoreMeasuredFiles);
    struct PutBack
    {
      ~PutBack() { restoreMeasuredFiles(); }
    } putBack;

    // The rows: per edit, it and its undoing; the clean build; per executable its whole build.
    struct Row
    {
      std::string name, compare;
      std::vector<double> kpp, other;
    };
    std::vector<Row> rows;
    for (const Measurement::Edit& e : m.edits)
    {
      rows.push_back({e.row, e.row});
      rows.push_back({e.row + ", undone", ""});
    }
    rows.push_back({"Clean build of " + target + ", libraries already built", "clean"});
    for (const std::string& t : targets)
      rows.push_back({"Whole build of " + exeOf(root, *findCell(d, t, configuration), request).filename().string() + ", every library included", "whole " + t});

    auto fail = [&](const std::string& what)
    {
      fprintf(stderr, "error: %s (the build's output: %s)\n", what.c_str(), log.string().c_str());
      return 1;
    };
    // A build of TARGET as a user runs it: its wall time, and that it wrote the executable anew.
    auto build = [&](const std::string& t, Row* row) -> bool
    {
      fs::path exe = exeOf(root, *findCell(d, t, configuration), request);
      fs::file_time_type before = writtenAt(exe);
      double seconds = 0;
      int code = timed(quoteArg(self) + " " + quoteArg(t) + " " + quoteArg(configuration), root, log, seconds);
      if (code != 0)
        return fail("kpp " + t + " " + configuration + " failed (exit " + std::to_string(code) + ")"), false;
      if (row)
      {
        if (writtenAt(exe) <= before)
          return fail("kpp " + t + " " + configuration + " wrote no new " + exe.string() + ": not a build of the change"), false;
        row->kpp.push_back(seconds);
        printf("  %-62s %7.2f s\n", row->name.c_str(), seconds);
        fflush(stdout);
      }
      return true;
    };
    auto compare = [&](Row& row) -> bool
    {
      const Measurement::Compare* c = compareOf(row.compare);
      if (!c)
        return true;
      if (!c->touch.empty())
        fs::last_write_time(compareDirectory / c->touch, fs::file_time_type::clock::now(), ec);
      double seconds = 0;
      int code = timed("cmd.exe /d /s /c \"" + c->command + "\"", compareDirectory, log, seconds);
      if (code != 0)
        return fail("the compared build of \"" + row.name + "\" failed (exit " + std::to_string(code) + "): " + c->command), false;
      row.other.push_back(seconds);
      printf("  %-62s %7.2f s (compared)\n", row.name.c_str(), seconds);
      fflush(stdout);
      return true;
    };
    auto stopAll = [&]() -> bool
    {
      double seconds = 0;
      return timed(quoteArg(self) + " --stop-server", root, log, seconds) == 0 || (fail("kpp --stop-server failed"), false);
    };
    auto erase = [&](const fs::path& directory) -> bool
    {
      fs::remove_all(directory, ec);
      return !fs::exists(directory) || (fail("could not delete " + directory.string() + " (a program holding a file in it?)"), false);
    };

    std::vector<int> loads;
    for (int round = 1; round <= rounds; ++round)
    {
      loads.push_back(machineLoad());
      if (loads.back() < 0)
        printf("round %d (how busy the machine was before it: unknown on this host)\n", round);
      else
        printf("round %d (the machine %d%% busy before it)\n", round, loads.back());
      fflush(stdout);
      // The server the edits build through, up to date: not timed.
      if (!build(target, nullptr))
        return 1;
      size_t r = 0;
      for (size_t i = 0; i < m.edits.size(); ++i, r += 2)
      {
        std::string edited = files[i].second;
        edited.replace(edited.find(m.edits[i].from), m.edits[i].from.size(), m.edits[i].to);
        if (FILE* f = host::openFile(files[i].first, "wb"))
          fwrite(edited.data(), 1, edited.size(), f), fclose(f);
        if (!build(target, &rows[r]))
          return 1;
        restoreMeasuredFiles();
        if (!build(target, &rows[r + 1]) || !compare(rows[r]))
          return 1;
      }
      if (!stopAll() || !erase(root / "build" / configuration) || !build(target, &rows[r]) || !compare(rows[r]))
        return 1;
      ++r;
      for (const std::string& t : targets)
      {
        if (!stopAll() || !erase(root / "build" / configuration) || !erase(root / "build" / "clib") || !build(t, &rows[r]) || !compare(rows[r]))
          return 1;
        ++r;
      }
    }
    stopAll();

    // The table, to paste into the readme: each row's average over the rounds (kovarex, 2026-11-03:
    // "Don't put the range, just make an average"); each round's time is in the lines above.
    auto average = [](const std::vector<double>& values) -> std::string
    {
      if (values.empty())
        return "";
      double sum = 0;
      for (double v : values)
        sum += v;
      char text[64];
      snprintf(text, sizeof text, "%.1f s", sum / double(values.size()));
      return text;
    };
    std::string loadText;
    for (int load : loads)
      loadText += (loadText.empty() ? "" : ", ") + (load < 0 ? std::string("?") : std::to_string(load) + "%");
    printf("\n%s (%s), %s; %s %s, the machine %s busy before each round; wall time from start to exit, average of %d round%s\n\n",
           version.c_str(), std::string(kppCommit()).substr(0, 8).c_str(), machine().c_str(), target.c_str(), configuration.c_str(),
           loadText.c_str(), rounds, rounds == 1 ? "" : "s");
    bool compared = !compareDirectory.empty();
    printf(compared ? "| | %s | compared |\n|---|---:|---:|\n" : "| | %s |\n|---|---:|\n", version.c_str());
    for (const Row& row : rows)
      if (compared)
        printf("| %s | %s | %s |\n", row.name.c_str(), average(row.kpp).c_str(), average(row.other).c_str());
      else
        printf("| %s | %s |\n", row.name.c_str(), average(row.kpp).c_str());
    return 0;
  }
}
