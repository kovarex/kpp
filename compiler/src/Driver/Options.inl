// The driver: the command line's options, the report, usage and the version; parallelFor. Part of
// main.cpp, included in its namespace there (a file of its own since 2026-11-05, moved without a
// change).
  bool quietOutput = false;
  int report(const char* format, ...)
  {
    if (quietOutput)
      return 0;
    va_list args;
    va_start(args, format);
    int n = vprintf(format, args);
    va_end(args);
    return n;
  }

  // With --c-sources: a source file of the inputs that is C (stb_image's implementation is a .hpp),
  // not the configuration's `<config>` nor a .kpp file.
  bool isCSource(const std::filesystem::path& path)
  {
    return path.extension() != ".kpp" && path.string().rfind("<", 0) != 0;
  }

  struct Options
  {
    std::vector<std::filesystem::path> roots;
    std::vector<std::string> extensions = {".cpp", ".hpp", ".h", ".kpp"};
    unsigned threads = std::thread::hardware_concurrency();
    bool dumpTokens = false;
    bool parse = true;
    int showErrors = 20;
    int showUnresolved = 25;
    int showWhy = 0; // --why N
    std::string whyAt; // --why-at FILE:LINE[,FILE:LINE...]
    bool accessWarnings = false; // --access-warnings
    bool returnCopyWarnings = false; // --return-copy-warnings
    bool zeroNullWarnings = false; // --zero-null-warnings
    bool structRule = true;      // design.md "struct and class" (--struct-rule, the default)
    bool cTypeWarnings = false;  // --c-type-warnings
    bool quiet = false;
    std::string emitLLVM; // --emit-llvm <path>
    std::string exe;      // --exe <path>
    std::string runBuild; // --run-build CONFIGURATION: print build.kpp's description (GlobalScope::runBuild)
    std::string stdDir;   // --std <dir>; default: <kppc dir>\..\..\stdlib when it exists
    std::filesystem::path conversionLibrary; // the library --convert keeps the program's names apart from
    bool noStd = false;
    std::filesystem::path libraryRoot; // the standard library's directory, when it joins the program
    // --library-root NAME DIR and --unscoped-library-root NAME DIR (design.md 16): the files under DIR
    // (or the file DIR) are the library NAME's, whose top-level names a program writes NAME::name
    // unless it is unscoped. Repeatable; one library may have several roots (its tests').
    struct LibraryRoot
    {
      std::string name;
      std::filesystem::path dir;
      bool scoped = true;
    };
    std::vector<LibraryRoot> libraryRoots;
    std::vector<std::string> cIncludeDirs; // --cinclude DIR, for imported C headers
    std::vector<std::string> libs;         // --lib FILE: libraries linked into the executable
    std::vector<std::string> keepSymbols;  // --keep-symbol NAME: linked in although nothing names it
    std::vector<std::string> excluded;     // --exclude FILE: a file under the roots left out
    std::vector<std::pair<std::string, std::string>> filePrefixMap; // --file-prefix-map FROM=TO (source.hpp's programPath)
    bool showImportErrors = false; // --errors given: list parse errors in imported headers too
    ConfigValues configValues;     // --config name=value, --release; becomes the `config` namespace
    bool debug = true;             // config::debug
    int optimize = -1;             // --optimize 1, --no-optimize 0, unset -1: clang -O2 (ThinLTO, a whole link) when !debug
    bool fullLTO = false;          // --lto full: an optimized build is one optimization over the whole program (--lto thin: ThinLTO)
    unsigned ltoPartitions = 0;    // --lto-partitions N: full LTO's code generated in N partitions (0: one per thread)
    int optLevel = 2;              // --opt-level 2|3: clang's -O for an optimized build's modules and its link
    unsigned modules = 0;          // --modules N: IR modules compiled in parallel (0: 2 per thread, 1: one module)
    unsigned codegenThreads = 0;   // --codegen-threads N: threads emitting IR (0: --threads)
    std::string convertDir;        // --convert DIR: write the inputs as kpp sources there
    const SourceConfiguration* configuration = nullptr; // converting: the #if branches the front end and the converter keep
    std::string overridesDir;      // --overrides DIR: hand-written files that replace converted ones
    std::string convertBase;       // --convert-base DIR: --convert writes each file at its path below DIR
    std::vector<std::pair<std::string, std::filesystem::path>> convertLibraries; // --convert-library NAME DIR (convert.hpp)
    bool cSources = false;         // --c-sources: the inputs are C, not the C++ Factorio compiles Lua as (convert.hpp)
    int repeat = 1;
    bool keepGoing = false;        // --keep-going: compile and link what codegen emitted despite its errors
    bool stubMissing = false;      // --stub-missing (with --keep-going): what was left out aborts with its name when called
    bool noInline = false;         // --no-inline: every call stays a call (no trivial callee emitted in its caller)
    std::string clangFlags;        // --clang-flags: more flags for compiling the modules (-fsanitize=address)
    bool incrementalLink = false;  // --incremental-link: a debug build's link redoes only the objects that changed
    bool deferLink = false;        // --defer-link: the link command written for kpp to run (Project/Build.cpp), not run
    std::string toolchain;         // --toolchain bundled|msvc: which clang and linker (source.hpp's selectToolchain)
    bool debugInfo = false;        // --debug-info: line tables (docs/debugging.md)
    bool check = false;            // --check: codegen's errors too, nothing written (`kppc --lsp`'s checks)
    bool packages = false;         // --packages: the code packages and what each file declares (design.md, "Code packages")
    Target target;                 // --target: the platform the program is built for (Windows x64 by default)
    bool targetChosen = false;     // --target was given (else the toolchain's own: windows-gnu for the bundled one)
    std::string sysroot;           // --sysroot: the target's headers and libraries, for another platform than the host
  };
  // Whether the build generates code: an executable, the IR, or --check's errors from codegen.
  bool generatesCode(const Options& o) { return !o.emitLLVM.empty() || !o.exe.empty() || o.check; }

  void usage(FILE* out = stderr)
  {
    fprintf(out,
            "usage: kppc [options] <file-or-dir>...\n"
            "       kpp [targets] [configurations] [options] in a project's directory (build.kpp: --run ARGS,\n"
            "       --test, --set NAME=VALUE, --verbose (kppc's report of each build) and the options below;\n"
            "       kpp new, kpp options, kpp measure [--rounds N] [--compare DIR]; docs/build.md)\n"
            "  --version     the version and commit, the compiler, the toolchain a build takes, the standard library\n"
            "  --help        this list\n"
            "  -j N          worker threads (default: hardware threads)\n"
            "  --ext .x,.y   file extensions to pick up (default .cpp,.hpp,.h,.kpp)\n"
            "  --repeat N    lex the corpus N times, report the best time\n"
            "  --dump        print tokens\n"
            "  --lex-only    skip parsing\n"
            "  --errors N    show the first N parse errors (default 20)\n"
            "  --unresolved N show the N most common unresolved names (default 25)\n"
            "  --why N       per cause of unknown expression types, the N most common failing constructs\n"
            "  --why-at F:L  print every expression typed on line L of the file whose path ends with F (F:L,F:L,... for several)\n"
            "  --access-warnings  warn of private and protected members named outside their class, its derived classes and\n"
            "                its friends, and of structs with a private member, a non-public base or a virtual function\n"
            "                (docs/design.md, \"struct and class\"); no error, no other change\n"
            "  --zero-null-warnings  a literal 0 that would be a pointer (an error, docs/design.md 4) is a warning instead:\n"
            "                convert.cmd's nullptr pass rewrites what they name\n"
            "  --return-copy-warnings  warn of each `return x;` of a class object that is copied or moved, not built in\n"
            "                the result's place (a named return), and why; no error, no other change\n"
            "  --struct-rule  a struct is a public value type: one with a private or protected section, a base that is not\n"
            "                public or a virtual function, or deriving from a polymorphic class is an error (docs/design.md,\n"
            "                \"struct and class\"; the default: accepted, it changes nothing)\n"
            "  --c-type-warnings  warn of each C name of an integer type (int, unsigned, short, long, long long, signed\n"
            "                and unsigned char, int32_t, size_t ...) written outside extern \"C\", the C headers and the\n"
            "                standard library, naming kpp's (i32, u64 ...) (docs/design.md, \"Integer types and sizes\"); no error\n"
            "  --quiet       no statistics\n"
            "  --msvc-diagnostics  diagnostics at path(line) instead of path:line, the form Visual Studio's\n"
            "                Error List reads (docs/build.md)\n"
            "  --diagnostic-columns  the column too where it is known: path:line:column, path(line,column)\n"
            "                (`kppc --lsp`, docs/ide.md)\n"
            "  --emit-llvm F write LLVM IR for the program to F\n"
            "  --exe F       compile the program to the executable F (needs clang)\n"
            "  --check       every error a build reports, codegen's included, writing nothing\n"
            "  --packages    the code packages (each directory, named as it is; docs/design.md, \"Code packages\")\n"
            "                with their README.md's first paragraph and the names each file declares\n"
            "  --lsp [OPTIONS SOURCES]  a language server for an editor, on stdin and stdout (docs/ide.md)\n"
            "  --std DIR     standard library sources (default: stdlib next to the compiler)\n"
            "  --no-std      compile without the standard library\n"
            "  --library-root NAME DIR  the sources DIR (a directory or a file) are the library NAME's, whose\n"
            "                names are written NAME::name outside it (repeatable; design.md 16)\n"
            "  --unscoped-library-root NAME DIR  the same for an unscoped library: its names are bare everywhere\n"
            "  --cinclude D  directory searched by imported C headers (repeatable)\n"
            "  --lib F       a library (.lib) linked into the executable (repeatable)\n"
            "  --keep-symbol S  link in what defines the symbol S although nothing names it (a library's object that\n"
            "                registers itself), with any toolchain: link.exe's /INCLUDE:S, the GNU driver's -u S (repeatable)\n"
            "  --exclude F   leave out the file F found under the sources (repeatable; build.kpp's exclude)\n"
            "  --file-prefix-map FROM=TO  the paths the program's text holds (an assertion's message, a\n"
            "                CallerLocation's file) under the directory FROM start with TO (repeatable; kpp's\n"
            "                release and final builds: no path of this machine in the program)\n"
            "  --config N=V  build configuration constant config::N (bool, integer, enumerator or string)\n"
            "  --release     the same as --config debug=false\n"
            "  --optimize, --no-optimize  clang -O2 (ThinLTO, a whole link) or -O0, whatever config::debug is\n"
            "                (default: -O2 when config::debug is false; build.kpp's fastdebug: asserts in, optimized)\n"
            "  --lto thin|full  how an optimized build optimizes across its modules, in the link (lld): ThinLTO (the\n"
            "                default: each module with what it imports from the others, in parallel), or full LTO (one\n"
            "                optimization over the whole program, its code generated in one partition per thread:\n"
            "                build.kpp's final, as FinalRelease's /GL and /LTCG)\n"
            "  --lto-partitions N  full LTO's code generated in N partitions (default: one per thread); 1 when C\n"
            "                libraries built with -flto join it (their statics stay theirs: docs/build.md)\n"
            "  --opt-level N  clang's -O for an optimized build's modules and its link: 2 (the default) or 3\n"
            "  --modules N   split the program into N IR modules compiled in parallel (default: four per thread, two in a release build)\n"
            "  --codegen-threads N  threads emitting IR (default: --threads; the IR does not depend on it)\n"
            "  --keep-going  compile and link the IR codegen emitted even when it reported errors (functions with\n"
            "                errors are left out; the link lists what is missing)\n"
            "  --stub-missing  with --keep-going, a bring-up tool: every function left out (or used without a\n"
            "                definition) is a stub that prints its name and line and aborts when called, so the\n"
            "                program links; the errors are still reported and the exit code is still 1\n"
            "  --no-inline   keep every call a call: no trivial function is emitted in its caller\n"
            "                (docs/compile-speed.md, \"Design: inlining trivial functions in kppc\")\n"
            "  --clang-flags F  more flags clang compiles the modules with (`-fsanitize=address`; link with\n"
            "                `--lib -fsanitize=address`); they are part of each module's cache key\n"
            "  --incremental-link  a debug build links incrementally (link.exe /INCREMENTAL): the next link redoes\n"
            "                only the objects that changed, as long as the executable and its .ilk stay; every\n"
            "                function is kept (/OPT:NOREF)\n"
            "  --defer-link  write the link command to EXE.link.command (and EXE.link.pending when this build's\n"
            "                objects are new) instead of linking: kpp links once the C libraries, built alongside, are there\n"
            "  --toolchain T  bundled: the installation's own llvm-mingw (bin\\, the default where it has one),\n"
            "                which builds windows-gnu with nothing of Visual Studio's; msvc: Visual Studio's clang\n"
            "                and link.exe (the default otherwise, and what --incremental-link needs; design.md 17);\n"
            "                a list in the order preferred (msvc,bundled): the first of them this machine has\n"
            "  --debug-info  line tables in the objects (CodeView) and a .pdb next to the executable: a debugger\n"
            "                steps through the kpp source (docs/debugging.md)\n"
            "  --target T    build for T: windows (the default), linux, freebsd, or a triple (docs/design.md,\n"
            "                \"Targets and hosts\"); sets config::platform\n"
            "  --sysroot D   the target's C headers and libraries, for a target other than the host\n"
            "                (tools/sysroot/ makes one for Linux from WSL)\n"
            "  --convert D   rewrite the C++ inputs as kpp sources under D (see docs/path-to-factorio.md)\n"
            "  --convert-base B  with --convert: each file at its input's path below B, not below its input\n"
            "  --convert-release with --convert: preprocess as the game's Release (NDEBUG), not FastDebug\n"
            "  --convert-final   with --convert: preprocess as the game's FinalRelease (Release, FINAL_RELEASE, ALL_IN_ONE)\n"
            "  --convert-marks   with --convert: mark each #if group, for --convert-merge\n"
            "  --convert-merge D R OUT [CHOICES [F]]  the configurations' marked conversions D and R (and the final F) as one, into OUT\n"
            "                root's name (B\\libraries\\Lua\\lapi.c as D\\libraries\\Lua\\lapi.kpp)\n"
            "  --c-sources   with --convert: the inputs are C (a C library), converted by C's rules\n"
            "  --convert-library NAME DIR  with --convert: the program uses the scoped library NAME, converted\n"
            "                into DIR: its names are written NAME::name (design.md 16)\n"
            "  --server      build through a compile server for these arguments, started if none runs\n"
            "                (docs/compile-speed.md, \"The compile server: protocol\")\n"
            "  --stop-server stop the compile server of the other arguments\n"
            "  --stop-all-servers  alone: stop every compile server of this user (a server also exits by\n"
            "                itself once the kppc it was started from changes, or after an idle hour)\n"
            "  --generate-visual-studio-files, --generate-vs-code-files  write project files for the build the\n"
            "                other arguments describe, debug and release, listing every source file, instead of\n"
            "                building (build\\visual-studio\\, .vscode\\; docs/build.md, \"Project files for an IDE\")\n"
            "  --run F ARGS  build F (a file or directory) with the options before --run and run it with ARGS;\n"
            "                the exit code is the program's (docs/design.md, \"Running and testing kpp code\");\n"
            "                it builds through the compile server unless --no-server is among the options\n"
            "  --tests       build the program's `test \"name\" { }` declarations with the test runner as main\n"
            "                (config::tests is true; docs/design.md 8.6)\n"
            "  --test F... [-- ARGS]  build the tests of F (files or directories) with the options before --test\n"
            "                and run them; ARGS go to the runner: --filter TEXT, --list, --report FILE\n"
            "  --no-run      with --run or --test: build only, and name the executable (one for another\n"
            "                --target, which this machine does not run, needs it)\n");
  }

  // `kpp --version` (docs/build.md, "Versions"): the version and commit kppc was built from, the
  // compiler, the toolchain a build takes without --toolchain, and the standard library.
  int printVersion()
  {
    std::filesystem::path compiler = compilerPath();
    if (*kppCommit())
      printf("kpp %s, commit %s\n", kppVersion(), kppCommit());
    else
      printf("kpp: version unknown (built outside a git checkout)\n");
    printf("compiler: %s\n", compiler.string().c_str());
    std::string problem = selectToolchain(compiler, "");
    std::string clang = problem.empty() ? clangPath() : "";
    const char* kind = bundledToolchain()      ? "bundled: builds windows-gnu by default"
                       : getenv("KPP_CLANG") ? "KPP_CLANG's"
                                             : "Visual Studio's: builds windows by default";
    std::error_code ec;
    if (!problem.empty())
      printf("toolchain: none: %s\n", problem.c_str());
    else if (std::filesystem::exists(clang, ec))
      printf("toolchain: %s (%s)\n", clang.c_str(), kind);
    else
      printf("toolchain: none: %s (%s) does not exist\n", clang.c_str(), kind);
    // Visual Studio's too where it is not the default: whether --toolchain msvc (and a build.kpp's
    // msvc,bundled) finds it here, and if not, what to install.
    if (!problem.empty() || bundledToolchain() || getenv("KPP_CLANG"))
    {
      std::string missing, msvc = visualStudioToolchain(missing);
      if (msvc.empty())
        printf("toolchain msvc: none: %s\n", missing.c_str());
      else
        printf("toolchain msvc: %s (Visual Studio's: --toolchain msvc)\n", msvc.c_str());
    }
    printf("back end: %s\n", inProcessBackend() ? "LLVM's, in kppc, for a debug build of windows or windows-gnu (clang otherwise)" : "clang's");
    // The sysroots --target freebsd and linux take without --sysroot (FreeBSD's always shown: a release
    // brings it; Linux's when there is one).
    for (const char* name : {"freebsd-x86_64", "linux-x86_64"})
    {
      std::string what;
      std::filesystem::path root = defaultSysroot(compiler, name, &what);
      if (!root.empty())
        printf("sysroot %s: %s%s\n", name, root.string().c_str(), what.empty() ? "" : (" (" + what + ")").c_str());
      else if (std::string_view(name) == "freebsd-x86_64")
        printf("sysroot %s: none (--target freebsd needs --sysroot)\n", name);
    }
    std::filesystem::path stdlib = defaultStdlib(compiler);
    printf("stdlib: %s%s\n", stdlib.string().c_str(), std::filesystem::is_directory(stdlib, ec) ? "" : " (missing)");
    // The official libraries use("NAME") finds when a project has no copy of its own.
    std::filesystem::path libraries = defaultLibraries(compiler);
    std::string names;
    for (std::filesystem::directory_iterator i(libraries, ec), end; !ec && i != end; i.increment(ec))
      if (std::filesystem::is_regular_file(i->path() / "build.kpp", ec))
        names += (names.empty() ? "" : ", ") + i->path().filename().string();
    printf("libraries: %s (%s)\n", libraries.string().c_str(), names.empty() ? "none" : names.c_str());
    return 0;
  }

  struct FileStats
  {
    uint64_t bytes = 0;
    uint64_t tokens = 0;
    uint64_t lines = 0;
    uint64_t errors = 0;
  };

  template<typename F>
  void parallelFor(size_t count, unsigned threads, F&& body)
  {
    std::atomic<size_t> next{0};
    auto worker = [&]
    {
      for (;;)
      {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= count)
          return;
        body(i);
      }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t)
      pool.emplace_back(worker);
    worker();
    for (auto& t : pool)
      t.join();
  }

