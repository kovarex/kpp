// The driver: the link's command and flags; the declarations, in namespace driver (Driver.hpp
// includes this), the definitions in Link.cpp (main.cpp split, stage 2, 2026-11-05, moved without a
// change).
  // --defer-link: the link, written for kpp to run once the program's C libraries, built alongside,
  // are there (Project/Build.cpp's buildProgram): <exe>.link.command, and <exe>.link.pending, which says
  // this build's objects are new (a replayed build leaves it as it was).
  void deferLinkCommand(const Options& options, const std::string& command);


  // A --debug-info build's .natvis (natvis.hpp): the executable's name with .natvis.
  std::string natvisPath(const Options& options);


  std::string linkFlags(const Options& options, bool wideEntry);
  void placeSanitizerRuntime(const Options& options);
  // AddressSanitizer's runtime for macOS arm64, LLVM 19.1.7's libclang_rt.asan_osx_dynamic.dylib, which kpp
  // brings at its place in clang's resource directory beside kpp (lib\clang\19\lib\darwin: a release's own;
  // tools\sysroot\llvm-macos.ps1 puts it beside a build of kpp).
  std::filesystem::path macAddressSanitizerRuntime();


  // What follows the link of an optimized ELF build with --debug-info (` && llvm-objcopy ...`): its
  // DWARF moved into NAME.debug beside the executable, which keeps a .gnu_debuglink to it; empty
  // otherwise (and, its error said, when there is no llvm-objcopy).
  std::string splitDebugInfoCommand(const Options& options);


  // An optimized build's LTO options for lld beyond what clang's driver passes it: --lto full's code
  // generated in one partition per thread or --lto-partitions' (FinalRelease's /LTCG has /CGTHREADS;
  // lld's MinGW driver, windows-gnu's, has no such option: one), and --opt-level 3 for the link's
  // optimization (lld's own is 2).
  std::string ltoLinkFlags(const Options& options);


  // Whether the program's files define a global wmain and no main (the game's entry on Windows).
  bool definesWideEntryOnly(const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols);


  // Whether the program is entered through wmain, so that the link names wmainCRTStartup: on Windows,
  // a main that takes arguments (the generated wmain hands it the wide argv as UTF-8: codegen's
  // emitEntry) or, without a main, the program's own wmain (the game's).
  bool wideEntry(const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols, const Target& target);


  // Splits a flag string on spaces, keeping quoted paths whole (without the quotes).
  std::vector<std::string> splitFlags(const std::string& text);
  void importCHeaders(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, std::vector<std::unique_ptr<ParsedFile>>& parsed,
                      SymbolTable& symbols, const Options& options);
