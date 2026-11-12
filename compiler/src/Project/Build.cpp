// A cell's build: kpp's arguments, the build (kpp, the C libraries alongside, the link, the steps
// around), running it and its tests, and kpp's commands on a project (act).
// (Moved out of project.cpp on 2026-11-05 without a change.)
#include "Project/Internal.hpp"

namespace projects
{
  // One kpp compile: through the compile server (runClient), or a kpp process of its own. Never
  // compile() in this process: a build that is no server's ends its process when it is done.
  int invoke(std::vector<std::string> args, bool server, CompileFunction compile, bool stop)
  {
    if (stop)
    {
      std::vector<char*> argv;
      for (std::string& a : args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      return stopServer(int(args.size()), argv.data());
    }
    if (!server)
    {
      std::string command;
      for (const std::string& a : args)
        command += (command.empty() ? "" : " ") + quoteArg(a);
      return host::shell(command);
    }
    std::vector<char*> argv;
    for (std::string& a : args)
      argv.push_back(a.data());
    argv.push_back(nullptr);
    return runClient(int(args.size()), argv.data(), compile);
  }

  std::string withExe(std::string command, const fs::path& exe, const std::string& configuration)
  {
    for (auto [key, value] : {std::pair<std::string, std::string>{"{exe}", exe.string()}, {"{configuration}", configuration}})
      for (size_t at; (at = command.find(key)) != std::string::npos;)
        command.replace(at, key.size(), value);
    return command;
  }

  // What a C library's __FILE__ (an SDL_assert's, a LOG(FATAL)'s) says of the paths under the project:
  // relative to it, and under a kpp library's root the library's name, as kpp's --file-prefix-map
  // writes kpp's own paths in a release build, so that no binary holds a path of this machine. Clang's
  // -ffile-prefix-map, the longest directory first (clang's own order). In every configuration: they
  // share the C libraries' objects (build\clib), which have no debug information to open files by.
  std::string libraryPrefixMap(const fs::path& root, const Program& p, bool gnuDriver)
  {
    bool gnu = gnuDriver;
    std::string flags;
    auto add = [&](const fs::path& from, const std::string& to)
    {
      std::error_code ec;
      std::string dir = fs::absolute(from, ec).lexically_normal().string();
      if (dir.empty())
        return;
      if (dir.back() != '\\' && dir.back() != '/')
        dir += '\\';
      std::string flag = "-ffile-prefix-map=" + dir + "=" + to; // (TO ends in no backslash: quoteArg's quote would be escaped)
      flags += " " + quoteArg(gnu ? flag : "/clang:" + flag);
    };
    add(root, "");
    for (const ProjectLibrary& l : p.kppLibraries)
      for (const std::string& r : l.roots)
        add(rooted(root, r), l.name + "/");
    return flags;
  }

  // A program's --toolchain (source.hpp's selectToolchain): the last of build.kpp's arguments and the
  // command line's, which come after them; empty for the default.
  std::string toolchainOf(const Program& p, const Request& request)
  {
    std::string toolchain;
    for (const std::vector<std::string>* list : {&p.arguments, &request.passed})
      for (size_t i = 0; i + 1 < list->size(); ++i)
        if ((*list)[i] == "--toolchain")
          toolchain = (*list)[i + 1];
    return toolchain;
  }

  // The platform a program is built for, as Library::systemLibrary names it (windows, linux, freebsd):
  // the last --target of build.kpp's arguments and the command line's, the host's without one. One kpp
  // does not know leaves the host's here, and kpp reports it.
  Target programTarget(const Program& p, const Request& request)
  {
    Target target = hostTarget();
    for (const std::vector<std::string>* list : {&p.arguments, &request.passed})
      for (size_t i = 0; i + 1 < list->size(); ++i)
        if ((*list)[i] == "--target")
        {
          std::string why;
          Target named;
          if (targetNamed((*list)[i + 1], named, why))
            target = named;
        }
    return target;
  }

  std::string targetPlatform(const Program& p, const Request& request) { return platformOf(programTarget(p, request).os).name; }

  // The executable a cell builds: build\CONFIGURATION\NAME.exe, NAME alone for Linux and FreeBSD.
  fs::path exeOf(const fs::path& root, const Program& p, const Request& request)
  {
    return root / "build" / p.configuration / (p.name + (targetPlatform(p, request) == "windows" ? ".exe" : ""));
  }

  // kpp's arguments for one cell: its configuration's options and constants (each config a --config,
  // in the order the project, the configuration, the target and configure() set them, so that the
  // later one wins), its own, and then the command line's (`kpp --config N=V` overrides them all).
  std::vector<std::string> buildArguments(const fs::path& root, const Description& d, const Program& p, const Request& request)
  {
    std::vector<std::string> args = {compilerPath().string()};
    if (request.msvc)
      args.push_back("--msvc-diagnostics");
    if (!request.verbose)
      args.push_back("--quiet");
    if (p.debugInfo)
      args.push_back("--debug-info");
    args.push_back(p.optimize ? "--optimize" : "--no-optimize");
    // An unoptimized build for Windows links incrementally with Visual Studio's link.exe; the bundled
    // toolchain's lld links whole, and so does ld.lld for Linux and FreeBSD. (A list, `msvc,bundled`,
    // is the one of it this machine has.)
    std::string toolchain = toolchainOf(p, request);
    // (not with a sanitizer: AddressSanitizer's runtime and link.exe's incremental link do not go together)
    if (!p.optimize && p.sanitizers.empty() && targetPlatform(p, request) == "windows" && toolchain != "bundled" &&
        (toolchain == "msvc" || (selectToolchain(compilerPath(), toolchain).empty() && !bundledToolchain())))
      args.push_back("--incremental-link");
    for (const std::string& c : p.configs)
      args.push_back("--config"), args.push_back(c);
    std::string clangFlags;
    for (const std::string& f : p.clangFlags)
      clangFlags += (clangFlags.empty() ? "" : " ") + f;
    if (!clangFlags.empty())
      args.push_back("--clang-flags"), args.push_back(clangFlags);
    std::string sanitizers;
    for (const std::string& s : p.sanitizers)
      if (("," + sanitizers + ",").find("," + s + ",") == std::string::npos)
        sanitizers += (sanitizers.empty() ? "" : ",") + s;
    if (!sanitizers.empty())
      args.push_back("--sanitize"), args.push_back(sanitizers);
    for (const std::string& a : p.arguments)
      args.push_back(a);
    // The C import's include directories, another platform's left out (cInclude(path, Platform),
    // kept as "PLATFORM PATH").
    std::string platform = targetPlatform(p, request);
    for (const std::string& c : forPlatform(p.cIncludes, platform))
      args.push_back("--cinclude"), args.push_back(rooted(root, c).string());
    for (const std::string& l : p.libraries)
      args.push_back("--lib"), args.push_back(l[0] == '-' ? l : rooted(root, l).string());
    for (const std::string& n : p.natvis)
      args.push_back("--natvis"), args.push_back(rooted(root, n).string());
    // The system libraries of the kpp libraries it uses (Library::systemLibrary), a library's own for
    // its tests, each once, those of another platform left out.
    std::vector<std::string> systemLibraries;
    std::vector<const std::vector<std::string>*> lists = {&p.systemLibraries};
    for (const ProjectLibrary& l : p.kppLibraries)
      lists.push_back(&l.systemLibraries);
    for (const std::vector<std::string>* list : lists)
      for (const std::string& s : *list)
      {
        size_t blank = s.find(' ');
        std::string name = blank == std::string::npos ? s : s.substr(blank + 1);
        if ((blank == std::string::npos || s.substr(0, blank) == platform) &&
            std::find(systemLibraries.begin(), systemLibraries.end(), name) == systemLibraries.end())
          systemLibraries.push_back(name);
      }
    for (const std::string& name : systemLibraries)
      args.push_back("--lib"), args.push_back("-l" + name);
    for (const std::string& name : p.links)
      if (const CLibrary* c = findLibrary(d, name))
        for (const fs::path& file : libraryFiles(root, *c, libraryFlags(p, request), platform))
          args.push_back("--lib"), args.push_back(file.string());
    // Their C++ files' library off MSVC's Windows (cppRuntimeLibraries: libstdc++ for Linux, libc++
    // for FreeBSD and windows-gnu), once, after them.
    std::vector<std::string> cppRuntime;
    bool gnu = libraryTarget(p, request).gnu;
    for (const std::string& name : p.links)
      if (const CLibrary* c = findLibrary(d, name))
        for (const std::string& lib : cppRuntimeLibraries(root, *c, platform, gnu))
          if (std::find(cppRuntime.begin(), cppRuntime.end(), lib) == cppRuntime.end())
            cppRuntime.push_back(lib);
    for (const std::string& lib : cppRuntime)
      args.push_back("--lib"), args.push_back(lib);
    for (const std::string& e : p.excluded)
      args.push_back("--exclude"), args.push_back(rooted(root, e).string());
    // The kpp libraries it uses, root by root with their names and scopes (design.md 16), before --exe:
    // kpp --test takes what follows it as the sources.
    if (p.thisReference)
      args.push_back("--this-reference");
    for (const ProjectLibrary& l : p.kppLibraries)
    {
      if (l.thisReference)
        args.push_back("--this-reference-library"), args.push_back(l.name);
      for (const std::string& r : l.roots)
        args.push_back(l.unscoped ? "--unscoped-library-root" : "--library-root"), args.push_back(l.name), args.push_back(r);
      for (const std::string& e : l.excluded)
        args.push_back("--exclude"), args.push_back(e);
    }
    // A release or final build (optimized) holds no path of this machine: the paths its text gives (an
    // assertion's message, a CallerLocation's file) are the standard library's as stdlib/..., a kpp
    // library's under its name, the project's relative to its directory (--file-prefix-map); its debug
    // information, in a file of its own, keeps them whole, and the executable names that file without
    // its directory (Link.cpp). A debug build keeps them whole, as its debugger and IDE open them.
    if (p.optimize)
    {
      args.push_back("--file-prefix-map"), args.push_back(defaultStdlib(compilerPath()).string() + "=stdlib");
      for (const ProjectLibrary& l : p.kppLibraries)
        for (const std::string& r : l.roots)
          args.push_back("--file-prefix-map"), args.push_back(rooted(root, r).string() + "=" + l.name);
      args.push_back("--file-prefix-map"), args.push_back(root.string() + "=");
    }
    args.insert(args.end(), request.passed.begin(), request.passed.end());
    args.push_back("--exe");
    args.push_back(exeOf(root, p, request).string());
    for (const std::string& r : p.roots)
      args.push_back(rooted(root, r).string());
    return args;
  }

  // kpp's arguments for a cell's build: buildArguments' with the link left to kpp (buildProgram). The
  // compile server of a build is the one of these arguments: --stop-server stops it by them too.
  std::vector<std::string> buildCommand(const fs::path& root, const Description& d, const Program& p, const Request& request)
  {
    std::vector<std::string> args = buildArguments(root, d, p, request);
    args.insert(args.begin() + 1, "--defer-link");
    return args;
  }

  int buildProgram(const fs::path& root, const Description& d, const Program& p, const Request& request, CompileFunction compile)
  {
    fs::path exe = exeOf(root, p, request);
    std::error_code ec;
    // configure(cell) chose the cell's steps by cell.platform (projectlib's kppCellPlatform): a build for
    // another platform would run them, Windows' cmd steps in a Linux build. They differ when configure
    // adds a --target itself, or when projectlib's reading of a target name is not kpp's.
    if (std::string platform = targetPlatform(p, request); !p.platform.empty() && p.platform != platform)
    {
      fprintf(stderr, "error: %s (%s) is built for %s, but build.kpp's configure(cell) saw cell.platform %s: a --target added in configure(cell) "
              "comes too late for it (give it to the configuration or the target in build()); else projectlib's kppTargetPlatform reads the name otherwise "
              "than kpp\n",
              p.name.c_str(), p.configuration.c_str(), platform.c_str(), p.platform.c_str());
      return 1;
    }
    // The program's toolchain, its C libraries' too (a game that links Visual Studio's C++ libraries
    // asks for --toolchain msvc): one that is not there is said here, before anything compiles.
    std::string toolchain = toolchainOf(p, request);
    if (std::string error = selectToolchain(compilerPath(), toolchain); !error.empty())
    {
      fprintf(stderr, "error: %s (%s): %s\n", p.name.c_str(), p.configuration.c_str(), error.c_str());
      return 1;
    }
    // A list's later toolchain (msvc,bundled where Visual Studio's clang is missing): why the first
    // one is not taken, a warning Visual Studio's Error List shows (a build of the other dialect, whose
    // C imports and libraries differ, is never a silent one).
    if (std::string skipped = toolchainSkipped(); !skipped.empty())
      fprintf(stderr, "warning: %s (%s): --toolchain %s takes %s: %s\n", p.name.c_str(), p.configuration.c_str(), toolchain.c_str(),
              toolchainName().c_str(), skipped.c_str());
    fs::create_directories(exe.parent_path(), ec);
    if (!p.beforeBuild.empty() && host::shellAsIs(withExe(p.beforeBuild, exe, p.configuration)) != 0)
    {
      fprintf(stderr, "error: %s's before-build command failed: %s\n", p.name.c_str(), p.beforeBuild.c_str());
      return 1;
    }
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    auto start = Clock::now();
    // --check (build.kpp's arguments or the command line's): kpp's front end alone, which writes no
    // executable, so neither the C libraries nor the link nor the after-build step run. Reported as the
    // check it is: `kpp factorio --check` said "factorio.exe was not written" after 0 errors (2026-11-08).
    bool checkOnly = false;
    for (const std::vector<std::string>* list : {&p.arguments, &request.passed})
      checkOnly = checkOnly || std::find(list->begin(), list->end(), "--check") != list->end();
    if (checkOnly)
    {
      int code = invoke(buildCommand(root, d, p, request), request.server, compile);
      if (code == 0)
        printf("kpp: checked %s (%s, total %.1f s)\n", p.name.c_str(), p.configuration.c_str(), since(start));
      else
        fprintf(stderr, "kpp: %s (%s) failed its check after %.1f s\n", p.name.c_str(), p.configuration.c_str(), since(start));
      return code;
    }
    double beforeSeconds = std::chrono::duration<double>(start - kppStarted).count();
    std::vector<LibraryJob> jobs(p.links.size());
    // (Linux and FreeBSD libraries are compiled by clang's GNU driver, as the bundled toolchain's are)
    LibraryTarget libTarget = libraryTarget(p, request);
    std::string prefixMap = libraryPrefixMap(root, p, bundledToolchain() || libTarget.gnu || libTarget.posix());
    std::vector<const CLibrary*> cLibraries;
    for (const std::string& name : p.links)
    {
      const CLibrary* c = findLibrary(d, name);
      if (!c)
      {
        fprintf(stderr, "error: %s links %s, which build.kpp declares no C library of\n", p.name.c_str(), name.c_str());
        return 1;
      }
      cLibraries.push_back(c);
    }
    // The C libraries are checked (each object against its source and the headers its .d names) and
    // their changed files compiled in one pool, all while kpp builds the program (its front end, code
    // generation and modules need no library); kpp leaves the link (--defer-link), which runs here
    // once both are done (docs/build.md, "The whole build"). The check alone took 0.13 to 0.19 s of
    // every build of the game before kpp started (2026-11-05).
    bool libraryWork = false, librariesBuilt = true;
    double prepareSeconds = 0, librarySeconds = 0;
    // The C library compiles run below kpp's threads while kpp builds, and at the normal priority once
    // it is done (the link then waits for them alone): factorio.exe's whole build 15.4 to 16.7 s -> 14.9
    // to 15.2 s, kpp's part of it 14.1-14.9 -> 13.5-13.8 s, the CPU the same; below the whole time was no
    // faster on a quiet machine and starved them under another load (one build waited 38.8 s for them
    // after kpp), and 8 compiles at once left the link waiting 0.8 to 2.2 s (codegen-speed.md section 5,
    // E's window of 2026-11-08). KPP_CLIB_PRIORITY=normal: as before, for a measurement.
    std::unique_ptr<host::PriorityGroup> lowered;
    if (const char* priority = getenv("KPP_CLIB_PRIORITY"); !priority || std::string(priority) != "normal")
      lowered = std::make_unique<host::PriorityGroup>();
    std::thread libraries;
    if (!cLibraries.empty())
      libraries = std::thread([&]
      {
        // Each library checked on a thread of its own (the game's 18 took 0.10 to 0.12 s one after
        // another on a quiet machine), each with its own header times (upToDate's, once per header).
        std::vector<char> prepared(cLibraries.size(), 0);
        std::vector<std::string> cFlags = libraryFlags(p, request); // (a debug build's -O0 first)
        std::vector<std::thread> checks;
        for (size_t i = 0; i < cLibraries.size(); ++i)
          checks.emplace_back([&, i]
          {
            std::unordered_map<std::string, fs::file_time_type> times;
            prepared[i] = prepareLibrary(root, *cLibraries[i], cFlags, prefixMap, jobs[i], times, libTarget);
          });
        for (std::thread& t : checks)
          t.join();
        if (std::find(prepared.begin(), prepared.end(), 0) != prepared.end())
        {
          librariesBuilt = false;
          return;
        }
        prepareSeconds = since(start);
        libraryWork = std::any_of(jobs.begin(), jobs.end(), [](const LibraryJob& j) { return j.archive || !j.commands.empty(); });
        if (libraryWork)
          librariesBuilt = runLibraries(jobs, lowered.get());
        librarySeconds = since(start);
      });
    std::vector<std::string> args = buildCommand(root, d, p, request);
    auto kppStart = Clock::now();
    int code = invoke(args, request.server, compile);
    double kppSeconds = since(kppStart);
    // A kpp that crashed says so here too, with its code: its own report (a stack) is above, and the
    // build's exit code is its code (2026-11-12, D: under --no-server it was lost).
#ifdef _WIN32
    bool crashed = unsigned(code) >= 0xC0000000u; // (an NTSTATUS error: an access violation, a stack overflow)
#else
    bool crashed = code > 128; // (128 + the signal, as the shell gives it)
#endif
    if (crashed)
      fprintf(stderr, "error: kpp's build of %s (%s) crashed (exit code 0x%08X)\n", p.name.c_str(), p.configuration.c_str(), unsigned(code));
    if (lowered)
      lowered->raise(); // (the link waits for them now: nothing of kpp's left to give way to)
    if (libraries.joinable())
      libraries.join();
    if (!librariesBuilt)
      code = 1;
    // The link: kpp's command (it marks it pending when it built new objects), also when only a
    // C library changed (kpp replays an unchanged build, which does not link).
    double linkSeconds = 0;
    fs::path pending = exe.string() + ".link.pending", command = exe.string() + ".link.command";
    // Also when what the link writes is gone (the executable, a debug build's .pdb beside it, deleted
    // while the compile server kept the build): kpp's objects are all there (it replays a build only
    // then), so the link makes them again.
    fs::path pdb = fs::path(exe).replace_extension(".pdb");
    bool linked = fs::exists(exe, ec) && !(p.debugInfo && exe.extension() == ".exe" && !fs::exists(pdb, ec));
    // And when a file the link takes besides kpp's objects changed: the program's .natvis files and
    // the libraries it names by path (linkFile, a prebuilt .lib). kpp's replay of an unchanged build
    // does not look at them, so a .natvis edit alone left the PDB with the old rules (2026-11-05,
    // F's Executable::natvis). Each by size and time, kept in <exe>.link.inputs after a link.
    fs::path inputsFile = exe.string() + ".link.inputs";
    std::string inputs;
    {
      std::vector<fs::path> files;
      for (const std::string& n : p.natvis)
        files.push_back(rooted(root, n));
      for (const std::string& l : p.libraries)
        if (!l.empty() && l[0] != '-')
          files.push_back(rooted(root, l));
      for (const fs::path& f : files)
      {
        std::error_code missing;
        auto size = fs::file_size(f, missing);
        auto time = fs::last_write_time(f, missing).time_since_epoch().count();
        inputs += f.string() + "\t" + (missing ? std::string("missing") : std::to_string(size) + "\t" + std::to_string(time)) + "\n";
      }
    }
    bool inputsChanged = readText(inputsFile) != inputs;
    bool linkPending = fs::exists(pending, ec) || (code == 0 && (!linked || inputsChanged));
    // An incremental link's state (the .ilk beside the executable) without the executable or its .pdb
    // goes, and what is left of the two with it: the link starts from nothing. link.exe's incremental
    // link with its PDB deleted crashed now and then, "LNK1000: Internal error during
    // IMAGE::BuildImage.FinalPhase" (CI runs 41825684415 and 41927566409, project.ps1's ".pdb deleted
    // under the compile server").
    fs::path ilk = fs::path(exe).replace_extension(".ilk");
    if (!linked && fs::exists(ilk, ec))
    {
      fs::remove(ilk, ec);
      fs::remove(exe, ec);
      fs::remove(pdb, ec);
    }
    if (librariesBuilt && (linkPending || (code == 0 && libraryWork)) && fs::exists(command, ec))
    {
      auto linkStart = Clock::now();
      // The linker's own words go to <exe>.link.txt when kpp writes the link with --keep-going (factorio-in-kpp's
      // tests): a failed link shows them (kovarex, 2026-11-11: "the link failed (exit 1104)" alone, link.exe's
      // LNK1104 "cannot open file ..." unseen in the file). Cleared first, so that an earlier link's are never shown.
      fs::path linkOutput = exe.string() + ".link.txt";
      fs::remove(linkOutput, ec);
      int rc = host::shell(readText(command));
      linkSeconds = since(linkStart);
      if (rc != 0)
      {
        std::string said = readText(linkOutput);
        size_t shown = 0, lines = 0;
        for (size_t at = 0, end; at < said.size(); at = end + 1)
        {
          end = said.find('\n', at);
          if (end == std::string::npos)
            end = said.size();
          std::string line = said.substr(at, end - at);
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          if (line.empty())
            continue;
          ++lines;
          if (shown < 40)
          {
            fprintf(stderr, "%s\n", line.c_str());
            ++shown;
          }
        }
        if (lines > shown)
          fprintf(stderr, "(%zu more lines in %s)\n", lines - shown, linkOutput.string().c_str());
        fprintf(stderr, "error: %s (%s): the link failed (exit %d)\n", p.name.c_str(), p.configuration.c_str(), rc);
        code = 1;
      }
      else
      {
        fs::remove(pending, ec);
        if (!writeText(inputsFile, inputs, "the stamps of the link's inputs"))
          code = 1;
      }
    }
    // Built means written: never "built" for an executable that is not there.
    if (code == 0 && !fs::exists(exe, ec))
    {
      fprintf(stderr, "error: %s (%s): %s was not written (kpp reported success; no link command to make it)\n", p.name.c_str(),
              p.configuration.c_str(), exe.string().c_str());
      code = 1;
    }
    double seconds = since(start);
    bool keepGoing = std::find(p.arguments.begin(), p.arguments.end(), "--keep-going") != p.arguments.end();
    char split[256];
    if (libraryWork)
      snprintf(split, sizeof split, "C libraries %.1f s alongside kpp %.1f s, link %.1f s", librarySeconds, kppSeconds, linkSeconds);
    else
      snprintf(split, sizeof split, "kpp %.1f s, link %.1f s", kppSeconds, linkSeconds);
    // The toolchain it took (build.kpp's `msvc,bundled` is the one of them this machine has): for Linux
    // and FreeBSD that toolchain's clang, which links with ld.lld and the target's sysroot, neither
    // Visual Studio's link.exe and libraries nor MinGW-w64's.
    std::string platform = targetPlatform(p, request), name = toolchainName();
    std::string used = platform == "windows" ? name + " toolchain"
                                             : platform + ": " + (name == "msvc" ? std::string("Visual Studio's") : name == "bundled" ? std::string("the bundled") : name + "'s") +
                                                 " clang and ld.lld";
    if (code == 0)
      printf("kpp: built %s (%s, %s, total %.1f s: %s)\n", exe.string().c_str(), p.configuration.c_str(), used.c_str(), seconds, split);
    else
      fprintf(stderr, "kpp: %s (%s) failed after %.1f s (%s)\n", p.name.c_str(), p.configuration.c_str(), seconds, split);
    auto afterStart = Clock::now();
    if (!p.afterBuild.empty() && (code == 0 || (keepGoing && fs::exists(exe, ec))) &&
        host::shellAsIs(withExe(p.afterBuild, exe, p.configuration)) != 0)
    {
      fprintf(stderr, "error: %s's after-build command failed: %s\n", p.name.c_str(), p.afterBuild.c_str());
      return 1;
    }
    // KPP_STEP_TIMES=1: where an edit build's time goes besides kpp's and the link's (an environment
    // variable, not an option: an option of its own would give the build a compile server of its own).
    if (getenv("KPP_STEP_TIMES"))
      printf("kpp steps: %.3f s before the build (build.kpp described at %.3f s, its libraries at %.3f s), "
             "kpp %.3f s (the C libraries' check alongside, %.3f s), link %.3f s, %.3f s else, after-build %.3f s\n",
             beforeSeconds, describedAt, usesResolvedAt, kppSeconds, prepareSeconds, linkSeconds, seconds - kppSeconds - linkSeconds, since(afterStart));
    return code;
  }

  Launch launchOf(const fs::path& root, const Program& p, const Request& request)
  {
    fs::path exe = exeOf(root, p, request);
    Launch l;
    l.program = p.debugCommand.empty() ? exe : rooted(root, withExe(p.debugCommand, exe, p.configuration));
    l.directory = p.debugDirectory.empty() ? root : rooted(root, withExe(p.debugDirectory, exe, p.configuration));
    l.arguments = withExe(p.debugArguments, exe, p.configuration);
    return l;
  }

  int runProgram(const fs::path& root, const Program& p, const Request& request, const std::vector<std::string>& extra)
  {
    Launch l = launchOf(root, p, request);
    std::string command = quoteArg(l.program.string());
    if (!l.arguments.empty())
      command += " " + l.arguments;
    for (const std::string& a : extra)
      command += " " + quoteArg(a);
    return host::shell(command, l.directory); // (cd /d DIR && ..., as it was; cd DIR && ... on POSIX)
  }

  // kpp --test: the tests of the executables and libraries asked for (all when none is named) in
  // one configuration, each built and run by kpp --test (run.hpp) with its cell's options and roots,
  // a library's test roots too, and the runner's arguments (after `--`). Every one runs; 1 when one
  // failed.
  int runTests(const fs::path& root, const Description& d, const Configuration& configuration, const std::vector<std::string>& targets,
               const Request& request)
  {
    std::vector<Program> chosen;
    auto asked = [&](const std::string& name)
    { return targets.empty() || std::find(targets.begin(), targets.end(), name) != targets.end(); };
    for (const Program& p : d.programs)
      if (p.configuration == configuration.name && asked(p.name))
        chosen.push_back(p);
    for (const KppLibrary& l : d.libraries)
      if (asked(l.name) && !libraryProgram(root, configuration, l, request, chosen.emplace_back()))
        return 1;
    int result = 0;
    for (const Program& p : chosen)
    {
      std::vector<std::string> args = buildArguments(root, d, p, request);
      size_t exe = std::find(args.begin(), args.end(), "--exe") - args.begin();
      std::vector<std::string> roots(args.begin() + std::min(exe + 2, args.size()), args.end());
      args.resize(exe);
      args.erase(std::remove(args.begin(), args.end(), "--incremental-link"), args.end());
      if (!request.server)
        args.push_back("--no-server");
      size_t runIndex = args.size();
      args.push_back("--test");
      args.insert(args.end(), roots.begin(), roots.end());
      args.push_back("--");
      args.insert(args.end(), request.runArguments.begin(), request.runArguments.end());
      std::vector<char*> argv;
      for (std::string& a : args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      printf("kpp: testing %s (%s)\n", p.name.c_str(), p.configuration.c_str());
      fflush(stdout);
      if (::runProgram(int(args.size()), argv.data(), int(runIndex)) != 0)
        result = 1;
    }
    return result;
  }

  // --clean and --rebuild (a Visual Studio project's Clean and Rebuild, generate.cpp): a cell's kpp build
  // state gone, so that its next build compiles every module. Every compile server whose build writes
  // its executable is stopped (the cell's own, and one started with other options: --verbose), and the
  // files go: the executable, what kpp and the link write beside it under its name (EXE.N.o and .hash,
  // the modules' objects and hashes; .ll; .link.pending, .command, .inputs and the response files;
  // .debug), and under its stem the .pdb, .ilk, .natvis, .lib and .exp. The C libraries' objects stay
  // (build\clib\, shared by the cells: Visual Studio's Rebuild of one project rebuilds no other, and kpp
  // measure's clean build is the same), and so does the opt-in shared object cache (KPP_OBJECT_CACHE,
  // keyed by everything an object is made of). The files removed, and whether a server was stopped.
  struct Cleaned
  {
    size_t files = 0;
    bool server = false;
  };
  Cleaned cleanCell(const fs::path& root, const Program& p, const Request& request)
  {
    Cleaned cleaned;
    fs::path exe = exeOf(root, p, request);
    std::string key = host::pathKey(exe);
    cleaned.server = !stopServers([&](const std::vector<std::string>& args)
                      {
                        for (size_t i = 0; i + 1 < args.size(); ++i)
                          if (args[i] == "--exe" && host::pathKey(fs::path(args[i + 1])) == key)
                            return true;
                        return false;
                      }).empty();
    std::string name = exe.filename().string(), stem = exe.stem().string();
    static const char* const beside[] = {".pdb", ".ilk", ".natvis", ".natvis-files", ".lib", ".exp"};
    std::error_code ec;
    std::vector<fs::path> doomed;
    for (fs::directory_iterator it(exe.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    {
      std::string file = it->path().filename().string();
      bool ours = file == name || file.compare(0, name.size() + 1, name + ".") == 0;
      for (const char* extension : beside)
        ours = ours || file == stem + extension;
      if (ours && it->is_regular_file(ec))
        doomed.push_back(it->path());
    }
    for (const fs::path& f : doomed)
      if (fs::remove(f, ec))
        ++cleaned.files;
      else if (ec)
        fprintf(stderr, "warning: %s: %s could not be removed (%s)\n", p.name.c_str(), f.string().c_str(), ec.message().c_str());
    return cleaned;
  }

  int act(const fs::path& root, const Request& request, CompileFunction compile)
  {
    auto commandStart = std::chrono::steady_clock::now(); // (the whole command's time: build.kpp's description too)
    // --stop-server: the compile servers of build.kpp's description and of every cell's builds. The
    // description's first, and the description then computed without a server, so that a build.kpp
    // that no longer describes stops its server too. Then every other server whose arguments name a
    // file of the project: a build with options of its own (--verbose, --toolchain) has a server of
    // its own, and an earlier build.kpp's cells had theirs.
    if (request.stop)
    {
      std::vector<std::string> stopped;
      auto stop = [&](std::vector<std::string> args, const std::string& what)
      {
        std::vector<char*> argv;
        for (std::string& a : args)
          argv.push_back(a.data());
        argv.push_back(nullptr);
        if (stopServerQuietly(int(args.size()), argv.data()))
          stopped.push_back(what);
      };
      stop(describeArguments(root, request), "build.kpp's description");
      Request serverless = request;
      serverless.server = false;
      Description d;
      bool described = describe(root, serverless, d);
      std::map<std::string, std::vector<std::string>> commands; // each cell's own arguments, by its executable
      if (described)
        for (const Program& p : d.programs)
        {
          std::vector<std::string> args = buildCommand(root, d, p, request);
          stop(args, exeOf(root, p, request).string());
          commands[exeOf(root, p, request).string()].assign(args.begin() + 1, args.end());
        }
      // The project's other servers: those whose arguments name its build.kpp or a file of its build\
      // (a cell's --exe, the description's settings), not merely one under its directory, which a
      // project inside it has too (tests/project.ps1's sections, each a project under one directory: a
      // stop in the outer one stopped an inner one's server, 2026-11-05).
      auto lower = [](std::string s)
      {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
        return s;
      };
      std::string buildFile = lower((root / "build.kpp").string()), buildDirectory = lower((root / "build").string() + "\\");
      for (const std::vector<std::string>& args : stopServers([&](const std::vector<std::string>& args)
           {
             for (const std::string& argument : args)
             {
               std::string a = lower(argument);
               if (a == buildFile || a.compare(0, buildDirectory.size(), buildDirectory) == 0)
                 return true;
             }
             return false;
           }))
      {
        // Which one it was: its --exe (or build.kpp's description), and the options by which its
        // arguments differ from the cell's own today (kpp's --verbose is kpp without --quiet).
        std::string what = "build.kpp's description", options;
        for (size_t i = 0; i + 1 < args.size(); ++i)
          if (args[i] == "--exe")
            what = args[i + 1];
        auto own = commands.find(what);
        if (own != commands.end())
        {
          for (const std::string& a : args)
            if (a.size() > 1 && a[0] == '-' && std::find(own->second.begin(), own->second.end(), a) == own->second.end())
              options += " " + a;
          for (const std::string& a : own->second)
            if (a.size() > 1 && a[0] == '-' && std::find(args.begin(), args.end(), a) == args.end())
              options += a == "--quiet" ? " --verbose" : " without " + a;
        }
        stopped.push_back(what + (own == commands.end()                ? " (a cell build.kpp no longer describes, or other arguments)"
                                  : options.empty()                    ? " (other arguments)"
                                                                       : " (started with" + options + ")"));
      }
      for (const std::string& what : stopped)
        printf("kpp: stopped the compile server of %s\n", what.c_str());
      if (stopped.empty())
        printf("kpp: no compile server of %s runs\n", root.string().c_str());
      return described ? 0 : 1;
    }
    Description d;
    if (!describe(root, request, d))
      return 1;
    describedAt = std::chrono::duration<double>(std::chrono::steady_clock::now() - kppStarted).count();
    if (!resolveUses(root, request, d))
      return 1;
    usesResolvedAt = std::chrono::duration<double>(std::chrono::steady_clock::now() - kppStarted).count();
    if (request.visualStudio || request.vsCode)
      return generate(root, d, request);
    // The names: each a configuration's or a target's (a library's, for --test).
    std::vector<std::string> configurations = request.configurations, targets;
    for (const std::string& name : request.names)
      if (findConfiguration(d, name))
        configurations.push_back(name);
      else if (std::any_of(d.programs.begin(), d.programs.end(), [&](const Program& p) { return p.name == name; }) ||
               std::any_of(d.unavailable.begin(), d.unavailable.end(), [&](const Program& p) { return p.name == name; }) ||
               (request.test && std::any_of(d.libraries.begin(), d.libraries.end(), [&](const KppLibrary& l) { return l.name == name; })))
        targets.push_back(name);
      else
      {
        std::string known;
        for (const Configuration& c : d.configurations)
          known += " " + c.name;
        fprintf(stderr, "error: build.kpp describes no target or configuration %s (its configurations:%s)\n", name.c_str(), known.c_str());
        return 2;
      }
    if (configurations.empty())
      configurations.push_back("debug");
    if (request.test)
    {
      int result = 0;
      for (const std::string& name : configurations)
        result = std::max(result, runTests(root, d, *findConfiguration(d, name), targets, request));
      return result;
    }
    std::vector<const Program*> chosen;
    for (const std::string& configuration : configurations)
      for (const Program& p : d.programs)
        if (p.configuration == configuration && (targets.empty() || std::find(targets.begin(), targets.end(), p.name) != targets.end()))
          chosen.push_back(&p);
    // A cell that does not exist (Cell::unavailable): named, an error with build.kpp's reason; in a
    // build of every target, left out with a notice.
    for (const std::string& configuration : configurations)
      for (const Program& p : d.unavailable)
        if (p.configuration == configuration)
        {
          if (std::find(targets.begin(), targets.end(), p.name) != targets.end())
          {
            fprintf(stderr, "error: %s has no %s configuration: %s\n", p.name.c_str(), configuration.c_str(), p.unavailable.c_str());
            return 2;
          }
          if (targets.empty())
            printf("kpp: %s (%s) left out: %s\n", p.name.c_str(), configuration.c_str(), p.unavailable.c_str());
        }
    // kpp options: each chosen cell's libraries, scoped or unscoped, and their options, with their
    // values and who set them (a library project: its own and its libraries', in each configuration).
    if (request.options)
    {
      std::vector<Program> listed;
      for (const Program* p : chosen)
        listed.push_back(*p);
      if (d.programs.empty())
        for (const std::string& configuration : configurations)
          for (const KppLibrary& l : d.libraries)
            if (!libraryProgram(root, *findConfiguration(d, configuration), l, request, listed.emplace_back()))
              return 1;
      for (const Program& p : listed)
      {
        // The library this project is (its own tests' program), then the libraries the cell uses.
        std::vector<ProjectLibrary> libraries = p.kppLibraries;
        for (const KppLibrary& l : d.libraries)
          if (d.programs.empty() && l.name == p.name)
            libraries.insert(libraries.begin(), ProjectLibrary{l.name, {}, l.unscoped});
        printf("%s %s:%s\n", p.name.c_str(), p.configuration.c_str(), libraries.empty() ? " no libraries" : "");
        for (const ProjectLibrary& l : libraries)
          printf(l.unscoped ? "  library %s, unscoped: its names are bare\n" : "  library %s, scoped: %s::NAME\n", l.name.c_str(),
                 l.name.c_str());
        for (const ProjectOption& o : p.options)
        {
          std::string values;
          for (const std::string& a : o.allowed)
            values += (values.empty() ? " (" : ", ") + a;
          printf("  %s::%s = %s  %s%s, %s; set by %s\n    %s\n", o.library.c_str(), o.name.c_str(), o.value.c_str(), o.type.c_str(),
                 values.empty() ? "" : (values + ")").c_str(), o.required ? "required" : ("default " + o.defaultValue).c_str(),
                 o.setBy.c_str(), o.description.c_str());
        }
      }
      return 0;
    }
    if (request.clean || request.rebuild)
    {
      for (const Program* p : chosen)
      {
        Cleaned c = cleanCell(root, *p, request);
        printf("kpp: cleaned %s (%s): %zu files removed%s\n", p->name.c_str(), p->configuration.c_str(), c.files,
               c.server ? ", its compile server stopped" : "");
      }
      fflush(stdout);
      if (!request.rebuild)
        return 0;
    }
    if (request.run && chosen.size() != 1)
    {
      fprintf(stderr, "error: --run needs one target in one configuration; this names %zu cells\n", chosen.size());
      return 2;
    }
    // A program for another platform does not run here: an error before the build, as kpp --run's
    // (run.cpp), never a build that then fails to start an ELF.
    if (Target t = programTarget(*chosen[0], request); request.run && (platformOf(t.os).name != std::string(host::platformName()) || t.arch != hostTarget().arch))
    {
      fprintf(stderr, "error: %s %s is built for %s, which does not run on this machine: build it without --run (%s)\n",
              chosen[0]->name.c_str(), chosen[0]->configuration.c_str(), t.name.c_str(),
              exeOf(root, *chosen[0], request).string().c_str());
      return 2;
    }
    if (chosen.empty())
      printf("kpp: build.kpp describes no executable; kpp --test builds and runs its library's tests\n");
    for (const Program* p : chosen)
      if (int code = buildProgram(root, d, *p, request, compile))
        return code;
    // The whole command (build.kpp's description, every cell, its C libraries and links), to compare
    // with another build system's.
    if (chosen.size() > 1)
      printf("kpp: built %zu programs, total %.1f s\n", chosen.size(),
             std::chrono::duration<double>(std::chrono::steady_clock::now() - commandStart).count());
    return request.run ? runProgram(root, *chosen[0], request, request.runArguments) : 0;
  }
}
