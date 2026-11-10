// The driver: the command line's options, the report, usage and the version; parallelFor; the
// declarations, in namespace driver (Driver.hpp includes this), the definitions in Options.cpp
// (main.cpp split, stage 2, 2026-11-05, moved without a change).
  inline bool quietOutput = false;
  int report(const char* format, ...);


  // With --c-sources: a source file of the inputs that is C (stb_image's implementation is a .hpp),
  // not the configuration's `<config>` nor a .kpp file.
  bool isCSource(const std::filesystem::path& path);


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
    bool indentationWarnings = false; // --indentation-warnings
    bool thisWarnings = false; // --this-warnings
    bool thisPointer = false; // --this-pointer: the program's own files keep C++'s pointer (the warnings that convert from it, convert.cmd)
    bool thisReference = false; // --this-reference: required `this.` and `this` the object (design.md 23c, 23d), not in the standard library's files
    bool thisReferenceWarnings = false; // --this-reference-warnings
    bool plainEnumWarnings = false; // --plain-enum-warnings
    bool structRule = true;      // design.md "struct and class" (--struct-rule, the default)
    bool cTypeWarnings = false;  // --c-type-warnings
    bool quiet = false;
    std::string emitLLVM; // --emit-llvm <path>
    std::string implicitUses; // --implicit-uses <path> (source.hpp's setImplicitUses)
    std::string exe;      // --exe <path>
    std::string runBuild; // --run-build CONFIGURATION: print build.kpp's description (GlobalScope::runBuild)
    std::string stdDir;   // --std <dir>; default: <kpp dir>\..\..\stdlib when it exists
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
    std::vector<std::string> thisReferenceLibraries; // --this-reference-library NAME: that library's sources are --this-reference's (design.md 23d)
    std::vector<std::string> cIncludeDirs; // --cinclude DIR, for imported C headers
    std::vector<std::string> libs;         // --lib FILE: libraries linked into the executable
    std::vector<std::string> natvisFiles;  // --natvis FILE: the program's own .natvis, linked into the PDB
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
    bool verifyIncremental = false; // --verify-incremental: a compile server's build checked against one from scratch (server.cpp);
                                    // here: an object whose line tables were patched is compared with the module's compile
    std::string toolchain;         // --toolchain bundled|msvc: which clang and linker (source.hpp's selectToolchain)
    bool debugInfo = false;        // --debug-info: line tables (docs/debugging.md)
    bool check = false;            // --check: codegen's errors too, nothing written (`kpp --lsp`'s checks)
    bool packages = false;         // --packages: the code packages and what each file declares (design.md, "Code packages")
    Target target = hostTarget();  // --target: the platform the program is built for (the host's by default)
    bool targetChosen = false;     // --target was given (else the toolchain's own: windows-gnu for the bundled one)
    std::string sysroot;           // --sysroot: the target's headers and libraries, for another platform than the host
  };

  // Whether the build generates code: an executable, the IR, or --check's errors from codegen.
  inline bool generatesCode(const Options& o) { return !o.emitLLVM.empty() || !o.exe.empty() || o.check; }


  void usage(FILE* out = stderr);


  // `kpp --version` (docs/build.md, "Versions"): the version and commit kpp was built from, the
  // compiler, the toolchain a build takes without --toolchain, and the standard library.
  int printVersion();


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
