// The driver: compile(), one build from the command line's arguments: the files read and lexed, the
// build, a compile server's kept build. A translation unit of its own since 2026-11-05 (main.cpp
// split, stage 2), moved without a change.
#include "Driver.hpp"
using namespace driver;

namespace
{
  // Each file's tag for its lambdas' closures (ParsedFile::lambdaTag): the FNV-1a hash of its name
  // under its root (names[i]), so the same wherever the sources are and whatever other files the
  // program has; unique in the program (a clash, which two names' hashes can make, takes the hash of
  // the later file's name with "#1", "#2", ... instead) and never 0 (unset).
  std::vector<uint32_t> lambdaTags(const std::vector<std::string>& names)
  {
    auto hash = [](const std::string& name)
    {
      uint32_t h = 2166136261u;
      for (unsigned char c : name)
        h = (h ^ c) * 16777619u;
      return h;
    };
    std::vector<uint32_t> tags;
    std::unordered_set<uint32_t> taken;
    for (const std::string& name : names)
    {
      uint32_t tag = hash(name);
      for (int k = 1; tag == 0 || !taken.insert(tag).second; ++k)
        tag = hash(name + "#" + std::to_string(k));
      tags.push_back(tag);
    }
    return tags;
  }
}

int compile(int argc, char** argv)
{
  codegenExit = 0;
  quietOutput = false;
  msvcDiagnostics = false;
  diagnosticColumns = false;
  Options options;
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    if (arg == "-j" && i + 1 < argc)
      options.threads = unsigned(atoi(argv[++i]));
    else if (arg == "--ext" && i + 1 < argc)
    {
      options.extensions.clear();
      std::string list = argv[++i];
      size_t pos = 0;
      while (pos <= list.size())
      {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos)
          comma = list.size();
        options.extensions.push_back(list.substr(pos, comma - pos));
        pos = comma + 1;
      }
    }
    else if (arg == "--repeat" && i + 1 < argc)
      options.repeat = atoi(argv[++i]);
    else if (arg == "--dump")
      options.dumpTokens = true;
    else if (arg == "--lex-only")
      options.parse = false;
    else if (arg == "--errors" && i + 1 < argc)
      options.showErrors = atoi(argv[++i]), options.showImportErrors = true;
    else if (arg == "--unresolved" && i + 1 < argc)
      options.showUnresolved = atoi(argv[++i]);
    else if (arg == "--why" && i + 1 < argc)
      options.showWhy = atoi(argv[++i]);
    else if (arg == "--why-at" && i + 1 < argc)
      options.whyAt = argv[++i];
    else if (arg == "--access-warnings")
      options.accessWarnings = true;
    else if (arg == "--return-copy-warnings")
      options.returnCopyWarnings = true;
    else if (arg == "--zero-null-warnings")
      options.zeroNullWarnings = true;
    else if (arg == "--override-warnings")
      options.overrideWarnings = true;
    else if (arg == "--this-warnings")
      options.thisWarnings = true;
    else if (arg == "--this-reference")
      options.thisReference = true;
    else if (arg == "--explicit-by-default")
      ; // the default since the flip (design.md "Explicit by default"): accepted until convert.cmd and build.kpp drop it
    else if (arg == "--this-reference-warnings")
      options.thisReferenceWarnings = true;
    else if (arg == "--plain-enum-warnings")
      options.plainEnumWarnings = true;
    else if (arg == "--no-implicit-fallthrough")
      ; // the default since the flip (design.md "No implicit fallthrough"): accepted until scripts drop it
    else if (arg == "--struct-rule")
      options.structRule = true;
    else if (arg == "--c-type-warnings")
      options.cTypeWarnings = true;
    else if (arg == "--quiet")
      options.quiet = true;
    else if (arg == "--msvc-diagnostics")
      msvcDiagnostics = true;
    else if (arg == "--diagnostic-columns")
      diagnosticColumns = true;
    else if (arg == "--check")
      options.check = true;
    else if (arg == "--packages")
      options.packages = options.quiet = true; // (the tree alone on stdout, not the front end's statistics)
    else if (arg == "--implicit-uses" && i + 1 < argc)
      options.implicitUses = argv[++i];
    else if (arg == "--emit-llvm" && i + 1 < argc)
      options.emitLLVM = argv[++i];
    else if (arg == "--exe" && i + 1 < argc)
      options.exe = argv[++i];
    else if (arg == "--run-build" && i + 1 < argc)
      options.runBuild = argv[++i];
    else if (arg == "--std" && i + 1 < argc)
      options.stdDir = argv[++i];
    else if (arg == "--no-std")
      options.noStd = true;
    else if (arg == "--cinclude" && i + 1 < argc)
      options.cIncludeDirs.push_back(argv[++i]);
    else if (arg == "--incremental-link")
      options.incrementalLink = true;
    else if (arg == "--defer-link")
      options.deferLink = true;
    else if (arg == "--toolchain" && i + 1 < argc)
      options.toolchain = argv[++i];
    else if (arg == "--debug-info")
      options.debugInfo = true;
    else if (arg == "--optimize" || arg == "--no-optimize")
      options.optimize = arg == "--optimize";
    else if (arg == "--target" && i + 1 < argc)
    {
      std::string why;
      if (!targetNamed(argv[++i], options.target, why))
      {
        fprintf(stderr, "--target %s %s\n", argv[i], why.c_str());
        return 2;
      }
      options.targetChosen = true;
    }
    else if (arg == "--sysroot" && i + 1 < argc)
      options.sysroot = argv[++i];
    else if (arg == "--no-run")
    {
      fprintf(stderr, "--no-run goes with --run or --test\n");
      return 2;
    }
    else if (arg == "--keep-going")
      options.keepGoing = true;
    else if (arg == "--stub-missing")
      options.stubMissing = true;
    else if (arg == "--no-inline")
      options.noInline = true;
    else if (arg == "--clang-flags" && i + 1 < argc)
      options.clangFlags = argv[++i];
    else if (arg == "--lto" && i + 1 < argc)
    {
      std::string mode = argv[++i];
      if (mode != "thin" && mode != "full")
      {
        fprintf(stderr, "--lto %s: thin or full\n", mode.c_str());
        return 2;
      }
      options.fullLTO = mode == "full";
    }
    else if (arg == "--lto-partitions" && i + 1 < argc)
    {
      int n = atoi(argv[++i]);
      if (n < 1)
      {
        fprintf(stderr, "--lto-partitions %s: a number of partitions, 1 or more\n", argv[i]);
        return 2;
      }
      options.ltoPartitions = unsigned(n);
    }
    else if (arg == "--opt-level" && i + 1 < argc)
    {
      std::string level = argv[++i];
      if (level != "2" && level != "3")
      {
        fprintf(stderr, "--opt-level %s: 2 or 3\n", level.c_str());
        return 2;
      }
      options.optLevel = level[0] - '0';
    }
    else if (arg == "--codegen-threads" && i + 1 < argc)
      options.codegenThreads = unsigned(atoi(argv[++i]));
    else if (arg == "--lib" && i + 1 < argc)
      options.libs.push_back(argv[++i]);
    else if (arg == "--natvis" && i + 1 < argc)
      options.natvisFiles.push_back(argv[++i]);
    else if (arg == "--keep-symbol" && i + 1 < argc)
      options.keepSymbols.push_back(argv[++i]);
    else if (arg == "--exclude" && i + 1 < argc)
      options.excluded.push_back(argv[++i]);
    else if (arg == "--file-prefix-map" && i + 1 < argc)
    {
      std::string item = argv[++i];
      size_t equals = item.find('=');
      if (equals == std::string::npos || equals == 0)
      {
        fprintf(stderr, "error: --file-prefix-map takes FROM=TO, a directory and what its paths start with instead, not %s\n", item.c_str());
        return 2;
      }
      options.filePrefixMap.push_back({item.substr(0, equals), item.substr(equals + 1)});
    }
    else if ((arg == "--library-root" || arg == "--unscoped-library-root") && i + 2 < argc)
    {
      options.libraryRoots.push_back({argv[i + 1], argv[i + 2], arg == "--library-root"});
      i += 2;
    }
    else if (arg == "--config" && i + 1 < argc)
    {
      std::string item = argv[++i];
      size_t eq = item.find('=');
      if (eq == std::string::npos)
        fprintf(stderr, "warning: --config expects name=value, got %s\n", item.c_str());
      else
        options.configValues[item.substr(0, eq)] = parseConfigValue(item.substr(eq + 1));
    }
    else if (arg == "--release")
      options.configValues["debug"] = parseConfigValue("false");
    else if (arg == "--tests")
      options.configValues["tests"] = parseConfigValue("true");
    else if (arg == "--modules" && i + 1 < argc)
      options.modules = unsigned(atoi(argv[++i]));
    else if (arg == "--convert" && i + 1 < argc)
      options.convertDir = argv[++i];
    else if (arg == "--verify-incremental")
      ; // the compile server's (server.cpp): a build in its own process is from scratch anyway
    else if (arg == "--overrides" && i + 1 < argc)
      options.overridesDir = argv[++i];
    else if (arg == "--convert-base" && i + 1 < argc)
      options.convertBase = argv[++i];
    else if (arg == "--c-sources")
      options.cSources = true;
    else if (arg == "--convert-library" && i + 2 < argc)
    {
      options.convertLibraries.push_back({argv[i + 1], argv[i + 2]});
      i += 2;
    }
    else if (arg == "--convert-release")
      setConvertRelease();
    else if (arg == "--convert-final")
      setConvertFinal();
    else if (arg == "--convert-marks")
      setConvertMarks();
    else if (arg == "--convert-native-tests")
      setConvertNativeTests();
    else if (arg == "--convert-platform" && i + 1 < argc)
    {
      if (!setConvertPlatform(argv[++i]))
        return optionError(arg + std::string(" ") + argv[i]);
    }
    else if (arg == "--convert-predefines" && i + 1 < argc)
    {
      if (!setConvertPredefines(argv[++i]))
        return optionError(arg + std::string(" ") + argv[i]);
    }
    else if (arg[0] == '-')
      return optionError(arg);
    else
      options.roots.push_back(arg);
  }
  if (options.roots.empty() && options.libraryRoots.empty())
  {
    usage();
    return 2;
  }
  // A library's name is the qualifier its names are written with: an identifier, and one library
  // is scoped or not, whichever of its roots says it.
  for (const Options::LibraryRoot& l : options.libraryRoots)
  {
    bool identifier = !l.name.empty() && !std::isdigit(static_cast<unsigned char>(l.name[0])) &&
                      std::all_of(l.name.begin(), l.name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    if (!identifier || lookupKeyword(l.name) != UINT32_MAX)
    {
      fprintf(stderr, "error: the library name '%s' is not an identifier\n", l.name.c_str());
      return 1;
    }
    std::error_code ec;
    if (!std::filesystem::exists(l.dir, ec))
    {
      fprintf(stderr, "error: the library %s's root %s does not exist\n", l.name.c_str(), l.dir.string().c_str());
      return 1;
    }
    for (const Options::LibraryRoot& other : options.libraryRoots)
      if (other.name == l.name && other.scoped != l.scoped)
      {
        fprintf(stderr, "error: the library %s is given both as --library-root and as --unscoped-library-root\n", l.name.c_str());
        return 1;
      }
  }
  if (options.threads == 0)
    options.threads = 1;
  quietOutput = options.quiet;
  if (!options.convertDir.empty())
  {
    options.conversionLibrary = options.stdDir.empty() ? defaultStdlib(argv[0])
                                                       : std::filesystem::path(options.stdDir);
  }
  // The standard library joins the program for code generation, and whenever --std names it.
  if ((generatesCode(options) || !options.stdDir.empty() || !options.runBuild.empty()) && !options.noStd)
  {
    std::error_code ec;
    std::filesystem::path stdPath = options.stdDir.empty()
      ? defaultStdlib(argv[0])
      : std::filesystem::path(options.stdDir);
    if (std::filesystem::is_directory(stdPath, ec))
    {
      options.libraryRoot = stdPath.lexically_normal();
      options.roots.insert(options.roots.begin(), options.libraryRoot);
    }
    else if (!options.stdDir.empty())
      fprintf(stderr, "warning: standard library directory %s not found\n", options.stdDir.c_str());
  }

  using Clock = std::chrono::steady_clock;

  // The paths the program's text holds (codegen's programPathOf): this build's map, none by default
  // (a compile server's builds each set theirs).
  setFilePrefixMap(options.filePrefixMap);

  // Phase 0: find and read all files. Reading is I/O bound, so it is timed separately.
  auto startCollect = Clock::now();
  std::vector<std::filesystem::path> paths;
  std::vector<FileStamp> stamps; // (paths[i]'s)
  std::vector<int> pathLibrary;  // (paths[i]'s: the index of its options.libraryRoots entry, -1 for the program's)
  // (paths[i]'s: its name under its root, which names its lambdas' closures (lambdaTags): the path
  // under the root after "std/" for the standard library's, "<library>/" for a library's and the
  // root directory's own name for the program's; a file given alone, its file name)
  std::vector<std::string> pathNames;
  // (paths[i]'s code package, for --packages: its root's directory and the directory under it, design.md
  // "Code packages"; a library's root is named as the library is)
  std::vector<PackageOf> pathPackages;
  // A compile server's listings are kept and watched (collectFilesWatched; thread A reviewed it,
  // 2026-11-05), except with KPP_NO_WATCH=1 and in --verify-incremental's build from scratch, which list.
  bool watched = servingBuild() && !replayDisabled() && !getenv("KPP_NO_WATCH");
  std::string watchNote, watchMissed;
  auto collect = [&](const std::filesystem::path& root, std::string prefix, int library)
  {
    auto found = watched ? collectFilesWatched(root, options.extensions, stamps, watchNote, watchMissed) : collectFiles(root, options.extensions, &stamps);
    std::error_code ec;
    bool alone = std::filesystem::is_regular_file(root, ec);
    {
      std::filesystem::path dir = std::filesystem::absolute(alone ? root.parent_path() : root, ec).lexically_normal();
      if (!dir.has_filename()) // (`src\`)
        dir = dir.parent_path();
      std::string rootName = library >= 0 && !options.libraryRoots[size_t(library)].name.empty() ? options.libraryRoots[size_t(library)].name
                                                                                                 : dir.filename().generic_string();
      for (const std::filesystem::path& p : found)
      {
        std::filesystem::path under = std::filesystem::absolute(p, ec).lexically_normal().parent_path().lexically_relative(dir);
        pathPackages.push_back({dir, rootName, under == "." ? std::string() : under.generic_string()});
      }
    }
    if (prefix.empty() && !alone)
    {
      std::filesystem::path dir = std::filesystem::absolute(root, ec).lexically_normal();
      if (!dir.has_filename()) // (`src\`)
        dir = dir.parent_path();
      prefix = dir.filename().generic_string() + "/";
    }
    for (const std::filesystem::path& p : found)
      pathNames.push_back(prefix + (alone ? p.filename() : p.lexically_relative(root)).generic_string());
    paths.insert(paths.end(), found.begin(), found.end());
    pathLibrary.resize(paths.size(), library);
  };
  // A path given that names no file or directory: an error naming it, not a failure of the listing.
  {
    std::vector<std::filesystem::path> given(options.roots.begin(), options.roots.end());
    for (const auto& library : options.libraryRoots)
      given.push_back(library.dir);
    for (const std::filesystem::path& root : given)
      if (std::error_code ec; !std::filesystem::exists(root, ec))
      {
        fprintf(stderr, "error: %s: no such file or directory\n", root.string().c_str());
        return 1;
      }
  }
  // The standard library first, then the libraries in the order given, then the program.
  size_t stdRoots = options.libraryRoot.empty() ? 0 : 1;
  for (size_t i = 0; i < stdRoots; ++i)
    collect(options.roots[i], "std/", -1);
  for (size_t i = 0; i < options.libraryRoots.size(); ++i)
    collect(options.libraryRoots[i].dir, options.libraryRoots[i].name.empty() ? "" : options.libraryRoots[i].name + "/", int(i));
  for (size_t i = stdRoots; i < options.roots.size(); ++i)
    collect(options.roots[i], "", -1);
  if (!watchNote.empty())
    logBuildNote(watchNote.substr(0, watchNote.size() - 2));
  // KPP_WATCH_CHECK's difference: an error, loudly (the build is from the listing, but fails).
  if (!watchMissed.empty())
  {
    logBuildNote("watch missed " + watchMissed.substr(0, watchMissed.size() - 2));
    fprintf(stderr, "error: the compile server's watched listing missed %s (KPP_WATCH_CHECK)\n", watchMissed.substr(0, watchMissed.size() - 2).c_str());
    return 1;
  }
  // --exclude: files under the roots that are not part of this program (another program's main).
  for (const std::string& excluded : options.excluded)
  {
    std::error_code ec;
    std::filesystem::path file = std::filesystem::absolute(excluded, ec).lexically_normal();
    size_t kept = 0;
    for (size_t i = 0; i < paths.size(); ++i)
      if (std::filesystem::absolute(paths[i], ec).lexically_normal() != file)
      {
        paths[kept] = std::move(paths[i]);
        pathLibrary[kept] = pathLibrary[i];
        pathNames[kept] = std::move(pathNames[i]);
        pathPackages[kept] = std::move(pathPackages[i]);
        stamps[kept++] = stamps[i];
      }
    if (kept == paths.size())
    {
      fprintf(stderr, "error: --exclude %s names no file under the sources\n", excluded.c_str());
      return 1;
    }
    paths.resize(kept);
    pathLibrary.resize(kept);
    pathNames.resize(kept);
    pathPackages.resize(kept);
    stamps.resize(kept);
  }
  // A compile server's builds run one at a time up to the point each keeps what it made, so that a
  // build finds its predecessor's kept (the predecessor answers its client before that point and
  // frees the build before it afterwards, on its own thread).
  std::unique_lock<std::mutex> keptLock(keptMutex, std::defer_lock);
  if (servingBuild())
    keptLock.lock();
  auto build = std::make_unique<Build>();
  build->files.resize(paths.size());
  build->fileLibrary = pathLibrary;
  std::vector<SourceFile>& files = build->files;
  files.reserve(paths.size() + 3); // the configuration, the test runner and the imported C headers
  std::atomic<uint64_t> totalBytes{0};
  auto hashOf = [](std::string_view bytes)
  {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes)
      h = (h ^ c) * 1099511628211ull;
    return h;
  };
  // A compile server's file whose stamp is the kept build's, and for which no editor's unsaved text
  // stands (an overlay, kpp --lsp), is the kept build's text: not read again (the listing gave its
  // stamp). An overlaid file's stamp is none, so that it is read once the overlay is gone.
  // (--verify-incremental's build from scratch reads every file.)
  const Build* last = servingBuild() && !replayDisabled() && options.convertDir.empty() && keptBuild ? keptBuild.get() : nullptr;
  std::vector<uint64_t> hashes(paths.size());
  parallelFor(paths.size(), options.threads, [&](size_t i)
  {
    files[i].path = paths[i];
    bool overlaid = hasFileOverlay(paths[i]);
    if (overlaid)
      stamps[i] = FileStamp{};
    if (last && !overlaid && stamps[i].time && i < last->stamps.size() && last->stamps[i] == stamps[i] && last->files[i].path == paths[i])
    {
      files[i].text = last->files[i].text;
      hashes[i] = last->textHashes[i];
    }
    else
    {
      if (!readFile(paths[i], files[i].text))
        fprintf(stderr, "cannot read %s\n", paths[i].string().c_str());
      if (servingBuild()) // (for the replay's fingerprint below)
        hashes[i] = hashOf(files[i].text);
    }
    totalBytes += files[i].text.size();
  });
  build->stamps = stamps;
  build->textHashes = hashes;
  {
    // The build configuration is one more source file: `config::debug`, `config::platform`...
    if (!options.configValues.count("debug"))
      options.configValues["debug"] = parseConfigValue("true");
    // The library's sizes were 32 or 64 bits by `--config sizeBits` until they became u64 alone
    // (design.md, "Container sizes are u64"): a command line that still chooses one is told so.
    if (options.configValues.count("sizeBits"))
    {
      fprintf(stderr, "--config sizeBits: the library's sizes are always 64 bits (ContainerSize is u64)\n");
      return 2;
    }
    if (std::string error = selectToolchain(argv[0], options.toolchain); !error.empty())
    {
      fprintf(stderr, "%s\n", error.c_str());
      return 2;
    }
    // The bundled toolchain (llvm-mingw) builds Windows programs in the GNU dialect: it has no Visual
    // Studio's headers and libraries for the MSVC one, and Visual Studio's clang has no MinGW-w64's.
    if (bundledToolchain() && options.target.msvc())
    {
      if (options.targetChosen)
      {
        fprintf(stderr, "--target windows (the MSVC dialect) needs Visual Studio's headers and libraries, which the bundled toolchain "
                        "does not have: --toolchain msvc, or --target windows-gnu\n");
        return 2;
      }
      std::string why;
      targetNamed("windows-gnu", options.target, why);
    }
    if (options.target.gnu && !bundledToolchain() && !getenv("KPP_CLANG"))
    {
      fprintf(stderr, "--target windows-gnu needs the bundled toolchain's MinGW-w64 (llvm-mingw): --toolchain bundled, or KPP_CLANG "
                      "naming such a clang\n");
      return 2;
    }
    // Another platform's C headers and libraries (design.md, "Targets and hosts"): --sysroot, else the
    // one that comes with kpp, sysroots\freebsd-x86_64 or linux-x86_64 beside it (a release brings
    // FreeBSD's: `--target freebsd` works as installed, 2026-11-04). A link without either is an error
    // naming where it looked; an --emit-llvm build needs none.
    // A Mac's: the macOS SDK the user supplies (Q7), --sysroot or SDKROOT, checked before any work: when
    // it is missing or wrong, what to have and where it comes from, never a header clang cannot find.
    if (options.target.mac())
    {
      if (options.sysroot.empty() && getenv("SDKROOT") && *getenv("SDKROOT"))
        options.sysroot = getenv("SDKROOT");
      std::string problem = options.sysroot.empty() ? "none was given (neither --sysroot nor SDKROOT)" : macSdkProblem(options.sysroot, options.target.archName());
      if (!problem.empty())
      {
        fprintf(stderr,
                "--target %s needs a macOS SDK, version 11.0 or newer: %s.\n"
                "  A macOS SDK is a directory such as MacOSX.sdk. On a Mac, `xcrun --show-sdk-path` prints the one that Xcode or the\n"
                "  Command Line Tools installed; copy that directory to this machine.\n"
                "  Then give it to kpp: --sysroot DIR, or the environment variable SDKROOT.\n",
                options.target.name.c_str(), problem.c_str());
        return 2;
      }
    }
    if (options.target.posix() && options.sysroot.empty())
    {
      std::string name = std::string(platformOf(options.target.os).name) + "-" + options.target.archName();
      std::filesystem::path beside = defaultSysroot(compilerPath(), name.c_str());
      if (!beside.empty())
        options.sysroot = beside.string();
      else if (!options.exe.empty())
      {
        fprintf(stderr, "--target %s needs the target's C headers and libraries to link: --sysroot DIR, or %s (%s)\n", options.target.name.c_str(),
                (compilerPath().parent_path() / "sysroots" / name).string().c_str(),
                options.target.os == Target::Os::FreeBSD ? "a release's installer puts it there; in a build of kpp, tools\\sysroot\\freebsd.ps1"
                : options.target.arm64()                 ? "tools\\sysroot\\linux.ps1 -Arch arm64, Debian 11's glibc 2.31 for arm64"
                                                         : "tools\\sysroot\\linux.ps1, Debian 11's glibc 2.31, which tests\\targets.ps1 runs");
        return 2;
      }
    }
    if (options.incrementalLink && bundledToolchain())
    {
      fprintf(stderr, "--incremental-link is Visual Studio's link.exe's, which the bundled toolchain does not have (its lld-link "
                      "links whole): add --toolchain msvc\n");
      return 2;
    }
#ifdef _WIN32
    // Windows' tools open no path of MAX_PATH (260) characters or more: kpp's own writes, and
    // link.exe's output even given as a \\?\ path (LNK1104), whatever LongPathsEnabled says, so a
    // build there fails file by file. Said once, before any work: the longest name kpp writes beside
    // the executable is NAME.failed-functions.txt (docs/build.md, "Long paths").
    if (!options.exe.empty())
    {
      const char* longest = ".failed-functions.txt";
      std::error_code ec;
      std::string exe = std::filesystem::absolute(options.exe, ec).string();
      if (exe.size() + strlen(longest) >= 260)
      {
        fprintf(stderr,
                "--exe %s: its path is %zu characters, and kpp writes files beside it with up to %zu more (%s); Windows' tools, kpp "
                "and link.exe, open no path of 260 characters or more (MAX_PATH), whatever LongPathsEnabled says: an output path of at "
                "most %zu characters\n",
                exe.c_str(), exe.size(), strlen(longest), longest, 259 - strlen(longest));
        return 2;
      }
    }
#endif
    // config::platform is the target's.
    const char* platform = platformOf(options.target.os).enumerator;
    if (!options.configValues.count("platform"))
      options.configValues["platform"] = parseConfigValue(platform);
    else if (options.configValues["platform"].text != platform)
    {
      fprintf(stderr, "--config platform=%s: the platform is the target's (--target), %s\n", options.configValues["platform"].text.c_str(), platform);
      return 2;
    }
    // config::arch is the target's too.
    const char* arch = archEnumerators[size_t(options.target.arch)];
    if (!options.configValues.count("arch"))
      options.configValues["arch"] = parseConfigValue(arch);
    else if (options.configValues["arch"].text != arch)
    {
      fprintf(stderr, "--config arch=%s: the architecture is the target's (--target), %s\n", options.configValues["arch"].text.c_str(), arch);
      return 2;
    }
    // config::mingw: Windows' GNU dialect (windows-gnu), whose C runtime's headers are MinGW-w64's
    // (math.h's FP_ codes, its fpclassify: CWindows.kpp), the target's too.
    if (options.configValues.count("mingw") && (options.configValues["mingw"].number != 0) != options.target.gnu)
    {
      fprintf(stderr, "--config mingw=%s: it is the target's (--target %s)\n", options.configValues["mingw"].text.c_str(), options.target.name.c_str());
      return 2;
    }
    options.configValues["mingw"] = parseConfigValue(options.target.gnu ? "true" : "false");
    if (!options.configValues.count("tests"))
      options.configValues["tests"] = parseConfigValue("false");
    options.debug = options.configValues["debug"].number != 0;
    if (options.optimize < 0)
      options.optimize = !options.debug;
    if (options.incrementalLink && options.optimize)
    {
      fprintf(stderr, "--incremental-link is for an unoptimized build: an optimized build links whole (ThinLTO)\n");
      return 2;
    }
    if (options.incrementalLink && !options.target.windows())
    {
      fprintf(stderr, "--incremental-link is link.exe's, for --target windows\n");
      return 2;
    }
    // config:: constants declared in source with their defaults (config.cpp), read before any file is
    // parsed: the parser's declaration-scope if constexpr, when and requires read their values.
    std::map<std::string, ConfigDeclared> declared;
    if (!configDeclarations(files, paths.size(), build->fileLibrary, options, declared))
      return 1;
    SourceFile config;
    config.path = "<config>";
    config.text = configSource(options.configValues, declared);
    build->configDeclared = declared;
    totalBytes += config.text.size();
    files.push_back(std::move(config));
    // --tests: the runner is one more source file (tests.hpp).
    if (options.configValues.count("tests") && options.configValues["tests"].number)
    {
      SourceFile runner;
      runner.path = testRunnerPath;
      runner.text = testRunnerSource();
      totalBytes += runner.text.size();
      files.push_back(std::move(runner));
      for (auto [path, text] : {std::pair(testRunnerWindowsPath, testRunnerWindowsSource()), std::pair(testRunnerPosixPath, testRunnerPosixSource())})
      {
        SourceFile platform;
        platform.path = path;
        platform.text = std::move(text);
        totalBytes += platform.text.size();
        files.push_back(std::move(platform));
      }
    }
  }
  // What names each file's lambdas' closures: its name under its root (the files the driver adds,
  // their own: "<config>").
  pathNames.resize(files.size());
  for (size_t i = paths.size(); i < files.size(); ++i)
    pathNames[i] = files[i].path.generic_string();
  build->lambdaTags = lambdaTags(pathNames);
  // A compile server's build whose every input is as the previous one's (step 3a of the
  // persistent compiler, docs/compile-speed.md): the previous build's output again.
  if (servingBuild() && options.convertDir.empty() && !options.packages) // (--packages prints what the front end read: a build of its own)
  {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::string_view bytes)
    {
      for (unsigned char c : bytes)
        h = (h ^ c) * 1099511628211ull;
      h = (h ^ 0xff) * 1099511628211ull;
    };
    for (size_t i = 0; i < files.size(); ++i)
    {
      mix(files[i].path.string());
      if (i < hashes.size()) // (a program's file by its hash, made as it was read)
        mix(std::string_view(reinterpret_cast<const char*>(&hashes[i]), sizeof hashes[i]));
      else
        mix(files[i].text);
    }
    mix(environmentBlock()); // TMP (the import cache), KPP_CLANG, LIB...
    // The files the link reads besides the build's own, by their size and time: a .natvis (--natvis)
    // or a library (--lib FILE) replaced with the same name is a new build, not the last one replayed
    // (the PDB would keep the old rules, the executable the old library). A missing one mixes in as
    // such, and the link reports it.
    for (const std::vector<std::string>* list : {&options.natvisFiles, &options.libs})
      for (const std::string& file : *list)
      {
        if (file.empty() || file[0] == '-') // (-lname: a system library, found by the linker)
          continue;
        std::error_code ec;
        uintmax_t size = std::filesystem::file_size(file, ec);
        auto time = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
        mix(file);
        mix(std::string_view(reinterpret_cast<const char*>(&size), sizeof size));
        mix(std::string_view(reinterpret_cast<const char*>(&time), sizeof time));
      }
    std::vector<std::string> outputs;
    for (const std::string& o : {options.emitLLVM, options.exe})
      if (!o.empty())
        outputs.push_back(std::filesystem::absolute(o).string());
    // What the build leaves: its outputs, but under --defer-link the link command in place of the
    // executable, which kpp links (and links again when it is missing, Project/Build.cpp), and the modules'
    // objects and hashes beside it.
    std::function<std::vector<std::string>()> left = [emitLLVM = options.emitLLVM, exe = options.exe, deferLink = options.deferLink]
    {
      std::vector<std::string> files;
      if (!emitLLVM.empty())
        files.push_back(std::filesystem::absolute(emitLLVM).string());
      if (exe.empty())
        return files;
      std::filesystem::path program = std::filesystem::absolute(exe);
      files.push_back(program.string() + (deferLink ? ".link.command" : ""));
      std::string prefix = program.filename().string() + ".";
      std::vector<std::string> modules; // <exe>.<k>.o and <exe>.<k>.hash
      std::error_code ec;
      for (std::filesystem::directory_iterator i(program.parent_path(), ec), end; !ec && i != end; i.increment(ec))
      {
        std::string name = i->path().filename().string();
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
          continue;
        size_t digits = name.find_first_not_of("0123456789", prefix.size());
        if (digits != prefix.size() && digits != std::string::npos && (name.compare(digits, std::string::npos, ".o") == 0 || name.compare(digits, std::string::npos, ".hash") == 0))
          modules.push_back(i->path().string());
      }
      std::sort(modules.begin(), modules.end());
      files.insert(files.end(), modules.begin(), modules.end());
      return files;
    };
    int replayed = 0;
    bool headersSame = lastImportStamps.empty() || headersUnchanged(lastImportStamps);
    if (replayBuild(h, outputs, left, headersSame, replayed))
      return replayed;
    // Step 3b: the kept build with the changed function bodies checked again, when that is all
    // that changed.
    if (keptBuild && !replayDisabled() && options.repeat == 1 && !options.dumpTokens && options.parse && !options.packages) // (--verify-incremental's build is from scratch)
    {
      bool keptChanged = false;
      if (incrementalBuild(*keptBuild, files, seconds(Clock::now() - startCollect), headersSame, keptChanged))
      {
        keptBuild->stamps = std::move(build->stamps); // (its texts are this build's now)
        keptBuild->textHashes = std::move(build->textHashes);
        return codegenExit;
      }
    }
    lastImportStamps.clear(); // this build's, if it imports any
  }
  // Converting, the front end lexes the C++ sources with the #if branches the converter drops
  // blanked, so that its rewrites reach the branches that are written out (offsets are unchanged:
  // tokens still index files[i].text).
  std::vector<std::string> configured(paths.size());
  std::unique_ptr<SourceConfiguration>& configuration = build->configuration;
  if (!options.convertDir.empty())
  {
    {
      // (the hand edits of the C++ the conversion reads: overrides\source-edits.txt)
      std::vector<std::filesystem::path> editPaths;
      std::vector<std::string*> editTexts;
      for (size_t i = 0; i < paths.size(); ++i)
        if (files[i].path.extension() != ".kpp")
        {
          editPaths.push_back(files[i].path);
          editTexts.push_back(&files[i].text);
        }
      if (std::string errors = applyConvertSourceEdits(options.overridesDir, editPaths, editTexts); !errors.empty())
      {
        fprintf(stderr, "%s", errors.c_str());
        return 1;
      }
    }
    std::vector<std::filesystem::path> cppPaths;
    std::vector<std::string_view> cppTexts;
    std::vector<size_t> cppFiles;
    for (size_t i = 0; i < paths.size(); ++i)
      if (files[i].path.extension() != ".kpp")
      {
        cppPaths.push_back(files[i].path);
        cppTexts.push_back(files[i].text);
        cppFiles.push_back(i);
      }
    configuration = std::make_unique<SourceConfiguration>(cppPaths, cppTexts,
                                                          std::vector<std::filesystem::path>(options.roots.begin(), options.roots.end()),
                                                          options.cSources, options.overridesDir);
    for (size_t k = 0; k < cppFiles.size(); ++k)
      configured[cppFiles[k]] = configuration->configuredText(k);
    options.configuration = configuration.get();
  }
  double collectTime = seconds(Clock::now() - startCollect);

  // Phase 1: lex everything in parallel.
  double bestLex = 1e9;
  FileStats total;
  for (int round = 0; round < options.repeat; ++round)
  {
    build->symbols = std::make_unique<SymbolTable>();
    SymbolTable& symbols = *build->symbols;
    std::vector<FileStats> stats(files.size());
    build->lexed.assign(files.size(), LexResult{});
    std::vector<LexResult>& results = build->lexed;
    auto start = Clock::now();
    parallelFor(files.size(), options.threads, [&](size_t i)
    {
      results[i] = lex(i < configured.size() && !configured[i].empty() ? configured[i] : files[i].text, symbols);
      if (options.cSources && isCSource(files[i].path))
        cKeywordsAsNames(results[i].tokens, i < configured.size() && !configured[i].empty() ? configured[i] : files[i].text, symbols,
                         configuration ? &configuration->cKeywordMacros() : nullptr);
      stats[i].bytes = files[i].text.size();
      stats[i].tokens = results[i].tokens.size();
      stats[i].lines = results[i].lineCount;
      stats[i].errors = results[i].errorCount;
    });
    double lexTime = seconds(Clock::now() - start);
    bestLex = std::min(bestLex, lexTime);

    total = {};
    for (const auto& s : stats)
    {
      total.bytes += s.bytes;
      total.tokens += s.tokens;
      total.lines += s.lines;
      total.errors += s.errors;
    }

    if (round == options.repeat - 1)
    {
      reportLex(files.size(), total, symbols.size(), options.threads, collectTime, bestLex);
      if (total.errors)
      {
        unsigned shown = 0;
        for (size_t i = 0; i < files.size() && shown < 20; ++i)
          for (const Token& t : results[i].tokens)
            if (t.getKind() == TokenKind::Error && shown++ < 20)
              report("  error in %s at offset %u: '%.*s'\n", files[i].path.string().c_str(), t.offset,
                     int(std::min<uint32_t>(t.length, 40)), files[i].text.data() + t.offset);
      }
      if (options.parse)
      {
        build->options = options;
        build->keepable = options.repeat == 1 && options.convertDir.empty() && !options.dumpTokens && !options.packages;
        if (options.packages)
          build->packages = pathPackages;
        runParse(*build);
      }
      if (options.dumpTokens)
        for (size_t i = 0; i < files.size(); ++i)
        {
          report("== %s\n", files[i].path.string().c_str());
          for (const Token& t : results[i].tokens)
            report("%-10s %.*s\n", tokenKindName(t.getKind()), int(t.length), files[i].text.data() + t.offset);
        }
    }
  }
  // A compile server keeps what this build made for the next request; the build kept before is
  // freed (after this one answered its client, and outside the lock).
  std::unique_ptr<Build> previous;
  if (keptLock.owns_lock() && !replayDisabled())
  {
    previous = std::move(keptBuild);
    if (build->keepable && build->global && !replayDisabled())
      keptBuild = std::move(build);
    keptLock.unlock();
  }
  return codegenExit;
}
