// kppc: the kpp compiler. Currently the speed-test prototype: parallel lexing of a file tree.
#include "lexer.hpp"
#include "codegen.hpp"
#include "convert.hpp"
#include "parser.hpp"
#include "resolver.hpp"
#include "ast_walk.hpp"
#include "generate.hpp"
#include "project.hpp"
#include "run.hpp"
#include "tests.hpp"
#include "server.hpp"
#include "lsp.hpp"
#include "source.hpp"
#include "backend.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <cctype>
#include <cstring>
#include <exception>
#include <functional>
#include <set>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h> // _getpid
#endif

namespace
{
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
    bool structRule = false;     // --struct-rule
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
    std::vector<std::string> excluded;     // --exclude FILE: a file under the roots left out
    bool showImportErrors = false; // --errors given: list parse errors in imported headers too
    ConfigValues configValues;     // --config name=value, --release; becomes the `config` namespace
    bool debug = true;             // config::debug
    int optimize = -1;             // --optimize 1, --no-optimize 0, unset -1: clang -O2 (ThinLTO, a whole link) when !debug
    unsigned modules = 0;          // --modules N: IR modules compiled in parallel (0: 2 per thread, 1: one module)
    unsigned codegenThreads = 0;   // --codegen-threads N: threads emitting IR (0: --threads)
    std::string convertDir;        // --convert DIR: write the inputs as kpp sources there
    const SourceConfiguration* configuration = nullptr; // converting: the #if branches the front end and the converter keep
    std::string overridesDir;      // --overrides DIR: hand-written files that replace converted ones
    std::string convertBase;       // --convert-base DIR: --convert writes each file at its path below DIR
    bool cSources = false;         // --c-sources: the inputs are C, not the C++ Factorio compiles Lua as (convert.hpp)
    int repeat = 1;
    bool keepGoing = false;        // --keep-going: compile and link what codegen emitted despite its errors
    bool stubMissing = false;      // --stub-missing (with --keep-going): what was left out aborts with its name when called
    bool noInline = false;         // --no-inline: every call stays a call (no trivial callee emitted in its caller)
    std::string clangFlags;        // --clang-flags: more flags for compiling the modules (-fsanitize=address)
    bool incrementalLink = false;  // --incremental-link: a debug build's link redoes only the objects that changed
    bool deferLink = false;        // --defer-link: the link command written for kpp to run (project.cpp), not run
    std::string toolchain;         // --toolchain bundled|msvc: which clang and linker (source.hpp's selectToolchain)
    bool debugInfo = false;        // --debug-info: line tables (docs/debugging.md)
    bool check = false;            // --check: codegen's errors too, nothing written (`kppc --lsp`'s checks)
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
            "       kpp new, kpp options; docs/build.md)\n"
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
            "  --struct-rule  a struct is a public value type: one with a private or protected section, a base that is not\n"
            "                public or a virtual function, or deriving from a polymorphic class is an error (docs/design.md,\n"
            "                \"struct and class\"; the default once the converted game has none)\n"
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
            "  --lsp [OPTIONS SOURCES]  a language server for an editor, on stdin and stdout (docs/ide.md)\n"
            "  --std DIR     standard library sources (default: stdlib next to the compiler)\n"
            "  --no-std      compile without the standard library\n"
            "  --library-root NAME DIR  the sources DIR (a directory or a file) are the library NAME's, whose\n"
            "                names are written NAME::name outside it (repeatable; design.md 16)\n"
            "  --unscoped-library-root NAME DIR  the same for an unscoped library: its names are bare everywhere\n"
            "  --cinclude D  directory searched by imported C headers (repeatable)\n"
            "  --lib F       a library (.lib) linked into the executable (repeatable)\n"
            "  --exclude F   leave out the file F found under the sources (repeatable; build.kpp's exclude)\n"
            "  --config N=V  build configuration constant config::N (bool, integer, enumerator or string)\n"
            "  --release     the same as --config debug=false\n"
            "  --optimize, --no-optimize  clang -O2 (ThinLTO, a whole link) or -O0, whatever config::debug is\n"
            "                (default: -O2 when config::debug is false; build.kpp's fastdebug: asserts in, optimized)\n"
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
            "                and link.exe (the default otherwise, and what --incremental-link needs; design.md 17)\n"
            "  --debug-info  line tables in the objects (CodeView) and a .pdb next to the executable: a debugger\n"
            "                steps through the kpp source (docs/debugging.md)\n"
            "  --target T    build for T: windows (the default), linux, freebsd, or a triple (docs/design.md,\n"
            "                \"Targets and hosts\"); sets config::platform\n"
            "  --sysroot D   the target's C headers and libraries, for a target other than the host\n"
            "                (tools/sysroot/ makes one for Linux from WSL)\n"
            "  --convert D   rewrite the C++ inputs as kpp sources under D (see docs/path-to-factorio.md)\n"
            "  --convert-base B  with --convert: each file at its input's path below B, not below its input\n"
            "                root's name (B\\libraries\\Lua\\lapi.c as D\\libraries\\Lua\\lapi.kpp)\n"
            "  --c-sources   with --convert: the inputs are C (a C library), converted by C's rules\n"
            "  --server      build through a compile server for these arguments, started if none runs\n"
            "                (docs/compile-speed.md, \"The compile server: protocol\")\n"
            "  --stop-server stop the compile server of the other arguments\n"
            "  --generate-visual-studio-files, --generate-vs-code-files  write project files for the build the\n"
            "                other arguments describe, debug and release, listing every source file, instead of\n"
            "                building (build\\visual-studio\\, .vscode\\; docs/build.md, \"Project files for an IDE\")\n"
            "  --run F ARGS  build F (a file or directory) with the options before --run and run it with ARGS;\n"
            "                the exit code is the program's (docs/design.md, \"Running and testing kpp code\");\n"
            "                it builds through the compile server unless --no-server is among the options\n"
            "  --tests       build the program's `test \"name\" { }` declarations with the test runner as main\n"
            "                (config::tests is true; docs/design.md 8.6)\n"
            "  --test F... [-- ARGS]  build the tests of F (files or directories) with the options before --test\n"
            "                and run them; ARGS go to the runner: --filter TEXT, --list, --report FILE\n");
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
    printf("back end: %s\n", inProcessBackend() ? "LLVM's, in kppc, for a debug build of windows or windows-gnu (clang otherwise)" : "clang's");
    std::filesystem::path stdlib = defaultStdlib(compiler);
    printf("stdlib: %s%s\n", stdlib.string().c_str(), std::filesystem::is_directory(stdlib, ec) ? "" : " (missing)");
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

  double seconds(std::chrono::steady_clock::duration d);
  int codegenExit = 0;
  std::string lastImportStamps; // the C import's header stamps (importCHeaders), for a server's next build
  std::string lastImportsLine;  // the imports report of the build running (importCHeaders)

  // A function's body in a file's tokens: its `{` and the matching `}`; eligible when what depends
  // on it outside it can be found: not when a return type is deduced from it (callers elsewhere are
  // typed by it).
  struct Body
  {
    FunctionDecl* f;
    uint32_t open, close;
    bool eligible;
    bool templated; // a template's or a class template's: its instances are checked again (step 4)
    bool constant;  // constexpr or consteval: what the evaluator computed by running it is found (step 4)
  };
  // Everything one build made, from the files read to the resolved program. A compile server keeps
  // it for the next request: a request that changes only function bodies is built from it, the
  // changed bodies checked again and nothing else (docs/compile-speed.md, step 3b).
  struct Build
  {
    Options options;
    std::vector<SourceFile> files;
    std::vector<LexResult> lexed;
    std::unique_ptr<SymbolTable> symbols;
    std::unique_ptr<SourceConfiguration> configuration;
    std::vector<std::unique_ptr<ParsedFile>> parsed;
    std::unique_ptr<GlobalScope> global;
    std::vector<ResolveStats> stats;
    std::vector<int> fileLibrary; // (files[i]'s: its options.libraryRoots entry, -1 for the program's; the files the driver adds are the program's)
    // What the reports print that a later build of the kept program cannot recompute.
    std::string importsLine, indexLine;
    double indexTime = 0, finishTime = 0; // (the index line again, when a later build changed the entries)
    size_t indexScopes = 0, indexEntries = 0; // as the index left them (the resolve adds instances' after)
    std::vector<std::string> clashes;
    size_t parseErrors = 0; // in the program's files (not the C import's): the build stops before codegen
    double resolveTime = 0;
    GlobalScope::InstantiationStats instantiation{}; // at the end of the resolve (codegen instantiates more)
    ResolveStats instanceStats;
    bool keepable = false; // the whole pipeline ran as one build of the files (not --repeat, --convert, --dump)
    // The parses later versions of files were taken from (step 3b): their arenas hold the nodes
    // grafted into the kept files.
    std::vector<std::unique_ptr<ParsedFile>> grafts;
    // Codegen's kept state (step 5b) and what the next build changes for it; each module's size in
    // bytes as last built (the IR report counts the modules rebuildLLVM did not assemble again).
    // Declared last: destroyed first, before the program it points into.
    std::vector<size_t> moduleBytes;
    std::vector<FunctionDecl*> changedBodies, addedFunctions, removedFunctions;
    // The classes whose data members an incremental build changed (step 4's layout edits): codegen
    // lays them out again (rebuildLLVM).
    std::vector<const Decl*> changedLayouts;
    // The templates of the instances an incremental build checked again or took out (they read a
    // change: recheckInstancesOf): codegen emits their instances again.
    std::vector<const Decl*> instancesOf;
    // The globals whose initializers read a changed or edited name: codegen emits their constructors
    // again (rebuildLLVM).
    std::vector<const Decl*> changedGlobals;
    bool rebuildCodegen = false;
    // The build generated no code (parse errors, a clash; reportResolve): codegen's kept session has
    // not seen what it changed, which the next build that generates code emits too (incrementalBuild).
    bool codegenBehind = false;
    // The program's files' stamps (as listed) and their texts' hashes, by the files' order: a compile
    // server's next build does not read a file whose stamp is the same again (compile).
    std::vector<FileStamp> stamps;
    std::vector<uint64_t> textHashes;
    // An incremental build moved a kept local type (a lambda's closure, a local class) or renamed a
    // closure, or moved a template's position-named entity: names in the code are the position's
    // (`f::__lambda_3_1@2045`, a static local's `.2051`), so codegen runs in full (its session keeps
    // the types' names). See remapKeptFile.
    bool positionalCodegenInFull = false;
    CodegenSessionPtr codegen;
    // The session a codegen in full replaced: freed once the build has answered its client
    // (incrementalBuild), not on the build's own time (about 1 s on the game).
    CodegenSessionPtr retiredCodegen;
  };
  std::mutex keptMutex;              // held by a server's build until it has kept its result
  std::unique_ptr<Build> keptBuild;  // a server's last build (under keptMutex)

  // What the executable links against: legacy_stdio_definitions (the printf family the UCRT
  // headers define inline), the Windows libraries programs commonly reach, --lib files; unused
  // functions go (/OPT:REF).
  // wideEntry: the program defines wmain and no main, so the link names wmainCRTStartup itself. Left to
  // infer the entry, lld-link takes a main that any library has when the program's objects are
  // bitcode (a ThinLTO release build): the game's opus.lib carries opus_compare.obj, and the release
  // game ran opus_compare's main, "found both wmain and main; using latter" (docs/path-to-factorio.md,
  // "The release build").
  // A debug build's link folds no identical functions (/OPT:NOICF): folding took 1.5 s of the game's
  // 4.5 s link, and folded functions share an address, so a crash report or a debugger names one of
  // them for the other. --incremental-link keeps every function instead (/OPT:NOREF, which
  // /INCREMENTAL needs) and relinks only the objects that changed (docs/compile-speed.md, "The link in
  // the edit loop").
  // What makes clang compile for the target: nothing for Windows' MSVC dialect (Visual Studio's
  // clang's default), the triple otherwise (a module's own triple does not decide clang's: clang
  // overrides it with its default).
  std::string targetFlags(const Options& options)
  {
    return options.target.msvc() ? std::string() : " --target=" + options.target.triple;
  }

  // What a program in Windows' GNU dialect (windows-gnu) links besides MinGW-w64's libraries, made
  // once by the tools next to clang into the import cache (%TEMP%\kpp-imports), under a name with the
  // hash of what they are made from (another compiler making one at once makes the same):
  // - an import library of ucrtbase.dll's functions that MinGW-w64's leave out: the UCRT's C++
  //   runtime's (its own is libc++'s) and its long double ones, which are doubles as MSVC's are;
  // - those of math.h's long double functions on doubles: kpp's long double is a double, as MSVC's
  //   (docs/build.md, "The toolchain"), and libmingwex's are x87's 80 bits.
  // The files, quoted for the link; empty, with an error printed, when one cannot be made.
  std::string gnuRuntime(const Options& options)
  {
    static const char* def = "LIBRARY ucrtbase.dll\nEXPORTS\n__uncaught_exceptions\nstrtold\nwcstold\n_strtold_l\n";
    static const char* longDouble =
      "/* kpp's long double is a double, as MSVC's: MinGW-w64's long double functions on doubles. */\n"
      "#define F1(f) long double f##l(long double x) { return __builtin_##f(x); }\n"
      "#define F2(f) long double f##l(long double x, long double y) { return __builtin_##f(x, y); }\n"
      "F1(acos) F1(acosh) F1(asin) F1(asinh) F1(atan) F1(atanh) F1(cbrt) F1(ceil) F1(cos) F1(cosh) F1(erf) F1(erfc)\n"
      "F1(exp) F1(exp2) F1(expm1) F1(fabs) F1(floor) F1(lgamma) F1(log) F1(log10) F1(log1p) F1(log2) F1(logb)\n"
      "F1(nearbyint) F1(rint) F1(round) F1(sin) F1(sinh) F1(sqrt) F1(tan) F1(tanh) F1(tgamma) F1(trunc)\n"
      "F2(atan2) F2(copysign) F2(fdim) F2(fmax) F2(fmin) F2(fmod) F2(hypot) F2(nextafter) F2(pow) F2(remainder)\n"
      "long double fmal(long double x, long double y, long double z) { return __builtin_fma(x, y, z); }\n"
      "long double frexpl(long double x, int* e) { return __builtin_frexp(x, e); }\n"
      "long double ldexpl(long double x, int e) { return __builtin_ldexp(x, e); }\n"
      "long double modfl(long double x, long double* i) { double d; double r = __builtin_modf(x, &d); *i = d; return r; }\n"
      "long double nanl(const char* tag) { return __builtin_nan(tag); }\n"
      "long double nexttowardl(long double x, long double y) { return __builtin_nextafter(x, y); }\n"
      "long double remquol(long double x, long double y, int* q) { return __builtin_remquo(x, y, q); }\n"
      "long double scalblnl(long double x, long n) { return __builtin_scalbln(x, n); }\n"
      "long double scalbnl(long double x, int n) { return __builtin_scalbn(x, n); }\n"
      "int ilogbl(long double x) { return __builtin_ilogb(x); }\n"
      "long lrintl(long double x) { return __builtin_lrint(x); }\n"
      "long lroundl(long double x) { return __builtin_lround(x); }\n"
      "long long llrintl(long double x) { return __builtin_llrint(x); }\n"
      "long long llroundl(long double x) { return __builtin_llround(x); }\n"
      "long double _chgsignl(long double x) { return -x; }\n";
    std::filesystem::path tools = std::filesystem::path(clangPath()).parent_path();
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "kpp-imports";
    std::filesystem::create_directories(dir, ec);
    // Makes `stem-HASH.ext` from text (written to `stem-PID.in`) with command(input, output).
    auto made = [&](const std::string& stem, const std::string& ext, const std::string& text,
                    const std::function<std::string(const std::string&, const std::string&)>& command) -> std::string
    {
      uint64_t hash = 1469598103934665603ull;
      for (unsigned char c : text + command("", "")) // (the tools' path and flags too)
        hash = (hash ^ c) * 1099511628211ull;
      char hex[32];
      snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
      std::filesystem::path file = dir / (stem + "-" + hex + ext);
      if (std::filesystem::exists(file, ec))
        return file.string();
      std::string own = std::to_string(_getpid());
      std::filesystem::path input = dir / (stem + "-" + own + ".in"), output = dir / (stem + "-" + own + ext);
      if (FILE* f = fopen(input.string().c_str(), "wb"))
      {
        fputs(text.c_str(), f);
        fclose(f);
      }
      int rc = system(("\"" + command(input.string(), output.string()) + "\"").c_str());
      std::filesystem::remove(input, ec);
      if (rc != 0 || !std::filesystem::exists(output, ec))
      {
        fprintf(stderr, "error: could not make %s for windows-gnu (exit %d): %s\n", (stem + ext).c_str(), rc, command(input.string(), output.string()).c_str());
        return "";
      }
      std::filesystem::rename(output, file, ec);
      return ec ? output.string() : file.string();
    };
    std::string lib = made("libkpp_ucrt", ".a", def, [&](const std::string& in, const std::string& out)
    {
      return "\"" + (tools / "llvm-dlltool.exe").string() + "\" -m i386:x86-64 -d \"" + in + "\" -l \"" + out + "\"";
    });
    std::string object = made("kpp_long_double", ".o", longDouble, [&](const std::string& in, const std::string& out)
    {
      return "\"" + clangPath() + "\"" + targetFlags(options) + " -c -O2 -mlong-double-64 -w -x c \"" + in + "\" -o \"" + out + "\"";
    });
    if (lib.empty() || object.empty())
      return "";
    return " \"" + lib + "\" \"" + object + "\"";
  }

  // --defer-link: the link, written for kpp to run once the program's C libraries, built alongside,
  // are there (project.cpp's buildProgram): <exe>.link.command, and <exe>.link.pending, which says
  // this build's objects are new (a replayed build leaves it as it was).
  void deferLinkCommand(const Options& options, const std::string& command)
  {
    for (auto [suffix, text] : {std::pair<const char*, const std::string*>{".link.command", &command}, {".link.pending", nullptr}})
      if (FILE* f = fopen((options.exe + suffix).c_str(), "wb"))
      {
        if (text)
          fwrite(text->data(), 1, text->size(), f);
        fclose(f);
      }
      else
      {
        fprintf(stderr, "cannot write %s%s\n", options.exe.c_str(), suffix);
        codegenExit = 1;
      }
  }

  std::string linkFlags(const Options& options, bool wideEntry)
  {
    // Windows' GNU dialect (windows-gnu): lld's GNU driver, unused sections dropped as /OPT:REF
    // drops them, the Windows libraries programs commonly reach, ucrtbase's functions MinGW-w64's
    // import libraries leave out, -municode for a wmain, --lib files.
    if (options.target.gnu)
    {
      std::string flags = targetFlags(options).substr(1) + " -fuse-ld=lld -Wl,--gc-sections -luser32 -ladvapi32 -lshell32 -lole32 -lws2_32";
      flags += gnuRuntime(options);
      if (!options.sysroot.empty())
        flags += " \"--sysroot=" + options.sysroot + "\"";
      if (wideEntry)
        flags += " -municode";
      if (options.debugInfo)
        flags += " -g -Wl,--pdb=";
      for (const std::string& lib : options.libs)
        flags += " \"" + lib + "\"";
      return flags;
    }
    // Another target (ELF): ld.lld with the target's sysroot, unused sections dropped as /OPT:REF
    // drops them, the C library's math and threads (libm and pthreads are libraries of their own
    // there), --lib files.
    if (!options.target.windows())
    {
      std::string flags = targetFlags(options).substr(1) + " -fuse-ld=lld -Wl,--gc-sections -lm -pthread";
      if (!options.sysroot.empty())
        flags += " \"--sysroot=" + options.sysroot + "\"";
      for (const std::string& lib : options.libs)
        flags += " \"" + lib + "\"";
      return flags;
    }
    std::string flags = options.optimize           ? "-Wl,/OPT:REF"
                        : options.incrementalLink ? "-Wl,/OPT:NOREF -Wl,/INCREMENTAL"
                                                  : "-Wl,/OPT:REF -Wl,/OPT:NOICF";
    flags += " -llegacy_stdio_definitions -luser32 -ladvapi32 -lshell32 -lole32 -lws2_32";
    if (bundledToolchain()) // its lld-link (Visual Studio's link.exe is clang's default)
      flags += " -fuse-ld=lld";
    if (wideEntry)
      flags += " -Wl,/ENTRY:wmainCRTStartup";
    if (options.debugInfo)
      flags += " -Wl,/DEBUG";
    for (const std::string& lib : options.libs)
      flags += " \"" + lib + "\"";
    return flags;
  }

  // Whether the program's files define a global wmain and no main (the game's entry on Windows).
  bool definesWideEntryOnly(const std::vector<std::unique_ptr<ParsedFile>>& parsed, const SymbolTable& symbols)
  {
    bool wide = false;
    for (const auto& file : parsed)
      for (Decl* d : file->decls)
        if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
        {
          const QualifiedName& name = static_cast<FunctionDecl*>(d)->name;
          if (name.parts.size() != 1 || name.parts[0].special != 0 || name.parts[0].symbol == UINT32_MAX)
            continue;
          std::string_view spelled = symbols.name(name.parts[0].symbol);
          if (spelled == "main")
            return false;
          wide = wide || spelled == "wmain";
        }
    return wide;
  }

  // Splits a flag string on spaces, keeping quoted paths whole (without the quotes).
  std::vector<std::string> splitFlags(const std::string& text)
  {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (char c : text)
    {
      if (c == '"')
        quoted = !quoted;
      else if (c == ' ' && !quoted)
      {
        if (!current.empty())
          out.push_back(current);
        current.clear();
      }
      else
        current += c;
    }
    if (!current.empty())
      out.push_back(current);
    return out;
  }
  void importCHeaders(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, std::vector<std::unique_ptr<ParsedFile>>& parsed,
                      SymbolTable& symbols, const Options& options);

  // A --config value: true/false, an integer, or a word (an enumerator such as Windows, or a string).
  ConfigValue parseConfigValue(const std::string& text)
  {
    ConfigValue v;
    v.text = text;
    if (text == "true" || text == "false")
    {
      v.number = text == "true";
      return v;
    }
    char* end = nullptr;
    long long n = strtoll(text.c_str(), &end, 0);
    if (end && *end == 0 && !text.empty())
    {
      v.number = n;
      return v;
    }
    v.isNumber = false;
    return v;
  }

  // The `config` namespace as kpp source, one constant per --config value.
  std::string configSource(const ConfigValues& values)
  {
    std::string s = "// Build configuration, generated by kppc from --config name=value and --release.\n"
                    "enum class Platform { Windows, Linux, Mac, FreeBSD };\n"
                    "namespace config\n{\n";
    // A library's constant, LIB::NAME (docs/build.md, "kpp libraries": its options), is NAME in the
    // namespace config::LIB; the map's order keeps one library's together.
    std::string open;
    for (const auto& [qualified, value] : values)
    {
      size_t colons = qualified.rfind("::");
      std::string scope = colons == std::string::npos ? "" : qualified.substr(0, colons);
      std::string name = colons == std::string::npos ? qualified : qualified.substr(colons + 2);
      if (scope != open)
      {
        if (!open.empty())
          s += "  }\n";
        if (!scope.empty())
          s += "  namespace " + scope + "\n  {\n";
        open = scope;
      }
      std::string indent = scope.empty() ? "  " : "    ";
      if (name == "platform" && scope.empty())
        s += indent + "const Platform platform = Platform::" + value.text + ";\n";
      else if (!value.isNumber)
        s += indent + "const char* " + name + " = \"" + value.text + "\";\n";
      else if (value.text == "true" || value.text == "false")
        s += indent + "const bool " + name + " = " + value.text + ";\n";
      else
        s += indent + "const int " + name + " = " + std::to_string(value.number) + ";\n";
    }
    if (!open.empty())
      s += "  }\n";
    return s + "}\n";
  }

  std::string readWholeFile(const std::filesystem::path& path)
  {
    std::string text;
    readFile(path, text);
    return text;
  }

  // The files a make-style dependency list (clang's -MD) names after its target: words split at
  // unescaped blanks, `\` before a newline continuing the line, `\ ` a blank in a path.
  std::vector<std::string> makeDependencies(const std::string& text)
  {
    std::vector<std::string> words;
    std::string word;
    auto end = [&]()
    {
      if (!word.empty())
        words.push_back(std::move(word));
      word.clear();
    };
    for (size_t i = 0; i < text.size(); ++i)
    {
      char c = text[i];
      if (c == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
      {
        while (i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
          ++i;
        end();
      }
      else if (c == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '#'))
        word += text[++i];
      else if (c == '$' && i + 1 < text.size() && text[i + 1] == '$')
        word += text[++i];
      else if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        end();
      else
        word += c;
    }
    end();
    size_t first = 0;
    while (first < words.size() && words[first].back() != ':')
      ++first;
    return first < words.size() ? std::vector<std::string>(words.begin() + first + 1, words.end()) : std::vector<std::string>{};
  }

  // "size time path" of a file, what a cached import remembers of each header it read.
  std::string fileStamp(const std::string& path)
  {
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
      return {};
    auto time = std::filesystem::last_write_time(path, ec);
    if (ec)
      return {};
    return std::to_string(size) + " " + std::to_string(time.time_since_epoch().count()) + " " + path;
  }

  // Writes the stamps of the headers a dependency list names, one per line; false when there is no
  // list or a header cannot be stamped (no stamps: the import is never taken for a hit).
  bool writeHeaderStamps(const std::string& makeDeps, const std::filesystem::path& source, const std::filesystem::path& out)
  {
    std::vector<std::string> headers = makeDependencies(makeDeps);
    if (headers.empty())
      return false;
    std::string stamps;
    for (const std::string& h : headers)
    {
      if (std::filesystem::path(h).filename() == source.filename())
        continue; // the file of #includes clang was given, not a header (and deleted once done)
      std::string stamp = fileStamp(h);
      if (stamp.empty())
        return false;
      stamps += stamp + "\n";
    }
    FILE* f = fopen(out.string().c_str(), "wb");
    if (!f)
      return false;
    fwrite(stamps.data(), 1, stamps.size(), f);
    fclose(f);
    return true;
  }

  // Whether every header a cached import's stamps name still has its size and time.
  bool headersUnchanged(const std::filesystem::path& stampsPath)
  {
    std::string stamps = readWholeFile(stampsPath);
    if (stamps.empty())
      return false;
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < stamps.size();)
    {
      size_t end = stamps.find('\n', pos);
      if (end == std::string::npos)
        end = stamps.size();
      if (end > pos)
        lines.push_back(stamps.substr(pos, end - pos));
      pos = end + 1;
    }
    // (on every thread: the game's import reads hundreds of headers, a compile server's every build)
    std::atomic<bool> same{true};
    parallelFor(lines.size(), std::max(1u, std::thread::hardware_concurrency()), [&](size_t i)
    {
      const std::string& line = lines[i];
      size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
      if (same && (b == std::string::npos || fileStamp(line.substr(b + 1)) != line))
        same = false;
    });
    return same;
  }

  // The bodies of the object-like macros that call function-like ones (SDL3's
  // `SDL_WINDOW_FULLSCREEN SDL_UINT64_C(0x0000000000000001)`, windows.h's `E_FAIL
  // _HRESULT_TYPEDEF_(0x80004005L)`), expanded by clang: each such name on a line of its own after
  // the headers, behind a numbered marker. Name -> expansion.
  std::map<std::string, std::string> expandMacroCalls(const std::string& defines, const std::string& wrapper, const std::filesystem::path& source,
                                                      const std::string& clang, const std::string& includeArgs)
  {
    std::vector<std::string> names;
    for (size_t p = 0; p < defines.size();)
    {
      size_t end = defines.find('\n', p);
      if (end == std::string::npos)
        end = defines.size();
      std::string line = defines.substr(p, end - p);
      p = end + 1;
      if (line.rfind("#define ", 0) != 0 || line.compare(8, 2, "__") == 0)
        continue;
      size_t nameEnd = line.find_first_of(" (", 8);
      if (nameEnd == std::string::npos || line[nameEnd] == '(')
        continue;
      bool call = false;
      for (size_t i = nameEnd + 1; i < line.size() && !call; ++i)
        if (line[i] == '(')
        {
          size_t j = i;
          while (j > nameEnd + 1 && line[j - 1] == ' ')
            --j;
          call = j > nameEnd + 1 && (isalnum((unsigned char)line[j - 1]) || line[j - 1] == '_');
        }
      if (call)
        names.push_back(line.substr(8, nameEnd - 8));
    }
    std::map<std::string, std::string> out;
    if (names.empty())
      return out;
    std::string text = wrapper + "\n";
    for (size_t i = 0; i < names.size(); ++i)
      text += "__kpp_x_" + std::to_string(i) + "__ " + names[i] + "\n";
    std::filesystem::path expanded = source;
    expanded.replace_extension(".pp");
    FILE* w = fopen(source.string().c_str(), "wb");
    if (!w)
      return out;
    fwrite(text.data(), 1, text.size(), w);
    fclose(w);
    if (system(("\"\"" + clang + "\" -x c -w" + includeArgs + " \"" + source.string() + "\" -E -P -o \"" + expanded.string() + "\"\"").c_str()) != 0)
      return out; // the bodies stay as they are (not constants)
    std::string result = readWholeFile(expanded);
    for (size_t p = 0; p < result.size();) // the markers' lines (one with an empty expansion is the marker alone)
    {
      size_t end = result.find('\n', p);
      if (end == std::string::npos)
        end = result.size();
      std::string line = result.substr(p, end - p);
      p = end + 1;
      if (line.rfind("__kpp_x_", 0) != 0)
        continue;
      size_t close = line.find("__ ", 8);
      if (close == std::string::npos)
        continue;
      size_t index = size_t(strtoull(line.c_str() + 8, nullptr, 10));
      std::string body = line.substr(close + 3);
      while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
        body.pop_back();
      if (index < names.size() && !body.empty())
        out[names[index]] = body;
    }
    return out;
  }

  // Turns `#define NAME body` lines (clang -dM output) into kpp constant declarations where the
  // body looks like a constant: numbers, string literals, operators, parentheses and names. A body
  // that calls function-like macros is taken as clang expanded it (expansions).
  std::string macroConstants(const std::string& defines, const std::map<std::string, std::string>& expansions)
  {
    std::string out = "\n// Object-like macros, as constants.\n";
    // Names of object-like macros with a body: a body may refer to them (INFINITY uses _HUGE_ENUF).
    std::set<std::string> candidates;
    {
      size_t p = 0;
      while (p < defines.size())
      {
        size_t end = defines.find('\n', p);
        if (end == std::string::npos)
          end = defines.size();
        std::string line = defines.substr(p, end - p);
        p = end + 1;
        if (line.rfind("#define ", 0) != 0)
          continue;
        size_t nameEnd = line.find_first_of(" (", 8);
        if (nameEnd == std::string::npos || line[nameEnd] == '(' || nameEnd + 1 >= line.size())
          continue;
        candidates.insert(line.substr(8, nameEnd - 8));
      }
    }
    static const std::set<std::string> typeWords = {"float", "double", "int", "long", "short", "char", "unsigned", "signed", "const",
                                                    "sizeof", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
                                                    "uint32_t", "uint64_t", "intptr_t", "uintptr_t", "wchar_t", "bool", "true", "false",
                                                    "DWORD", "WORD", "BYTE", "HANDLE", "LONG", "ULONG", "UINT", "INT", "LONG_PTR", "ULONG_PTR",
                                                    "LPVOID", "SIZE_T", "DWORD_PTR", "HRESULT", "BOOL", "LONGLONG", "ULONGLONG", "UINT_PTR", "INT_PTR",
                                                    "clock_t", "time_t", "ptrdiff_t", "errno_t", "wint_t", "intmax_t", "uintmax_t", "UINT8", "UINT16",
                                                    "UINT32", "UINT64", "INT8", "INT16", "INT32", "INT64", "ULONG64", "LONG64", "DWORD64", "USHORT",
                                                    "SHORT", "UCHAR", "CHAR", "WCHAR"};
    std::map<std::string, std::string> integerTypes; // constants with a plain integer body, by name
    std::map<std::string, std::string> floatTypes;   // constants with a plain floating literal body (__FLT_MAX__), by name
    // `3.40282347e+38F`, `2.2204460492503131e-16`: a decimal floating literal.
    auto floatLiteral = [](const std::string& body)
    {
      return !body.empty() && isdigit((unsigned char)body[0]) && body.find_first_not_of("0123456789.eE+-fFlL") == std::string::npos &&
             body.find_first_of(".eE") != std::string::npos;
    };
    for (size_t p = 0; p < defines.size();)
    {
      size_t end = defines.find('\n', p);
      if (end == std::string::npos)
        end = defines.size();
      std::string line = defines.substr(p, end - p);
      p = end + 1;
      if (line.rfind("#define ", 0) != 0)
        continue;
      size_t nameEnd = line.find_first_of(" (", 8);
      if (nameEnd == std::string::npos || line[nameEnd] == '(' || nameEnd + 1 >= line.size())
        continue;
      std::string body = line.substr(nameEnd + 1);
      while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
        body.pop_back();
      if (auto e = expansions.find(line.substr(8, nameEnd - 8)); e != expansions.end())
        body = e->second;
      if (floatLiteral(body))
      {
        floatTypes[line.substr(8, nameEnd - 8)] = body.back() == 'f' || body.back() == 'F' ? "float" : "double";
        continue;
      }
      // A negative one in parentheses is plain too (corecrt_math.h's `_FINITE (-1)`, which FP_NORMAL names).
      if (body.size() > 3 && body.front() == '(' && body.back() == ')' && body[1] == '-')
        body = body.substr(2, body.size() - 3);
      if (body.empty() || !isdigit((unsigned char)body[0]) || body.find_first_not_of("0123456789xXabcdefABCDEFuUlLi") != std::string::npos ||
          (body.find('i') != std::string::npos && body.find("i64") == std::string::npos && body.find("i32") == std::string::npos) ||
          (body.find_first_of("xX") == std::string::npos && body.find_first_of("abcdefABCDEF") != std::string::npos))
        continue;
      bool wide = body.find_first_of("lL") != std::string::npos || body.find("i64") != std::string::npos;
      bool isUnsigned = body.find_first_of("uU") != std::string::npos;
      integerTypes[line.substr(8, nameEnd - 8)] = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
    }
    size_t pos = 0;
    while (pos < defines.size())
    {
      size_t end = defines.find('\n', pos);
      if (end == std::string::npos)
        end = defines.size();
      std::string line = defines.substr(pos, end - pos);
      pos = end + 1;
      if (line.rfind("#define ", 0) != 0)
        continue;
      size_t nameStart = 8;
      size_t nameEnd = line.find_first_of(" (", nameStart);
      if (nameEnd == std::string::npos || line[nameEnd] == '(')
        continue; // function-like or empty
      std::string name = line.substr(nameStart, nameEnd - nameStart);
      std::string body = line.substr(nameEnd + 1);
      while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
        body.pop_back();
      if (auto e = expansions.find(name); e != expansions.end())
        body = e->second;
      // __FILE__-style internals stay out, unless a plain number (__LONG_LONG_MAX__, __FLT_MAX__)
      bool internal = name.rfind("__", 0) == 0;
      if (name.empty() || body.empty() || (internal && body.find_first_not_of("0123456789xXabcdefABCDEFuUlL") != std::string::npos && !floatLiteral(body)))
        continue;
      bool hasNumber = false, ok = true, call = false, namesKnown = true;
      int quotes = 0;
      for (size_t i = 0; i < body.size(); ++i)
      {
        char c = body[i];
        bool prevIdent = i > 0 && (isalnum((unsigned char)body[i - 1]) || body[i - 1] == '_');
        if (c == '"')
          ++quotes;
        else if (isdigit((unsigned char)c))
          hasNumber = hasNumber || !prevIdent; // a digit inside a name (_SAL1_1_) is not a number
        else if (!(isalnum((unsigned char)c) || strchr("_ .+-*/()<>|&~!^,'\\xX", c)))
          ok = false;
        if ((isalpha((unsigned char)c) || c == '_') && !prevIdent && quotes % 2 == 0)
        {
          // An identifier: it must be a type word or another constant macro for the body to be a constant.
          size_t j = i;
          while (j < body.size() && (isalnum((unsigned char)body[j]) || body[j] == '_'))
            ++j;
          std::string word = body.substr(i, j - i);
          if (!typeWords.count(word) && !candidates.count(word))
            namesKnown = false;
        }
        if (c == '(')
        {
          size_t j = i;
          while (j > 0 && body[j - 1] == ' ')
            --j;
          if (j > 0 && (isalnum((unsigned char)body[j - 1]) || body[j - 1] == '_'))
            call = true; // `name(`: a function-like macro or a call, not a constant; `)(` in casts is fine
        }
      }
      bool aliasOfInteger = integerTypes.count(body) != 0; // DBL_MANT_DIG = __DBL_MANT_DIG__
      bool aliasOfFloat = floatTypes.count(body) != 0;     // FLT_MAX = __FLT_MAX__
      if (!ok || call || (quotes != 0 && quotes != 2) || !namesKnown ||
          (!hasNumber && quotes == 0 && body.find('(') == std::string::npos && !aliasOfInteger && !aliasOfFloat))
        continue;
      // A declared type where the body shows one: a cast, a string, a float or an integer literal.
      std::string type = "auto";
      if (body.rfind("((", 0) == 0 && body.find(')') != std::string::npos)
      {
        std::string cast = body.substr(2, body.find(')') - 2);
        bool word = !cast.empty();
        for (char ch : cast)
          if (!(isalnum((unsigned char)ch) || ch == '_' || ch == ' '))
            word = false;
        if (word && cast.find(' ') == std::string::npos)
          type = cast;
      }
      else if (quotes == 2)
        type = "char*";
      else if (hasNumber && body.find_first_of("\"") == std::string::npos && body.find_first_of("xX") == std::string::npos &&
               body.find_first_of(".eE") != std::string::npos)
        type = body.back() == 'f' || body.back() == 'F' ? "float" : "double";
      else if (hasNumber && body.find_first_not_of("0123456789xXabcdefABCDEFuUlLi()- +") == std::string::npos &&
               (body.find('i') == std::string::npos || body.find("i64") != std::string::npos || body.find("i32") != std::string::npos))
      {
        bool wide = body.find_first_of("lL") != std::string::npos || body.find("i64") != std::string::npos;
        bool isUnsigned = body.find_first_of("uU") != std::string::npos;
        type = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
      }
      if (type == "auto" && aliasOfInteger)
        type = integerTypes[body];
      if (type == "auto" && aliasOfFloat)
        type = floatTypes[body];
      if (type == "auto" && hasNumber && quotes == 0)
      {
        // Names of other integer constants with numbers and operators (LLONG_MIN = (-__LONG_LONG_MAX__-1LL)):
        // an integer of the widest kind seen.
        bool integer = true, wide = false, isUnsigned = false;
        for (size_t i = 0; i < body.size() && integer;)
        {
          char c = body[i];
          if (isalpha((unsigned char)c) || c == '_')
          {
            size_t j = i;
            while (j < body.size() && (isalnum((unsigned char)body[j]) || body[j] == '_'))
              ++j;
            std::string word = body.substr(i, j - i);
            auto known = integerTypes.find(word);
            bool suffix = i > 0 && isdigit((unsigned char)body[i - 1]) && word.find_first_not_of("uUlLi0123456789") == std::string::npos;
            if (known != integerTypes.end())
            {
              wide = wide || known->second.find("long long") != std::string::npos;
              isUnsigned = isUnsigned || known->second.find("unsigned") != std::string::npos;
            }
            else if (suffix)
            {
              wide = wide || word.find_first_of("lL") != std::string::npos || word.find("i64") != std::string::npos;
              isUnsigned = isUnsigned || word.find_first_of("uU") != std::string::npos;
            }
            else
              integer = false;
            i = j;
            continue;
          }
          if (c == '.' || c == '"' || c == '\'')
            integer = false;
          ++i;
        }
        if (integer)
          type = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
      }
      if (type.find("int") != std::string::npos || type.find("long") != std::string::npos)
        integerTypes[name] = type;
      out += "const " + type + " " + name + " = " + body + ";\n";
    }
    return out;
  }

  // Lexes one large text on several threads: the C import, 10 MB of preprocessed headers (the
  // Factorio build's), took 0.3 to 0.6 s on one thread, most of the import. The pieces end at
  // line ends with no continuation (no token of the preprocessed text spans one) and each is
  // lexed from a copy with its own terminator; the tokens are those of one lex of the whole.
  LexResult lexInChunks(std::string_view text, SymbolTable& symbols, unsigned threads)
  {
    const size_t piece = 256 * 1024;
    std::vector<size_t> starts{0};
    for (size_t at = piece; at < text.size();)
    {
      size_t nl = text.find('\n', at);
      while (nl != std::string_view::npos && nl > 0 && (text[nl - 1] == '\\' || (text[nl - 1] == '\r' && nl > 1 && text[nl - 2] == '\\')))
        nl = text.find('\n', nl + 1);
      if (nl == std::string_view::npos || nl + 1 >= text.size())
        break;
      starts.push_back(nl + 1);
      at = nl + 1 + piece;
    }
    if (starts.size() == 1)
      return lex(text, symbols);
    std::vector<LexResult> parts(starts.size());
    parallelFor(starts.size(), threads, [&](size_t k)
    {
      size_t from = starts[k], to = k + 1 < starts.size() ? starts[k + 1] : text.size();
      std::string copy(text.substr(from, to - from));
      parts[k] = lex(copy, symbols);
      for (Token& t : parts[k].tokens)
        t.offset += uint32_t(from);
    });
    LexResult whole;
    size_t count = 0;
    for (const LexResult& r : parts)
      count += r.tokens.size();
    whole.tokens.reserve(count);
    for (size_t k = 0; k < parts.size(); ++k)
    {
      std::vector<Token>& tokens = parts[k].tokens;
      if (k + 1 < parts.size() && !tokens.empty() && tokens.back().getKind() == TokenKind::End)
        tokens.pop_back(); // only the last piece ends the text
      whole.tokens.insert(whole.tokens.end(), tokens.begin(), tokens.end());
      whole.lineCount += parts[k].lineCount;
      whole.errorCount += parts[k].errorCount;
    }
    return whole;
  }

  // Collects `extern "C" import "header";` declarations, preprocesses the headers with clang
  // once (cached by content hash in the temp directory) and parses the result as one more file.
  // An imported C text for a SysV target: its va_list (clang's __builtin_va_list) is SysV x86-64's,
  // an array of one register save area (`__va_list_tag[1]`, 24 bytes), which C passes on as a pointer
  // to it; the resolver's __builtin_va_list is Windows' char*.
  std::string vaListForSysV(const std::string& text)
  {
    const std::string from = "__builtin_va_list", to = "__kpp_va_list";
    std::string out = "typedef struct __kpp_va_list_tag { unsigned int gp_offset; unsigned int fp_offset; void* overflow_arg_area; "
                      "void* reg_save_area; } __kpp_va_list[1];\n";
    out.reserve(text.size() + out.size());
    auto word = [&](size_t i) { return i < text.size() && (isalnum((unsigned char)text[i]) || text[i] == '_'); };
    for (size_t i = 0; i < text.size();)
    {
      if (text.compare(i, from.size(), from) == 0 && !(i > 0 && word(i - 1)) && !word(i + from.size()))
      {
        out += to;
        i += from.size();
      }
      else
        out += text[i++];
    }
    return out;
  }

  // The toolchain an import is made with, for its cache key: clang's version, the target triple and
  // _MSC_VER it gives, and the directories its `#include <...>` searches. clang picks the MSVC and
  // the Windows SDK from the environment (INCLUDE and the rest under VsDevCmd, else the newest Visual
  // Studio with MSVC: installing another one changes them), and _MSC_VER from that MSVC: under 14.44's
  // (1944) the UCRT's time.h defines time() and localtime() `static __inline`, under 14.51's not
  // (corecrt.h's _STATIC_INLINE_UCRT_FUNCTIONS), from the same header files.
  std::string clangToolchain(const std::string& clang, const std::string& includeArgs)
  {
    std::string out;
    FILE* pipe = _popen(("\"\"" + clang + "\" -x c -E -v" + includeArgs + " NUL -o NUL 2>&1\"").c_str(), "r");
    if (!pipe)
      return out;
    bool list = false;
    std::string line;
    char chunk[4096];
    while (fgets(chunk, sizeof chunk, pipe))
    {
      line += chunk;
      if (line.back() != '\n' && !feof(pipe))
        continue; // the cc1 line is longer than a chunk
      std::string s = std::move(line);
      line.clear();
      if (s.rfind("clang version", 0) == 0)
        out += s;
      else if (s.find("-cc1") != std::string::npos)
      {
        // The command line's other options name the working directory: only these two.
        for (const char* option : {"-triple ", "-fms-compatibility-version="})
          if (size_t at = s.find(option); at != std::string::npos)
            out += s.substr(at, s.find(' ', at + strlen(option)) - at) + "\n";
      }
      else if (s.rfind("#include <...>", 0) == 0)
        list = true;
      else if (s.rfind("End of search list", 0) == 0)
        list = false;
      else if (list)
        out += s;
    }
    _pclose(pipe);
    return out;
  }

  void importCHeaders(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, std::vector<std::unique_ptr<ParsedFile>>& parsed,
                      SymbolTable& symbols, const Options& options)
  {
    std::vector<std::string> headers;
    for (size_t i = 0; i < parsed.size(); ++i)
      for (Decl* d : parsed[i]->decls)
        if (d->kind == NodeKind::ImportDecl)
        {
          const Token& t = lexed[i].tokens[static_cast<ImportDecl*>(d)->pathToken];
          std::string path = files[i].text.substr(t.offset + 1, t.length - 2);
          if (std::find(headers.begin(), headers.end(), path) == headers.end())
            headers.push_back(path);
        }
    if (headers.empty())
      return;
    auto importStart = std::chrono::steady_clock::now();
    std::string wrapper = "#define WIN32_LEAN_AND_MEAN\n#define NOMINMAX\n#define _USE_MATH_DEFINES\n";
    for (const std::string& h : headers)
      // (intrin.h is MSVC's: another target's clang has the same SSE intrinsics in immintrin.h)
      wrapper += !options.target.windows() && h == "intrin.h" ? std::string("#include <immintrin.h>\n#include <x86intrin.h>\n") : "#include <" + h + ">\n";
    // MSVC's intrin.h declares these; clang's has them as builtins it declares itself. The code
    // generator emits each one's instruction (msvcBuiltin).
    if (std::find(headers.begin(), headers.end(), "intrin.h") != headers.end())
      wrapper += "unsigned short __popcnt16(unsigned short);\nunsigned int __popcnt(unsigned int);\nunsigned __int64 __popcnt64(unsigned __int64);\n"
                 "unsigned char _rotl8(unsigned char, unsigned char);\nunsigned char _rotr8(unsigned char, unsigned char);\n"
                 "unsigned short _rotl16(unsigned short, unsigned char);\nunsigned short _rotr16(unsigned short, unsigned char);\n";
    // malloc.h's alloca is `#define alloca _alloca` (only where __STDC__ is not defined, as it is
    // here), which the import does not carry: declared as the function, the code generator's
    // stack allocation like _alloca. MinGW-w64's malloc.h has _alloca a macro too (`__builtin_alloca`).
    if (std::find(headers.begin(), headers.end(), "malloc.h") != headers.end())
      wrapper += "#undef alloca\nvoid* alloca(size_t);\n#undef _alloca\nvoid* _alloca(size_t);\n";
    // The include directories as absolute paths, in the key and on clang's command line: a relative
    // `--cinclude native\include` names another directory in every worktree.
    std::error_code ec;
    // Another target's headers: clang preprocesses them for it, from its sysroot (in the key too).
    std::string includeArgs = targetFlags(options);
    if (!options.target.msvc() && !options.sysroot.empty())
      includeArgs += " \"--sysroot=" + options.sysroot + "\"";
    for (const std::string& dir : options.cIncludeDirs)
    {
      std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
      includeArgs += " -I\"" + (ec ? dir : absolute.lexically_normal().string()) + "\"";
    }
    std::string clang = clangPath();
    std::string toolchain = clangToolchain(clang, includeArgs);
    if (toolchain.find("clang version") == std::string::npos)
    {
      fprintf(stderr, "importing C headers failed: %s -v gave no version\n", clang.c_str());
      codegenExit = 1;
      return;
    }
    uint64_t hash = 1469598103934665603ull;
    // the version of what the cache holds (macroConstants' rules, vaListForSysV), then what it is made from
    for (unsigned char c : "kpp import 7\n" + wrapper + includeArgs + "\n" + toolchain)
      hash = (hash ^ c) * 1099511628211ull;
    std::filesystem::path cacheDir = std::filesystem::temp_directory_path(ec) / "kpp-imports";
    std::filesystem::create_directories(cacheDir, ec);
    char hex[32];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
    std::filesystem::path cached = cacheDir / (std::string(hex) + ".i");
    std::filesystem::path dependencies = cacheDir / (std::string(hex) + ".deps");
    std::string text;
    // A cached import is used only while every header clang read for it is as it was then (the
    // key names the headers imported, not what they hold: an edited kpp_directx.h is seen).
    bool cacheHit = std::filesystem::exists(cached, ec) && headersUnchanged(dependencies);
    lastImportStamps = dependencies.string();
    if (!cacheHit)
    {
      // Several compilers may build one import at once (the spec suite's): each works in files of its
      // own and renames the results into place whole.
      std::string own = std::string(hex) + "." + std::to_string(_getpid());
      std::filesystem::path wrapperPath = cacheDir / (own + ".c");
      std::filesystem::path preprocessed = cacheDir / (own + ".pp");
      std::filesystem::path defines = cacheDir / (own + ".dm");
      std::filesystem::path makeDeps = cacheDir / (own + ".d");
      FILE* w = fopen(wrapperPath.string().c_str(), "wb");
      if (!w)
      {
        fprintf(stderr, "cannot write %s\n", wrapperPath.string().c_str());
        return;
      }
      fwrite(wrapper.data(), 1, wrapper.size(), w);
      fclose(w);
      std::string base = "\"\"" + clang + "\" -x c -w" + includeArgs + " \"" + wrapperPath.string() + "\"";
      int rc = system((base + " -E -P -MD -MF \"" + makeDeps.string() + "\" -o \"" + preprocessed.string() + "\"\"").c_str());
      int rc2 = system((base + " -E -dM -o \"" + defines.string() + "\"\"").c_str());
      if (rc != 0 || rc2 != 0)
      {
        fprintf(stderr, "importing C headers failed (clang exit %d/%d)\n", rc, rc2);
        codegenExit = 1;
        return;
      }
      std::string definesText = readWholeFile(defines);
      std::map<std::string, std::string> expansions = expandMacroCalls(definesText, wrapper, cacheDir / (own + ".x.c"), clang, includeArgs);
      text = readWholeFile(preprocessed) + macroConstants(definesText, expansions);
      if (!options.target.windows())
        text = vaListForSysV(text);
      // The import, then its stamps (a reader that finds the import with older stamps checks those:
      // they still match only when the headers, and so the import, are the same).
      std::filesystem::path ownImport = cacheDir / (own + ".i"), ownStamps = cacheDir / (own + ".deps");
      if (FILE* out = fopen(ownImport.string().c_str(), "wb"))
      {
        fwrite(text.data(), 1, text.size(), out);
        fclose(out);
        std::filesystem::rename(ownImport, cached, ec);
        if (!ec && writeHeaderStamps(readWholeFile(makeDeps), wrapperPath, ownStamps))
          std::filesystem::rename(ownStamps, dependencies, ec);
      }
      for (const std::filesystem::path& p : {wrapperPath, preprocessed, defines, makeDeps, ownImport, ownStamps, cacheDir / (own + ".x.c"), cacheDir / (own + ".x.pp")})
        std::filesystem::remove(p, ec);
    }
    else
      text = readWholeFile(cached);
    // The imported declarations become one more file, parsed in C mode.
    SourceFile source;
    source.path = "<imported C headers>";
    source.text = std::move(text);
    source.text.reserve(source.text.size() + 1);
    files.push_back(std::move(source));
    lexed.push_back(lexInChunks(files.back().text, symbols, options.threads));
    parsed.push_back(std::make_unique<ParsedFile>());
    parsed.back()->path = files.back().path.string();
    parsed.back()->index = uint32_t(parsed.size() - 1);
    parsed.back()->isImported = true;
    parseFile(lexed.back().tokens, files.back().text, symbols, *parsed.back(), true, nullptr);
    for (size_t i = 0; i < parsed.size(); ++i)
    {
      // The vectors may have grown: re-point the parsed files at their tokens and text.
      parsed[i]->tokens = &lexed[i].tokens;
      parsed[i]->text = files[i].text;
    }
    char line[512];
    snprintf(line, sizeof line, "imports: %zu header(s) -> %zu declarations, %zu parse errors (ignored), %.3f s%s\n", headers.size(),
             size_t(parsed.back()->declCount), parsed.back()->diagnostics.size(), seconds(std::chrono::steady_clock::now() - importStart),
             cacheHit ? " (cached)" : "");
    lastImportsLine = line; // a compile server's incremental build prints it again (Build::importsLine)
    report("%s", line);
  }

  // The errors the checker found itself (a static_assert that fails or cannot be evaluated).
  // laidOut: codegen ran and checked the asserts on sizes and alignments (its errors), and the
  // static_asserts of the first instancesChecked class template instances.
  void reportCheckerErrors(GlobalScope& global, bool laidOut = false, size_t instancesChecked = 0)
  {
    for (const std::string& e : global.checkerErrors(instancesChecked))
    {
      fprintf(stderr, "%s\n", e.c_str());
      codegenExit = 1;
    }
    if (size_t waiting = global.layoutStaticAsserts().size())
      report(laidOut ? "static_assert: %zu on a class's size or alignment, checked with codegen's layouts\n"
                     : "static_assert: %zu on a class's size or alignment, left to the code generator's layout\n",
             waiting);
    if (size_t unused = global.unusedFailedAsserts())
      report("static_assert: %zu failed in template instances' functions never emitted (unused, so not errors, as in C++)\n", unused);
  }

  // --access-warnings: the warnings to stderr (no error: the exit code stays), the counts with the
  // statistics.
  void reportAccessWarnings(Build& build)
  {
    if (!build.options.accessWarnings)
      return;
    std::vector<ParsedFile*> files;
    for (const std::unique_ptr<ParsedFile>& p : build.parsed)
      files.push_back(p.get());
    std::string summary;
    for (const std::string& w : build.global->accessReport(files, summary))
      fprintf(stderr, "%s\n", w.c_str());
    report("%s\n", summary.c_str());
  }

  void runCodegen(Build& build)
  {
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    GlobalScope& global = *build.global;
    SymbolTable& symbols = *build.symbols;
    const Options& options = build.options;
    using Clock = std::chrono::steady_clock;
    if (!options.target.windows() && definesWideEntryOnly(parsed, symbols))
    {
      fprintf(stderr, "error: the program's entry is wmain, which is Windows'; for --target %s it defines main\n", options.target.name.c_str());
      codegenExit = 1;
      return;
    }
    // --emit-llvm wants the whole program in one file; an executable is built from several modules
    // compiled in parallel (one per group of files), unless --modules 1. A debug build has four per
    // thread: an edit compiles its module again, and the game's leaf edit through the server took
    // 1.60 to 1.91 s with 64 modules, 1.39 to 1.51 with 128, 1.28 to 1.41 with 256 (its clean build
    // 11.6, 11.4 and 14.5 s; docs/compile-speed.md, "The debug edit loop"). A release build has two
    // (more made its ThinLTO link slower).
    unsigned perThread = options.optimize ? 2 : 4;
    unsigned modules = options.modules ? options.modules : unsigned(std::min<size_t>(parsed.size(), perThread * std::max(1u, options.threads)));
    if (!options.emitLLVM.empty())
      modules = 1;
    auto start = Clock::now();
    // A compile server's incremental build: codegen's kept session emits again only what changed
    // (rebuildLLVM); it declines (an error, a change to the shared declarations) and the program is
    // generated in full, as when nothing is kept.
    CodegenResult cg;
    bool rebuilt = false;
    global.setCodeGenerated(true);
    build.codegenBehind = false;
    if (build.rebuildCodegen && build.codegen && build.moduleBytes.size() == modules)
    {
      cg = rebuildLLVM(*build.codegen, build.changedBodies, build.addedFunctions, build.removedFunctions, build.changedLayouts, build.instancesOf,
                       build.changedGlobals, rebuilt);
      logBuildNote(!rebuilt              ? "codegen: rebuildLLVM declined (" + cg.declined + "), the program generated in full"
                   : cg.failed()        ? "codegen: the session kept across " + std::to_string(cg.errors.size() + cg.hiddenErrors) + " error(s), " +
                                              std::to_string(cg.emittedAgain) + " function(s) emitted again"
                                        : "codegen: " + std::to_string(std::count(cg.changedModules.begin(), cg.changedModules.end(), true)) +
                                              " module(s) assembled again, " + std::to_string(cg.emittedAgain) + " function(s) emitted again");
    }
    build.rebuildCodegen = false;
    if (!rebuilt)
    {
      build.retiredCodegen = std::move(build.codegen);
      // (--check writes nothing: no IR assembled despite errors, so that a server keeps its session across them)
      bool despiteErrors = options.keepGoing && !options.check;
      cg = generateLLVM(parsed, global, symbols, options.debug, modules, despiteErrors, options.codegenThreads ? options.codegenThreads : options.threads,
                        despiteErrors && options.stubMissing, !options.noInline, servingBuild() ? &build.codegen : nullptr,
                        options.debugInfo, &options.target);
      cg.changedModules.clear();
    }
    double t = seconds(Clock::now() - start);
    // Errors assemble no module; a session kept across them keeps the sizes of the modules the last
    // build without errors assembled (it assembles the others once the errors are gone).
    if (cg.modules.empty() && cg.failed() && build.codegen)
    {
      if (build.moduleBytes.size() != modules)
        build.moduleBytes.assign(modules, 0);
    }
    else if (build.moduleBytes.size() != cg.modules.size())
      build.moduleBytes.assign(cg.modules.size(), 0);
    size_t irBytes = cg.ir.size();
    for (size_t k = 0; k < cg.modules.size(); ++k)
    {
      if (cg.changedModules.empty() || cg.changedModules[k])
        build.moduleBytes[k] = cg.modules[k].size();
      irBytes += build.moduleBytes[k];
    }
    report("codegen: %.3f s, %zu functions, %zu structs, %zu bytes of IR in %zu module(s)\n", t, cg.functions, cg.structs, irBytes,
           std::max<size_t>(1, cg.modules.size()));
    if (options.keepGoing && options.stubMissing && !options.check)
      printf("stubs: %zu functions left out are stubs that abort when called (--stub-missing)\n", cg.stubs);
    for (const std::string& e : cg.errors)
      fprintf(stderr, "%s\n", e.c_str());
    if (!cg.errors.empty())
      codegenExit = 1;
    reportCheckerErrors(global, true, cg.instanceAssertsChecked);
    reportAccessWarnings(build);
    if (options.check)
      return; // the errors are all it is for
    auto writeFile = [&](const std::string& path, const std::string& text)
    {
      FILE* out = fopen(path.c_str(), "wb");
      if (!out)
      {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        codegenExit = 1;
        return false;
      }
      fwrite(text.data(), 1, text.size(), out);
      fclose(out);
      return true;
    };
    std::string clang = clangPath();
    report("toolchain: %s (%s, %s: links with %s)\n", clang.c_str(),
           bundledToolchain() ? "bundled" : getenv("KPP_CLANG") ? "KPP_CLANG" : "Visual Studio's", options.target.name.c_str(),
           !options.target.msvc() ? "lld" : bundledToolchain() || options.optimize ? "lld-link" : "link.exe");
    // A release build of several modules is a ThinLTO build: the modules become bitcode, and the
    // link optimises them in parallel with what each needs from the others imported, so that calls
    // into another module's instantiations inline as they do in one module (optimised separately,
    // the library benchmarks in compiler/bench/stdlib ran up to 20 times slower).
    bool thinLTO = options.optimize && !cg.modules.empty();
    const char* level = !options.optimize ? "-O0" : thinLTO ? "-O2 -flto=thin" : "-O2";
    // Every function in its own section, so that the linker drops the ones nobody calls (and
    // the libraries they would need): a program pays only for what it uses.
    // (-ffp-contract=off: no multiply-add fused, every operation rounded as written; the game's
    // /fp:strict, design.md section 2)
    // -fverify-intermediate-code: LLVM's verifier checks the module, as the back end in kppc does
    // (src/backend.cpp). A release clang passes -disable-llvm-verifier by default, which took an IR
    // bug of kppc's in silently: at -O0 the module compiled (exit 0), at -O2 clang hung on it
    // (KPP_TEST_BROKEN_MODULE's use its definition does not dominate, tests\verifier.ps1).
    const char* compileFlags = "-ffunction-sections -fdata-sections -ffp-contract=off -fverify-intermediate-code";
    // --keep-going: the errors stay reported (and the exit code), but what was emitted is compiled and
    // linked; the functions errors left out are listed next to the executable for the link's report.
    bool stop = codegenExit && !options.keepGoing;
    if (options.keepGoing && !cg.failedFunctions.empty() && !options.exe.empty())
    {
      std::string list;
      for (const std::string& name : cg.failedFunctions)
        list += name + "\n";
      writeFile(options.exe + ".failed-functions.txt", list);
    }
    if (cg.modules.empty())
    {
      std::string llPath = options.emitLLVM.empty() ? options.exe + ".ll" : options.emitLLVM;
      if (!writeFile(llPath, cg.ir) || options.exe.empty() || stop)
        return;
      std::string command = "\"\"" + clang + "\" " + level + targetFlags(options) + " -w " + compileFlags + " \"" + llPath + "\" -o \"" + options.exe + "\" " +
                            linkFlags(options, definesWideEntryOnly(parsed, symbols)) + "\"";
      if (options.deferLink) // (one module: its compile goes with the link)
      {
        deferLinkCommand(options, command);
        report("clang:   deferred with the link\n");
        return;
      }
      start = Clock::now();
      int rc = system(command.c_str());
      report("clang:   %.3f s (%s, exit %d)\n", seconds(Clock::now() - start), level, rc);
      if (rc != 0)
      {
        fprintf(stderr, "%s: error: clang failed (exit %d); its messages are above\n", llPath.c_str(), rc); // (whatever --quiet says)
        codegenExit = 1;
      }
      return;
    }
    if (stop)
      return;
    std::vector<std::string> objects(cg.modules.size());
    std::atomic<int> failures{0}, cached{0};
    struct ModuleFailure
    {
      unsigned module;
      int rc; // clang's exit code; 0: LLVM's back end in this process
      std::string path, output;
    };
    std::vector<ModuleFailure> moduleFailures;
    std::mutex moduleFailuresMutex;
    start = Clock::now();
    // LLVM's back end in this process when kppc has it (src/backend.cpp, docs/codegen-speed.md): a
    // debug build's module for Windows (windows or windows-gnu, with --debug-info too) compiled from
    // its text in memory, no IR file written, no clang started. Its objects are clang's apart from
    // what names the compiler in CodeView (S_COMPILE3's back-end version, LF_BUILDINFO's tool and
    // command line, which kppc leaves empty). A release build (ThinLTO, lld), Linux and --clang-flags
    // (their objects not compared with clang's) stay clang's; so does any build with
    // KPP_BACKEND=clang (the comparison).
    const char* backendChoice = getenv("KPP_BACKEND");
    bool inProcess = inProcessBackend() && !options.optimize && options.target.windows() && options.clangFlags.empty() &&
                     !(backendChoice && std::string(backendChoice) == "clang");
    // KPP_TEST_BROKEN_MODULE=K (run_tests' "rejected module"): module K gets a function LLVM's verifier
    // rejects (a use its definition does not dominate), as an IR bug of kppc's would be.
    // KPP_TEST_BROKEN_ASM=K: module K gets an instruction the assembler rejects, which the verifier
    // passes and only the back end finds (LLVM's diagnostics, not its verifier).
    const char* brokenModule = getenv("KPP_TEST_BROKEN_MODULE");
    const char* brokenAsm = getenv("KPP_TEST_BROKEN_ASM");
    parallelFor(cg.modules.size(), options.threads, [&](size_t k)
    {
      std::string llPath = options.exe + "." + std::to_string(k) + ".ll";
      objects[k] = options.exe + "." + std::to_string(k) + ".o";
      if (!cg.changedModules.empty() && !cg.changedModules[k])
      {
        ++cached; // not assembled again: its text and object are the last build's (checked before rebuildLLVM ran)
        return;
      }
      if (brokenModule && std::to_string(k) == brokenModule)
        cg.modules[k] += "\ndefine void @kpp.test.broken() {\n  %a = add i32 %b, 1\n  %b = add i32 %a, 1\n  ret void\n}\n";
      if (brokenAsm && std::to_string(k) == brokenAsm)
        cg.modules[k] += "\nmodule asm \"kpp_no_such_instruction\"\n";
      // A module whose text (and optimisation level) did not change keeps its object file.
      uint64_t h = 1469598103934665603ull;
      for (unsigned char c : cg.modules[k])
        h = (h ^ c) * 1099511628211ull;
      for (const char* c = level; *c; ++c)
        h = (h ^ (unsigned char)*c) * 1099511628211ull;
      for (unsigned char c : options.clangFlags)
        h = (h ^ c) * 1099511628211ull;
      std::string hashText = std::to_string(h);
      std::string hashPath = options.exe + "." + std::to_string(k) + ".hash";
      std::error_code ec;
      if (readWholeFile(hashPath) == hashText && std::filesystem::exists(objects[k], ec))
      {
        ++cached;
        return;
      }
      if (inProcess)
      {
        std::string object, error;
        BackendOptions backend;
        backend.path = llPath;
        backend.module = unsigned(k);
        bool compiled = compileModule(cg.modules[k], backend, object, error);
        if (compiled && !writeFile(objects[k], object))
          error = "cannot write " + objects[k];
        if (!compiled || !error.empty())
        {
          ++failures;
          std::error_code removed;
          std::filesystem::remove(objects[k], removed); // (no object of an earlier build linked in its place)
          writeFile(llPath, cg.modules[k]); // (the text LLVM rejected, for the report)
          std::lock_guard lock(moduleFailuresMutex);
          moduleFailures.push_back({unsigned(k), 0, llPath, error});
        }
        else
          writeFile(hashPath, hashText);
        return;
      }
      if (!writeFile(llPath, cg.modules[k]))
      {
        ++failures;
        return;
      }
      // clang's messages go to a file of the module's, printed with the failure below (32 clangs
      // writing to one stderr at once interleave).
      std::string errPath = options.exe + "." + std::to_string(k) + ".clang.txt";
      std::string command = "\"\"" + clang + "\" " + level + targetFlags(options) + " -w " + compileFlags + " " + options.clangFlags + " -c \"" + llPath + "\" -o \"" + objects[k] +
                            "\" 2> \"" + errPath + "\"\"";
      if (int rc = system(command.c_str()); rc != 0)
      {
        ++failures;
        std::error_code removed;
        std::filesystem::remove(objects[k], removed); // (no object of an earlier build linked in its place)
        std::lock_guard lock(moduleFailuresMutex);
        moduleFailures.push_back({unsigned(k), rc, llPath, readWholeFile(errPath)});
      }
      else
      {
        writeFile(hashPath, hashText);
        std::error_code removed;
        std::filesystem::remove(errPath, removed); // (-w: nothing in it)
      }
    });
    double compileTime = seconds(Clock::now() - start);
    // A module clang or LLVM rejected is an error of the build whatever --quiet says, with what it said
    // (its first lines): with --keep-going the link goes on without its object and reports what it
    // would have defined, which names no cause.
    std::sort(moduleFailures.begin(), moduleFailures.end(), [](const ModuleFailure& a, const ModuleFailure& b) { return a.module < b.module; });
    for (const ModuleFailure& f : moduleFailures)
    {
      std::string said;
      size_t lines = 0;
      for (size_t at = 0; at < f.output.size() && lines < 6;)
      {
        size_t end = f.output.find('\n', at);
        if (end == std::string::npos)
          end = f.output.size();
        std::string line = f.output.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty() || line.starts_with(" #") || line.starts_with("Stack dump") || line.starts_with("PLEASE submit") || line.starts_with("Exception Code"))
          continue;
        said += "\n  " + line;
        ++lines;
      }
      std::string who = f.rc ? "clang rejected module " + std::to_string(f.module) + " (exit " + std::to_string(f.rc) + ")"
                             : "LLVM's back end rejected module " + std::to_string(f.module);
      fprintf(stderr, "%s: error: %s%s\n", f.path.c_str(), who.c_str(), said.empty() ? " and said nothing" : said.c_str());
    }
    if (failures)
    {
      report("clang:   %.3f s (%s, %zu modules, %d failed)\n", compileTime, level, cg.modules.size(), failures.load());
      codegenExit = 1;
      if (!options.keepGoing)
        return;
    }
    start = Clock::now();
    // The objects go through a response file: 64 paths do not fit a command line.
    std::string response;
    auto slashes = [](std::string path) // clang reads a backslash in a response file as an escape
    {
      for (char& c : path)
        if (c == '\\')
          c = '/';
      return path;
    };
    for (const std::string& o : objects)
    {
      std::error_code ec;
      if (options.keepGoing && !std::filesystem::exists(o, ec))
        continue; // a module clang rejected (--keep-going): the link reports what it would have defined
      response += "\"" + slashes(o) + "\"\n";
    }
    response += "-o \"" + slashes(options.exe) + "\"\n";
    if (thinLTO)
    {
      // lld runs the ThinLTO back ends on the compiler's threads, and keeps their results for the
      // modules that did not change (the cache next to the executable).
      response += "-fuse-ld=lld\n-flto=thin\n-O2\n";
      if (options.target.msvc())
      {
        response += "-Wl,/opt:lldltojobs=" + std::to_string(std::max(1u, options.threads)) + "\n";
        response += "\"-Wl,/lldltocache:" + slashes(options.exe) + ".ltocache\"\n";
      }
      else
      {
        response += "-Wl,--thinlto-jobs=" + std::to_string(std::max(1u, options.threads)) + "\n";
        response += "\"-Wl,--thinlto-cache-dir=" + slashes(options.exe) + ".ltocache\"\n";
      }
    }
    for (const std::string& flag : splitFlags(linkFlags(options, definesWideEntryOnly(parsed, symbols))))
    {
      // A library under "C:\Program Files (x86)\..." (the Windows SDK's) stays one argument.
      std::string one = slashes(flag);
      response += (one.find(' ') != std::string::npos ? "\"" + one + "\"" : one) + "\n";
    }
    std::string responsePath = options.exe + ".link.rsp";
    if (!writeFile(responsePath, response))
      return;
    std::string link = "\"\"" + clang + "\" @\"" + responsePath + "\"\"";
    if (options.keepGoing) // the link's complaints (unresolved externals) go to a file next to the executable
      link = "\"\"" + clang + "\" @\"" + responsePath + "\" > \"" + options.exe + ".link.txt\" 2>&1\"";
    if (options.deferLink)
    {
      deferLinkCommand(options, link);
      report("clang:   %.3f s (%s, %zu modules on %u threads, %d unchanged) + link deferred\n", compileTime, level, cg.modules.size(),
             options.threads, cached.load());
      return;
    }
    int rc = system(link.c_str());
    report("clang:   %.3f s (%s, %zu modules on %u threads, %d unchanged) + link %.3f s (exit %d)\n", compileTime, level, cg.modules.size(),
           options.threads, cached.load(), seconds(Clock::now() - start), rc);
    if (rc != 0)
      codegenExit = 1;
  }

  void runResolve(Build& build);
  void reportParse(Build& build, double parseTime);
  void reportResolve(Build& build);

  uint32_t lineOf(const std::string& text, uint32_t offset)
  {
    uint32_t line = 1;
    for (uint32_t i = 0; i < offset && i < text.size(); ++i)
      if (text[i] == '\n')
        ++line;
    return line;
  }

  // A byte offset's column, from 1 (--diagnostic-columns).
  uint32_t columnOf(const std::string& text, uint32_t offset)
  {
    uint32_t start = std::min<uint32_t>(offset, uint32_t(text.size()));
    while (start > 0 && text[start - 1] != '\n')
      --start;
    return 1 + offset - start;
  }

  // --c-type-warnings: C's names of the integer types (int, unsigned, short, long, long long, signed
  // and unsigned char, int32_t, size_t ...) are for C interop only (design.md, "Integer types and
  // sizes"): each one written outside an extern "C" declaration, the C headers and the standard
  // library is a warning naming kpp's type (u32, i64 ...). No error: the exit code stays.
  void warnCTypeNames(const std::vector<SourceFile>& files, const std::vector<LexResult>& lexed, const std::vector<std::unique_ptr<ParsedFile>>& parsed)
  {
    static const std::map<std::string_view, const char*> named = {
      {"int8_t", "i8"},    {"int16_t", "i16"},  {"int32_t", "i32"},  {"int64_t", "i64"},   {"uint8_t", "u8"},   {"uint16_t", "u16"},
      {"uint32_t", "u32"}, {"uint64_t", "u64"}, {"size_t", "usize"}, {"ptrdiff_t", "i64"}, {"intptr_t", "i64"}, {"uintptr_t", "u64"},
      {"ssize_t", "i64"},  {"__int8", "i8"},    {"__int16", "i16"},  {"__int32", "i32"},   {"__int64", "i64"}};
    auto typeKeyword = [](const Token& t) {
      return t.is(Kw::signed_) || t.is(Kw::unsigned_) || t.is(Kw::short_) || t.is(Kw::int_) || t.is(Kw::long_) || t.is(Kw::char_) || t.is(Kw::double_);
    };
    for (size_t i = 0; i < files.size(); ++i)
    {
      if (parsed[i]->isImported || parsed[i]->isLibrary)
        continue;
      // The token ranges of the extern "C" declarations (each flagged, in a block or not), in order.
      std::vector<std::pair<uint32_t, uint32_t>> externC;
      auto collect = [&](auto& self, Span<Decl*> decls) -> void {
        for (Decl* d : decls)
          if (d->flags & DF_ExternC)
            externC.push_back({d->token, std::max(d->endToken, d->token + 1)});
          else if (d->kind == NodeKind::NamespaceDecl)
            self(self, static_cast<NamespaceDecl*>(d)->decls);
      };
      collect(collect, parsed[i]->decls);
      std::sort(externC.begin(), externC.end());
      const std::vector<Token>& tokens = lexed[i].tokens;
      const std::string& text = files[i].text;
      auto spell = [&](const Token& t) { return std::string_view(text).substr(t.offset, t.length); };
      size_t range = 0;
      for (size_t k = 0; k < tokens.size(); ++k)
      {
        while (range < externC.size() && externC[range].second <= k)
          ++range;
        if (range < externC.size() && externC[range].first <= k)
        {
          k = externC[range].second - 1;
          continue;
        }
        const Token& t = tokens[k];
        std::string phrase;
        const char* kpp = nullptr;
        if (typeKeyword(t))
        {
          // A run of type keywords is one type: `unsigned long long`, `long double`, `signed char`.
          int longs = 0;
          bool isUnsigned = false, isSigned = false, isShort = false, isChar = false, isDouble = false;
          size_t e = k;
          for (; e < tokens.size() && typeKeyword(tokens[e]); ++e)
          {
            longs += tokens[e].is(Kw::long_);
            isUnsigned |= tokens[e].is(Kw::unsigned_);
            isSigned |= tokens[e].is(Kw::signed_);
            isShort |= tokens[e].is(Kw::short_);
            isChar |= tokens[e].is(Kw::char_);
            isDouble |= tokens[e].is(Kw::double_);
            phrase += (phrase.empty() ? "" : " ") + std::string(spell(tokens[e]));
          }
          if (e < tokens.size() && tokens[e].getKind() == TokenKind::Identifier && named.count(spell(tokens[e])))
          {
            phrase += " " + std::string(spell(tokens[e])); // `unsigned __int64`
            longs = spell(tokens[e]) == "__int64" ? 2 : longs;
            ++e;
          }
          if (isDouble)
            kpp = longs ? "f64" : nullptr; // (double itself is no integer name)
          else if (isChar)
            kpp = isUnsigned ? "u8" : isSigned ? "i8" : nullptr; // (char is the text character)
          else if (isShort)
            kpp = isUnsigned ? "u16" : "i16";
          else if (longs >= 2)
            kpp = isUnsigned ? "u64" : "i64";
          else if (longs == 1)
            kpp = isUnsigned ? "u32 (C's unsigned long is 32 bits on Windows)" : "i32 (C's long is 32 bits on Windows)";
          else
            kpp = isUnsigned ? "u32" : "i32";
          k = e - 1;
        }
        else if (t.getKind() == TokenKind::Identifier)
          if (auto it = named.find(spell(t)); it != named.end())
          {
            phrase = it->first;
            kpp = it->second;
          }
        if (kpp)
          fprintf(stderr, "%s: warning: %s is C's name, for C interop (extern \"C\"): write %s\n",
                  sourceLocation(files[i].path.string(), lineOf(text, t.offset), columnOf(text, t.offset)).c_str(), phrase.c_str(), kpp);
      }
    }
  }

  void runParse(Build& build)
  {
    using Clock = std::chrono::steady_clock;
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    SymbolTable& symbols = *build.symbols;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    parsed.resize(files.size());
    double parseTime = 1e9;
    for (int round = 0; round < std::max(1, options.repeat); ++round)
    {
      // Repeated rounds re-parse from the original tokens ('>>' splits are undone by re-lexing).
      std::vector<std::vector<Token>> tokens(files.size());
      if (round > 0)
        parallelFor(files.size(), options.threads, [&](size_t i) { tokens[i] = lexed[i].tokens; });
      auto start = Clock::now();
      parallelFor(files.size(), options.threads, [&](size_t i)
      {
        parsed[i] = std::make_unique<ParsedFile>();
        parsed[i]->path = files[i].path.string();
        parsed[i]->index = uint32_t(i);
        std::string library = options.libraryRoot.string();
        parsed[i]->isLibrary = !library.empty() && parsed[i]->path.compare(0, library.size(), library) == 0;
        if (i < build.fileLibrary.size() && build.fileLibrary[i] >= 0)
        {
          const Options::LibraryRoot& root = options.libraryRoots[size_t(build.fileLibrary[i])];
          parsed[i]->library = root.name;
          if (root.scoped)
            parsed[i]->libraryScope = symbols.intern(root.name);
        }
        parseFile(round == 0 ? lexed[i].tokens : tokens[i], files[i].text, symbols, *parsed[i], false, &options.configValues);
      });
      double t = seconds(Clock::now() - start);
      if (round > 0)
        report("parse round %d: %.3f s\n", round + 1, t);
      parseTime = std::min(parseTime, t);
      if (round + 1 < std::max(1, options.repeat))
        continue;
      if (round > 0)
        for (size_t i = 0; i < files.size(); ++i)
        {
          lexed[i].tokens = std::move(tokens[i]);
          parsed[i]->tokens = &lexed[i].tokens;
        }
    }
    lastImportsLine.clear();
    importCHeaders(files, lexed, parsed, symbols, options);
    build.importsLine = lastImportsLine;
    reportParse(build, parseTime);
    runResolve(build);
  }

  // The parse statistics and the parse errors.
  void reportParse(Build& build, double parseTime)
  {
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    uint64_t decls = 0, bodies = 0, nodes = 0, errors = 0, filesWithErrors = 0, arenaBytes = 0, lines = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
      if (parsed[i]->isImported)
        continue; // C headers are not part of the parse statistics
      decls += parsed[i]->declCount;
      bodies += parsed[i]->functionBodies;
      nodes += parsed[i]->nodeCount;
      errors += parsed[i]->diagnostics.size();
      filesWithErrors += !parsed[i]->diagnostics.empty();
      arenaBytes += parsed[i]->arenaBytesReported ? parsed[i]->arenaBytesReported : parsed[i]->arena.bytesAllocated();
      lines += lexed[i].lineCount;
    }
    report("parse:   %.3f s  (%.1f M lines/s)\n", parseTime, lines / 1e6 / parseTime);
    report("  decls: %llu, function bodies: %llu, nodes: %llu, AST memory: %.1f MB\n", (unsigned long long)decls,
           (unsigned long long)bodies, (unsigned long long)nodes, arenaBytes / 1e6);
    report("  parse errors: %llu in %llu files (%.2f%% of files clean)\n", (unsigned long long)errors,
           (unsigned long long)filesWithErrors, 100.0 * (files.size() - filesWithErrors) / std::max<size_t>(1, files.size()));
    // A parse error in the program's files is an error of the build, always shown (whatever --errors
    // says) and on stderr: the parser recovers to see more of the file, but what it recovered to is no
    // program, so nothing is generated from it (runResolve), --keep-going or not.
    build.parseErrors = 0;
    for (size_t i = 0; i < files.size(); ++i)
      if (!parsed[i]->isImported)
        for (const Diagnostic& d : parsed[i]->diagnostics)
        {
          ++build.parseErrors;
          const Token& t = lexed[i].tokens[std::min<size_t>(d.token, lexed[i].tokens.size() - 1)];
          fprintf(stderr, "%s: error: %s (at '%.*s')\n", sourceLocation(files[i].path.string(), lineOf(files[i].text, t.offset), columnOf(files[i].text, t.offset)).c_str(), d.message,
                  int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
        }
    if (options.cTypeWarnings)
      warnCTypeNames(files, lexed, parsed);
    // --convert reads C++, whose macro uses the parser does not expand (the converter expands them
    // in the text it writes): its parse errors are shown, and the converted kpp is what must parse.
    if (build.parseErrors && options.convertDir.empty())
      codegenExit = 1;
    int shown = 0;
    for (size_t i = 0; i < files.size() && shown < options.showErrors; ++i)
      for (const Diagnostic& d : parsed[i]->diagnostics)
      {
        if (!parsed[i]->isImported)
          break; // on stderr, above
        if (!options.showImportErrors)
          break; // reported in one line by importCHeaders
        if (shown++ >= options.showErrors)
          break;
        const Token& t = lexed[i].tokens[std::min<size_t>(d.token, lexed[i].tokens.size() - 1)];
        if (quietOutput) // --quiet hides the statistics, not the program's errors
          fprintf(stderr, "%s: error: %s (at '%.*s')\n", sourceLocation(files[i].path.string(), lineOf(files[i].text, t.offset), columnOf(files[i].text, t.offset)).c_str(), d.message,
                  int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
        else
          report("  %s: error: %s (at '%.*s')\n", sourceLocation(files[i].path.string(), lineOf(files[i].text, t.offset), columnOf(files[i].text, t.offset)).c_str(), d.message,
                 int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
      }
  }

  void runResolve(Build& build)
  {
    using Clock = std::chrono::steady_clock;
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    SymbolTable& symbols = *build.symbols;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    build.global = std::make_unique<GlobalScope>(symbols);
    GlobalScope& global = *build.global;
    global.setLP64(!options.target.windows()); // (long, wchar_t, printf's %ld; docs/design.md, "Targets and hosts")
    global.setConverting(!options.convertDir.empty());
    for (size_t begin = 0; begin < options.whyAt.size();)
    {
      size_t end = std::min(options.whyAt.find(',', begin), options.whyAt.size());
      std::string at = options.whyAt.substr(begin, end - begin);
      if (size_t colon = at.rfind(':'); colon != std::string::npos)
      {
        std::string file = at.substr(0, colon);
        std::replace(file.begin(), file.end(), '/', '\\');
        global.traceLine(file, uint32_t(atoi(at.c_str() + colon + 1)));
      }
      begin = end + 1;
    }
    // What each unit of the resolve read, for a compile server's next, incremental, build
    // (docs/compile-speed.md, step 2); KPP_DEPENDENCY_STATS records them anywhere and reports them.
    // From the index on: an instance checked there (finishIndex) counts in the instances'
    // statistics, and a total made again from what each kept (step 4) must have it.
    GlobalScope::recordDependencies(servingBuild() || getenv("KPP_DEPENDENCY_STATS"));
    auto start = Clock::now();
    parallelFor(parsed.size(), options.threads, [&](size_t i) { global.indexFile(*parsed[i]); });
    double indexTime = seconds(Clock::now() - start);
    start = Clock::now();
    global.finishIndex();
    double finishTime = seconds(Clock::now() - start);
    char indexLine[256];
    snprintf(indexLine, sizeof indexLine, "index:   %.3f s  (+%.3f s for out-of-line class definitions): %zu scopes, %zu entries\n", indexTime,
             finishTime, global.scopeCount(), global.entryCount());
    build.indexLine = indexLine;
    build.indexTime = indexTime;
    build.indexScopes = global.scopeCount();
    build.indexEntries = global.entryCount();
    build.finishTime = finishTime;
    report("%s", indexLine);
    // (kpp's one global scope: not the rule of the C++ a conversion reads, whose names local to their file are)
    if (options.convertDir.empty())
      build.clashes = global.topLevelClashes();
    for (const std::string& clash : build.clashes)
    {
      fprintf(stderr, "%s\n", clash.c_str());
      codegenExit = 1;
    }

    std::vector<ResolveStats>& stats = build.stats;
    stats.assign(parsed.size(), ResolveStats{});
    start = Clock::now();
    for (size_t i = 0; i < stats.size(); ++i)
    {
      stats[i].fileIndex = uint32_t(i);
      stats[i].collectWhy = options.showWhy > 0;
    }
    // Class instances' member bodies as tasks, taken by the workers that run out of files
    // (docs/compile-speed.md, step 3), so that no file drags its instances onto one thread.
    if (options.accessWarnings)
      global.enableAccessWarnings();
    if (options.structRule)
      global.enableStructRule();
    global.resolveAll(parsed.size(), options.threads, [&](size_t i) { global.resolveFile(*parsed[i], lexed[i].tokens, files[i].text, stats[i]); });
    build.resolveTime = seconds(Clock::now() - start);
    // What the reports count is the resolve's: codegen instantiates more afterwards.
    global.resolveDone();
    build.instantiation = global.instantiationStats();
    build.instanceStats = global.instanceStats();
    reportResolve(build);
  }

  // The resolve's statistics, then codegen (or the conversion), and the build is done.
  void reportResolve(Build& build)
  {
    std::vector<SourceFile>& files = build.files;
    std::vector<LexResult>& lexed = build.lexed;
    SymbolTable& symbols = *build.symbols;
    const Options& options = build.options;
    std::vector<std::unique_ptr<ParsedFile>>& parsed = build.parsed;
    GlobalScope& global = *build.global;
    std::vector<ResolveStats>& stats = build.stats;
    double resolveTime = build.resolveTime;
    ResolveStats total;
    for (const ResolveStats& s : stats)
      total.add(s);
    total.add(build.instanceStats); // the instances', each once whoever asked first (at the end of the resolve)
    // KPP_DUMP_FILESTATS=path: each file's statistics and the instances', one line each, for finding
    // where two runs differ (tests/determinism.ps1 compares only the totals).
    if (const char* dumpPath = getenv("KPP_DUMP_FILESTATS"))
      if (FILE* f = fopen(dumpPath, "w"))
      {
        auto line = [&](const std::string& what, const ResolveStats& s)
        {
          fprintf(f, "%s types %llu/%llu/%llu names %llu/%llu/%llu/%llu/%llu exprs %llu/%llu/%llu calls %llu/%llu auto %llu\n", what.c_str(),
                  (unsigned long long)s.typesResolved, (unsigned long long)s.typesDependent, (unsigned long long)s.typesUnresolved,
                  (unsigned long long)s.namesLocal, (unsigned long long)s.namesMember, (unsigned long long)s.namesGlobal,
                  (unsigned long long)s.namesDependent, (unsigned long long)s.namesUnresolved, (unsigned long long)s.exprsTyped,
                  (unsigned long long)s.exprsDependent, (unsigned long long)s.exprsUnknown, (unsigned long long)s.callsResolved,
                  (unsigned long long)s.callsAmbiguousArity, (unsigned long long)s.unknownAuto);
        };
        for (size_t i = 0; i < stats.size(); ++i)
          line(files[i].path.string(), stats[i]);
        line("<instances>", build.instanceStats);
        fclose(f);
      }
    uint64_t types = total.typesResolved + total.typesDependent + total.typesUnresolved;
    uint64_t names = total.namesLocal + total.namesMember + total.namesGlobal + total.namesDependent + total.namesUnresolved;
    uint64_t project = total.typesUnresolved + total.namesUnresolved - total.qualifiedUnresolvedStd - total.unresolvedMacroLike;
    report("resolve: %.3f s\n", resolveTime);
    report("  type names:  %llu  (%.1f%% resolved, %.1f%% dependent on template parameters, %.1f%% unresolved)\n",
           (unsigned long long)types, 100.0 * total.typesResolved / std::max<uint64_t>(1, types),
           100.0 * total.typesDependent / std::max<uint64_t>(1, types), 100.0 * total.typesUnresolved / std::max<uint64_t>(1, types));
    report("  identifiers: %llu  (%.1f%% local, %.1f%% member, %.1f%% namespace/global, %.1f%% dependent, %.1f%% unresolved)\n",
           (unsigned long long)names, 100.0 * total.namesLocal / std::max<uint64_t>(1, names),
           100.0 * total.namesMember / std::max<uint64_t>(1, names), 100.0 * total.namesGlobal / std::max<uint64_t>(1, names),
           100.0 * total.namesDependent / std::max<uint64_t>(1, names), 100.0 * total.namesUnresolved / std::max<uint64_t>(1, names));
    report("  unresolved: %llu std::, %llu macro-like, %llu other; %llu fixed-width integer names typed as builtins\n",
           (unsigned long long)total.qualifiedUnresolvedStd, (unsigned long long)total.unresolvedMacroLike,
           (unsigned long long)project, (unsigned long long)total.typesBuiltinByName);
    uint64_t members = total.membersBound + total.membersDependent + total.membersUnknownBase + total.membersNotFound;
    uint64_t exprs = total.exprsTyped + total.exprsDependent + total.exprsUnknown;
    report("check:   %zu distinct types\n", global.typeCount());
    report("  member accesses: %llu  (%.1f%% bound to a declaration, %.1f%% dependent, %.1f%% object type unknown, %.1f%% no such member)\n",
           (unsigned long long)members, 100.0 * total.membersBound / std::max<uint64_t>(1, members),
           100.0 * total.membersDependent / std::max<uint64_t>(1, members), 100.0 * total.membersUnknownBase / std::max<uint64_t>(1, members),
           100.0 * total.membersNotFound / std::max<uint64_t>(1, members));
    report("  expressions: %llu  (%.1f%% typed, %.1f%% dependent on template parameters, %.1f%% unknown)\n",
           (unsigned long long)exprs, 100.0 * total.exprsTyped / std::max<uint64_t>(1, exprs),
           100.0 * total.exprsDependent / std::max<uint64_t>(1, exprs), 100.0 * total.exprsUnknown / std::max<uint64_t>(1, exprs));
    report("  unknown because: name %llu, object type %llu, member %llu, overload %llu, deduction %llu, auto return %llu, "
           "auto %llu, operator %llu, unsupported construct %llu\n",
           (unsigned long long)total.unknownName, (unsigned long long)total.unknownBase, (unsigned long long)total.unknownMember,
           (unsigned long long)total.unknownOverload, (unsigned long long)total.unknownDeduction, (unsigned long long)total.unknownAutoReturn,
           (unsigned long long)total.unknownAuto, (unsigned long long)total.unknownOperator, (unsigned long long)total.unknownUnsupported);
    report("  calls resolved to a function: %llu (%llu still ambiguous after conversion ranking)\n", (unsigned long long)total.callsResolved,
           (unsigned long long)total.callsAmbiguousArity);
    GlobalScope::InstantiationStats inst = build.instantiation;
    uint64_t instExprs = total.instExprsTyped + total.instExprsDependent + total.instExprsUnknown;
    report("instantiate: %zu class and %zu function template instantiations checked once each (%zu requests, %zu waits), %.3f s of CPU\n",
           inst.classes, inst.functions, inst.requests, inst.waits, inst.seconds);
    report("  %zu member bodies checked inside instantiations; %llu expressions there (%.1f%% typed, %.1f%% dependent, %.1f%% unknown)\n",
           inst.bodies, (unsigned long long)instExprs, 100.0 * total.instExprsTyped / std::max<uint64_t>(1, instExprs),
           100.0 * total.instExprsDependent / std::max<uint64_t>(1, instExprs), 100.0 * total.instExprsUnknown / std::max<uint64_t>(1, instExprs));
    if (getenv("KPP_DEPENDENCY_STATS")) // not in the normal report: a server's build prints what a new process's does
    {
      GlobalScope::DependencyStats deps = global.dependencyStats();
      report("dependencies: %zu walks, %zu declared types, %zu instances, %zu class instances, %zu scopes' relations; %zu names and %zu uses recorded\n",
             deps.walks, deps.declaredTypes, deps.instances, deps.classInstances, deps.scopes, deps.names, deps.uses);
    }
    std::vector<std::pair<uint32_t, uint32_t>> missing(total.missingMembers.begin(), total.missingMembers.end());
    std::sort(missing.begin(), missing.end(), [&](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : symbols.name(a.first) < symbols.name(b.first); }); // ties by name: symbol ids depend on the threads
    if (!missing.empty() && options.showUnresolved > 0)
      report("  most common members not found in a known class:\n");
    for (size_t i = 0; i < missing.size() && int(i) < options.showUnresolved / 2; ++i)
    {
      ResolveStats::Where w = total.missingMembersFirst[missing[i].first];
      const Token& t = lexed[w.file].tokens[std::min<size_t>(w.token, lexed[w.file].tokens.size() - 1)];
      report("    %6u  %-28.*s first at %s:%u\n", missing[i].second, int(symbols.name(missing[i].first).size()),
             symbols.name(missing[i].first).data(), files[w.file].path.string().c_str(), lineOf(files[w.file].text, t.offset));
    }
    std::vector<std::pair<uint32_t, uint32_t>> unknownBase(total.unknownBaseMembers.begin(), total.unknownBaseMembers.end());
    std::sort(unknownBase.begin(), unknownBase.end(), [&](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : symbols.name(a.first) < symbols.name(b.first); }); // ties by name: symbol ids depend on the threads
    if (!unknownBase.empty() && options.showUnresolved > 0)
      report("  most common members accessed on an object of unknown type:\n");
    for (size_t i = 0; i < unknownBase.size() && int(i) < options.showUnresolved / 2; ++i)
      report("    %6u  %.*s\n", unknownBase[i].second, int(symbols.name(unknownBase[i].first).size()), symbols.name(unknownBase[i].first).data());
    if (options.showWhy > 0)
      report("%s", whyReport(total, symbols, options.showWhy).c_str());
    if (options.showUnresolved > 0)
      report("  most common unresolved names:\n");
    std::vector<std::pair<uint32_t, uint32_t>> top(total.unresolvedSymbols.begin(), total.unresolvedSymbols.end());
    std::sort(top.begin(), top.end(), [&](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : symbols.name(a.first) < symbols.name(b.first); }); // ties by name: symbol ids depend on the threads
    // (no code generated: no function counts as emitted, as in a build from scratch, whatever a kept
    // program's last code generator emitted)
    if (build.parseErrors && generatesCode(options))
    {
      fprintf(stderr, "error: %zu parse error(s): no code generated\n", build.parseErrors);
      global.setCodeGenerated(false);
      reportCheckerErrors(global);
      build.codegenBehind = true;
    }
    else if (!build.clashes.empty() && generatesCode(options))
    {
      // (a name two entities claim is looked up as one of them: what codegen made of it would only
      // add errors that follow from the clash)
      fprintf(stderr, "error: %zu top-level name(s) declared twice: no code generated\n", build.clashes.size());
      global.setCodeGenerated(false);
      reportCheckerErrors(global);
      build.codegenBehind = true;
    }
    else if (generatesCode(options))
      runCodegen(build);
    else if (options.convertDir.empty())
    {
      reportCheckerErrors(global);
      reportAccessWarnings(build);
      if (!options.runBuild.empty() && !codegenExit)
      {
        // `--run-build debug,release`: build.kpp's description of each configuration, computed by
        // the interpreter, on stdout one after the other.
        for (size_t start = 0; start <= options.runBuild.size() && !codegenExit;)
        {
          size_t comma = std::min(options.runBuild.find(',', start), options.runBuild.size());
          std::string error;
          if (std::optional<std::string> text = global.runBuild(std::string_view(options.runBuild).substr(start, comma - start), error))
            fwrite(text->data(), 1, text->size(), stdout);
          else
          {
            fprintf(stderr, "error: %s\n", error.c_str());
            codegenExit = 1;
          }
          start = comma + 1;
        }
      }
    }
    if (!options.convertDir.empty())
    {
      std::vector<ConvertInput> inputs;
      for (size_t i = 0; i < parsed.size(); ++i)
        inputs.push_back({files[i].path, files[i].text, &lexed[i].tokens, parsed[i].get(), options.cSources && isCSource(files[i].path)});
      std::vector<std::filesystem::path> roots(options.roots.begin(), options.roots.end());
      std::string summary =
        convertToKpp(inputs, global, symbols, roots, options.convertDir, options.overridesDir, options.configuration, options.conversionLibrary,
                     options.convertBase);
      report("convert: %s\n%s", options.convertDir.c_str(), summary.c_str());
      if (convertErrorCount())
        codegenExit = 1;
      reportAccessWarnings(build); // over the C++ the conversion read, its friends still declared
    }
    for (size_t i = 0; i < top.size() && int(i) < options.showUnresolved; ++i)
    {
      ResolveStats::Where w = total.unresolvedFirst[top[i].first];
      const Token& t = lexed[w.file].tokens[std::min<size_t>(w.token, lexed[w.file].tokens.size() - 1)];
      report("    %6u  %-28.*s first at %s:%u\n", top[i].second, int(symbols.name(top[i].first).size()),
             symbols.name(top[i].first).data(), files[w.file].path.string().c_str(), lineOf(files[w.file].text, t.offset));
    }
    // Done: end the process without freeing what it built. The destructors of the syntax trees,
    // the types, the instances and the IR text cost about 5 s on the full game (docs/compile-speed.md,
    // "End to end"), and the operating system takes the memory back at once. Everything the run
    // writes is closed by now. A compile server answers its client here and frees afterwards.
    if (!options.dumpTokens)
      buildDone(codegenExit);
  }

  double seconds(std::chrono::steady_clock::duration d)
  {
    return std::chrono::duration<double>(d).count();
  }

  void reportLex(size_t fileCount, const FileStats& total, size_t symbolCount, unsigned threads, double collectTime, double lexTime)
  {
    report("files:   %zu\n", fileCount);
    report("bytes:   %llu (%.1f MB)\n", (unsigned long long)total.bytes, total.bytes / 1e6);
    report("lines:   %llu\n", (unsigned long long)total.lines);
    report("tokens:  %llu\n", (unsigned long long)total.tokens);
    report("symbols: %zu distinct identifiers\n", symbolCount);
    report("errors:  %llu\n", (unsigned long long)total.errors);
    report("threads: %u\n", threads);
    report("read:    %.3f s\n", collectTime);
    report("lex:     %.3f s  (%.0f MB/s, %.1f M lines/s, %.1f M tokens/s)\n", lexTime, total.bytes / 1e6 / lexTime,
           total.lines / 1e6 / lexTime, total.tokens / 1e6 / lexTime);
  }

  // ---- Step 3b: a request whose files changed only inside function bodies (docs/compile-speed.md) ----


  // A token's identity for comparing two versions of a file: its kind, and its symbol or spelling.
  bool sameToken(const Token& a, std::string_view ta, const Token& b, std::string_view tb)
  {
    if (a.kind != b.kind)
      return false;
    switch (a.getKind())
    {
      case TokenKind::Identifier: case TokenKind::Keyword: case TokenKind::Punctuator: case TokenKind::End:
        return a.id == b.id;
      default:
        return ta.substr(a.offset, a.length) == tb.substr(b.offset, b.length);
    }
  }

  // The bodies of the functions decls declare (members of namespaces, classes and enums included).
  // False when one cannot be told apart (a body that is no `{...}`).
  bool collectBodies(Span<Decl*> decls, const std::vector<Token>& tokens, bool inTemplate, std::vector<Body>& out)
  {
    for (Decl* d : decls)
    {
      if (!d)
        continue;
      bool templated = inTemplate || d->isTemplate || !d->templateParams.empty();
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
          if (!collectBodies(static_cast<NamespaceDecl*>(d)->decls, tokens, inTemplate, out))
            return false;
          break;
        case NodeKind::ClassDecl:
          if (!collectBodies(static_cast<ClassDecl*>(d)->members, tokens, templated, out))
            return false;
          break;
        case NodeKind::EnumDecl:
          if (!collectBodies(static_cast<EnumDecl*>(d)->members, tokens, templated, out))
            return false;
          break;
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (!f->body)
            break;
          uint32_t open = f->body->token;
          if (open >= tokens.size() || !tokens[open].is(P::LBrace))
            return false;
          uint32_t close = open;
          for (int depth = 0; close < tokens.size(); ++close)
            if (tokens[close].is(P::LBrace))
              ++depth;
            else if (tokens[close].is(P::RBrace) && --depth == 0)
              break;
          if (close >= tokens.size())
            return false;
          bool eligible = true;
          for (uint32_t k = f->token; eligible && k < open; ++k)
            if (tokens[k].is(Kw::auto_) || tokens[k].is(Kw::decltype_))
              eligible = false; // a deduced return type: callers elsewhere are typed by the body
          out.push_back({f, open, close, eligible, templated, bool(f->flags & (DF_Constexpr | DF_Consteval))});
          break;
        }
        default:
          break;
      }
    }
    return true;
  }

  // One changed file of the kept build, checked to have changed inside eligible function bodies
  // only: its new text and tokens, and which bodies changed (indexes into the paired body lists).
  struct ChangedFile
  {
    size_t index = 0;
    std::string text;
    LexResult lexed;              // the new text's, as the lexer gave them
    std::vector<Token> tokens;    // as the parser left them (filtered, `>>` split)
    std::unique_ptr<ParsedFile> parsed;
    std::vector<Body> oldBodies, newBodies;
    std::vector<size_t> changed;
  };

  // Whether a file's parse errors are all inside function bodies (bodies, sorted by their opening
  // brace), each of which the parser ended at its closing brace: the parser recovers inside a block
  // up to its end, so the declarations around such a body are what the tokens around it say, as in
  // a file without the error. A body that does not parse (a line half typed) is grafted with what
  // the parser recovered: its errors are the build's (no code is generated), the rest of the file
  // stays the kept tree.
  bool errorsInBodies(const ParsedFile& parsed, const std::vector<Body>& bodies)
  {
    for (const Diagnostic& d : parsed.diagnostics)
    {
      auto after = std::upper_bound(bodies.begin(), bodies.end(), d.token, [](uint32_t t, const Body& b) { return t < b.open; });
      if (after == bodies.begin())
        return false;
      const Body& b = *std::prev(after);
      if (d.token > b.close || b.f->endToken != b.close + 1)
        return false;
    }
    return true;
  }

  bool bodiesOnly(Build& kept, size_t i, std::string text, ChangedFile& out, std::string& why)
  {
    const ParsedFile& old = *kept.parsed[i];
    const std::vector<Token>& oldTokens = kept.lexed[i].tokens;
    std::string_view oldText = kept.files[i].text;
    if (kept.files[i].path.extension() != ".kpp" || kept.lexed[i].errorCount)
      return why = "not a .kpp file, or lexical errors", false;
    out.index = i;
    out.text = std::move(text);
    out.lexed = lex(out.text, *kept.symbols);
    out.tokens = out.lexed.tokens;
    out.parsed = std::make_unique<ParsedFile>();
    out.parsed->path = old.path;
    out.parsed->index = old.index;
    out.parsed->isLibrary = old.isLibrary;
    out.parsed->library = old.library;
    out.parsed->libraryScope = old.libraryScope;
    parseFile(out.tokens, out.text, *kept.symbols, *out.parsed, false, &kept.options.configValues);
    if (out.lexed.errorCount)
      return why = "the new text has lexical errors", false;
    if (!collectBodies(old.decls, oldTokens, false, out.oldBodies))
      return why = "the function bodies cannot be paired", false;
    if (!collectBodies(out.parsed->decls, out.tokens, false, out.newBodies) || out.oldBodies.size() != out.newBodies.size())
      return why = "the function bodies cannot be paired", false;
    auto byOpen = [](const Body& a, const Body& b) { return a.open < b.open; };
    std::sort(out.oldBodies.begin(), out.oldBodies.end(), byOpen);
    std::sort(out.newBodies.begin(), out.newBodies.end(), byOpen);
    if (!errorsInBodies(old, out.oldBodies))
      return why = "parse errors outside function bodies", false;
    if (!errorsInBodies(*out.parsed, out.newBodies))
      return why = "the new text has parse errors outside function bodies", false;
    // Outside the bodies the token streams must be the same; a body that differs is a change.
    size_t a = 0, b = 0;
    for (size_t k = 0; k <= out.oldBodies.size(); ++k)
    {
      size_t aEnd = k < out.oldBodies.size() ? out.oldBodies[k].open : oldTokens.size();
      size_t bEnd = k < out.newBodies.size() ? out.newBodies[k].open : out.tokens.size();
      if (aEnd < a || bEnd < b || aEnd - a != bEnd - b)
        return why = "a change outside function bodies", false;
      for (; a < aEnd; ++a, ++b)
        if (!sameToken(oldTokens[a], oldText, out.tokens[b], out.text))
          return why = "a change outside function bodies", false;
      if (k == out.oldBodies.size())
        break;
      const Body& ob = out.oldBodies[k];
      const Body& nb = out.newBodies[k];
      bool same = ob.close - ob.open == nb.close - nb.open;
      for (uint32_t x = 0; same && x <= ob.close - ob.open; ++x)
        same = sameToken(oldTokens[ob.open + x], oldText, out.tokens[nb.open + x], out.text);
      if (!same)
      {
        if (!ob.eligible || !nb.eligible)
          return why = "a changed body with a deduced return type", false;
        // A class or enumeration declared in a body gets its scope when the file is indexed: one in
        // a new body would have none (a lambda's closure is no such declaration).
        bool localType = false;
        auto findLocalTypes = [&](Node* n)
        {
          if ((n->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n)->lambda) || n->kind == NodeKind::EnumDecl)
            localType = true;
        };
        forEachNode(nb.f->body, findLocalTypes);
        forEachNode(ob.f->body, findLocalTypes); // (and the old body's would stay in the index)
        if (localType)
          return why = "a changed body declares a class or an enumeration", false;
        out.changed.push_back(k);
      }
      a = ob.close + 1;
      b = nb.close + 1;
    }
    return true;
  }

  // Where each of a file's old tokens is in its new tokens (UINT32_MAX: gone), by a diff of the two
  // token streams (Myers', on the stretch between the common prefix and suffix). False when the
  // versions differ too much for a diff worth keeping.
  bool tokenMap(const std::vector<Token>& a, std::string_view ta, const std::vector<Token>& b, std::string_view tb, std::vector<uint32_t>& map)
  {
    size_t n = a.size(), m = b.size();
    map.assign(n, UINT32_MAX);
    size_t prefix = 0;
    while (prefix < n && prefix < m && sameToken(a[prefix], ta, b[prefix], tb))
      map[prefix] = uint32_t(prefix), ++prefix;
    size_t suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix && sameToken(a[n - 1 - suffix], ta, b[m - 1 - suffix], tb))
      map[n - 1 - suffix] = uint32_t(m - 1 - suffix), ++suffix;
    int64_t N = int64_t(n - prefix - suffix), M = int64_t(m - prefix - suffix);
    if (N == 0 || M == 0)
      return true;
    auto same = [&](int64_t x, int64_t y) { return sameToken(a[prefix + x], ta, b[prefix + y], tb); };
    const int64_t maxD = std::min<int64_t>(N + M, 4000);
    int64_t offset = maxD + 1;
    std::vector<int64_t> v(size_t(2 * offset + 1), 0);
    std::vector<std::vector<int64_t>> trace;
    int64_t found = -1;
    for (int64_t d = 0; d <= maxD && found < 0; ++d)
    {
      if (int64_t(trace.size()) * (2 * offset + 1) > 64'000'000)
        return false;
      trace.push_back(v);
      for (int64_t k = -d; k <= d; k += 2)
      {
        int64_t x = (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])) ? v[offset + k + 1] : v[offset + k - 1] + 1;
        int64_t y = x - k;
        while (x < N && y < M && same(x, y))
          ++x, ++y;
        v[offset + k] = x;
        if (x >= N && y >= M)
        {
          found = d;
          break;
        }
      }
    }
    if (found < 0)
      return false;
    // Back through the trace: the diagonal moves are the tokens the two versions share.
    int64_t x = N, y = M;
    for (int64_t d = found; d > 0; --d)
    {
      const std::vector<int64_t>& pv = trace[size_t(d)];
      int64_t k = x - y;
      int64_t prevK = (k == -d || (k != d && pv[offset + k - 1] < pv[offset + k + 1])) ? k + 1 : k - 1;
      int64_t prevX = pv[offset + prevK], prevY = prevX - prevK;
      while (x > prevX && y > prevY)
      {
        --x, --y;
        map[prefix + size_t(x)] = uint32_t(prefix + size_t(y));
      }
      x = prevX, y = prevY;
    }
    while (x > 0 && y > 0)
    {
      --x, --y;
      map[prefix + size_t(x)] = uint32_t(prefix + size_t(y));
    }
    return true;
  }

  // A function a file declares outside any body, with the declaration it is a member of (null at
  // the file's top level) and whether a template encloses it.
  struct DeclaredFunction
  {
    FunctionDecl* f;
    Decl* container;
    bool templated;
  };

  // A class's data member (step 4's layout edits), with its class and whether a template encloses it.
  struct DeclaredMember
  {
    VariableDecl* v;
    ClassDecl* container;
    bool templated;
  };

  void collectFunctions(Span<Decl*> decls, Decl* container, bool inTemplate, std::vector<DeclaredFunction>& out,
                        std::vector<Decl*>& containers, std::vector<DeclaredMember>* members = nullptr)
  {
    for (Decl* d : decls)
    {
      if (!d)
        continue;
      bool templated = inTemplate || d->isTemplate || !d->templateParams.empty();
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
          containers.push_back(d);
          collectFunctions(static_cast<NamespaceDecl*>(d)->decls, d, inTemplate, out, containers, members);
          break;
        case NodeKind::ClassDecl:
          containers.push_back(d);
          collectFunctions(static_cast<ClassDecl*>(d)->members, d, templated, out, containers, members);
          break;
        case NodeKind::EnumDecl:
          containers.push_back(d);
          collectFunctions(static_cast<EnumDecl*>(d)->members, d, templated, out, containers, members);
          break;
        case NodeKind::FunctionDecl:
          out.push_back({static_cast<FunctionDecl*>(d), container, templated});
          break;
        case NodeKind::VariableDecl:
          if (members && container && container->kind == NodeKind::ClassDecl)
            members->push_back({static_cast<VariableDecl*>(d), static_cast<ClassDecl*>(container), inTemplate});
          break;
        default:
          break;
      }
    }
  }

  // Points every node of a kept file at its new tokens: its declarations' nodes, and the copies
  // instances made of them (they index the file's tokens too); skip: nodes dropped with the old
  // version, staleSources: the declarations whose instances are copied anew (their old copies
  // are dropped). False when a node's token has no place in the new version.
  bool remapKeptFile(Build& kept, ParsedFile& file, const std::vector<uint32_t>& map, const std::unordered_set<Node*>& skip,
                     const std::unordered_set<const Decl*>& staleSources, const ParsedFile& fresh)
  {
    // Names the code gives by position: a function's local types (`f::Local@2045`; its lambdas'
    // closures by the parser's numbers in the file's order alone, `f::__lambda_3_1`), its static
    // locals, catch filters and static initializer lists (by their tokens). A build from scratch of
    // the new text names them by their new places, so a kept function holding one that moves is
    // emitted again (in full, for a local type: codegen's session keeps the types' names; and for a
    // template's, whose instances it cannot emit again alone), and a kept closure takes the name the
    // new parse gives the lambda at its new place.
    std::unordered_map<uint32_t, ClassDecl*> freshClosures; // new token -> the new parse's closure there
    forEachNode(fresh.decls, [&](Node* n)
    {
      if (n->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(n)->lambda)
        freshClosures[n->token] = static_cast<ClassDecl*>(n);
    });
    auto moved = [&](uint32_t t) { return t >= map.size() || map[t] != t; };
    // A kept closure takes the name the new parse gives the lambda at its new place: true when that
    // name is another.
    auto renameClosure = [&](ClassDecl* c)
    {
      if (!c->lambda || c->name.empty() || c->token >= map.size() || map[c->token] == UINT32_MAX)
        return false;
      auto it = freshClosures.find(map[c->token]);
      if (it == freshClosures.end() || it->second->name.empty() || it->second->name.last().symbol == c->name.last().symbol)
        return false;
      const_cast<NamePart&>(c->name.parts[c->name.parts.size() - 1]).symbol = it->second->name.last().symbol;
      return true;
    };
    std::vector<DeclaredFunction> functions;
    std::vector<Decl*> containers;
    collectFunctions(file.decls, nullptr, false, functions, containers);
    for (const DeclaredFunction& d : functions)
    {
      if (!d.f->body || skip.count(d.f->body))
        continue;
      bool localType = false, positional = false;
      forEachNode(d.f->body, [&](Node* n)
      {
        if (skip.count(n))
          return;
        if (n->kind == NodeKind::ClassDecl || n->kind == NodeKind::EnumDecl)
        {
          // (A lambda's closure is named by its file's number, not its place: renamed only.)
          bool renamed = n->kind == NodeKind::ClassDecl && renameClosure(static_cast<ClassDecl*>(n));
          bool closure = n->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(n)->lambda;
          localType = localType || renamed || (!closure && moved(n->token));
        }
        else if ((n->kind == NodeKind::VariableDecl && (static_cast<Decl*>(n)->flags & DF_Static)) || n->kind == NodeKind::TryStmt ||
                 n->kind == NodeKind::BraceInitExpr)
          positional = positional || moved(n->token);
      });
      if (localType || (positional && d.templated))
        kept.positionalCodegenInFull = true;
      else if (positional)
        kept.changedBodies.push_back(d.f);
    }
    // The closures outside function bodies, named alike (a global's initializer `{ [](int v) {...},
    // ... }`, a default member initializer, a default argument): the instances made from them carry
    // their names (`FunctionImpl<int32, int32>::call<__lambda_3_2>`), so a renamed one is emitted in
    // full too. (Only renamed: outside a function their names carry no token. The ones in bodies
    // again: renamed already.)
    forEachNode(file.decls, [&](Node* n)
    {
      if (!skip.count(n) && n->kind == NodeKind::ClassDecl && renameClosure(static_cast<ClassDecl*>(n)))
        kept.positionalCodegenInFull = true;
    });
    bool lost = false;
    std::unordered_set<Node*> done;
    auto remap = [&](Node* n)
    {
      if (skip.count(n) || !done.insert(n).second)
        return;
      remapNodeTokens(n, [&](uint32_t t)
      {
        if (t < map.size() && map[t] != UINT32_MAX)
          return map[t];
        lost = true;
        return t;
      });
    };
    forEachNode(file.decls, remap);
    kept.global->forEachInstanceCopy(file, [&](Node* copy, const Decl* source)
    {
      if (!staleSources.count(source))
        forEachNode(copy, remap);
    });
    return !lost;
  }

  // Moves the new bodies into the kept declarations. The file takes its new tokens and text: every
  // node it keeps is pointed at the new tokens (through a diff of the two versions), and each
  // changed function takes its new body from the new parse, whose arena the kept build keeps. The
  // declaration objects, which the rest of the program points at, stay. False (the kept build is
  // then unusable) when a new body needs what only indexing the whole file gives (a `(name) op x`
  // cast to rewrite) or a kept node's token has no place in the new version.
  bool graftBodies(Build& kept, ChangedFile& c, std::string& why)
  {
    size_t i = c.index;
    ParsedFile& file = *kept.parsed[i];
    std::vector<uint32_t> map;
    if (!tokenMap(kept.lexed[i].tokens, kept.files[i].text, c.tokens, c.text, map))
      return why = "the two versions differ too much to pair their tokens", false;
    std::unordered_set<Node*> replaced; // the old bodies' nodes: dropped, not pointed anywhere
    std::unordered_set<const Decl*> stale; // templates whose instances are copied anew
    for (size_t k : c.changed)
    {
      forEachNode(c.oldBodies[k].f->body, [&](Node* n) { replaced.insert(n); });
      stale.insert(c.oldBodies[k].f);
    }
    if (!remapKeptFile(kept, file, map, replaced, stale, *c.parsed))
      return why = "a kept node's token has no place in the new version", false;
    for (auto& [b, where] : c.parsed->castCandidates)
      if (where && where->kind == NodeKind::FunctionDecl)
        for (size_t k : c.changed)
          if (where == c.newBodies[k].f)
            return why = "a new body has a cast to rewrite", false;
    // The new bodies, their declarations (locals, lambdas' closures) now the kept file's and their
    // parent the kept function where it was the new parse's.
    for (size_t k : c.changed)
    {
      FunctionDecl* keptF = c.oldBodies[k].f;
      FunctionDecl* newF = c.newBodies[k].f;
      keptF->body = newF->body;
      forEachNode(keptF->body, [&](Node* n)
      {
        if (n->kind > NodeKind::PackType && n->kind <= NodeKind::ImportDecl)
        {
          auto* d = static_cast<Decl*>(n);
          d->file = &file;
          if (d->parent == newF)
            d->parent = keptF;
        }
        else if (n->kind == NodeKind::LiteralExpr)
          static_cast<LiteralExpr*>(n)->file = &file; // its spelling is read from its file's tokens
      });
    }
    kept.files[i].text = std::move(c.text);
    kept.lexed[i].tokens = std::move(c.tokens);
    kept.lexed[i].lineCount = c.lexed.lineCount;
    kept.lexed[i].errorCount = c.lexed.errorCount;
    file.text = kept.files[i].text;
    file.tokens = &kept.lexed[i].tokens;
    file.diagnostics = c.parsed->diagnostics; // (inside bodies: errorsInBodies)
    file.declCount = c.parsed->declCount;
    file.functionBodies = c.parsed->functionBodies;
    file.nodeCount = c.parsed->nodeCount;
    file.arenaBytesReported = c.parsed->arena.bytesAllocated();
    kept.grafts.push_back(std::move(c.parsed)); // its arena holds the new bodies
    // The kept instance statistics' places in this file (an instance of a template it declares)
    // move with its tokens (and each instance walk's own, from which the total may be made again);
    // one in a replaced body cannot be told, and the build is from scratch.
    bool lost = false;
    auto moved = [&](uint32_t& token)
    {
      if (token < map.size() && map[token] != UINT32_MAX)
        token = map[token];
      else
        lost = true;
    };
    for (auto& [symbol, where] : kept.instanceStats.unresolvedFirst)
      if (where.file == i)
        moved(where.token);
    for (auto& [symbol, where] : kept.instanceStats.missingMembersFirst)
      if (where.file == i)
        moved(where.token);
    for (auto& [key, site] : kept.instanceStats.why)
      if (site.file == &file)
        moved(site.token);
    kept.global->forEachInstanceStatsPlace(uint32_t(i), &file, moved);
    if (lost)
      return why = "an instance's statistic names a place in a replaced body", false;
    return true;
  }

  // ---- Step 4, first kind: functions added, removed or changed (docs/compile-speed.md) ----

  // A file whose changes are whole functions (added, removed, or changed and so both): which, and
  // where each added one goes among the kept declarations.
  struct FunctionChange
  {
    size_t index = 0;
    std::string text;
    LexResult lexed;
    std::vector<Token> tokens;
    std::unique_ptr<ParsedFile> parsed;
    std::vector<uint32_t> map;                          // the kept tokens' places in the new ones
    std::vector<std::pair<FunctionDecl*, Decl*>> removed; // kept functions, with the kept declaration they are members of
    std::vector<std::pair<FunctionDecl*, Decl*>> added; // new functions, with the kept declaration they belong to
    std::vector<std::pair<VariableDecl*, ClassDecl*>> removedMembers; // kept data members, with their kept class
    std::vector<std::pair<VariableDecl*, ClassDecl*>> addedMembers;   // new data members, with the kept class they go into
  };

  // One past a data member's last token, its `;` included (the parser ends the declaration before it).
  uint32_t memberEnd(const VariableDecl* v, const std::vector<Token>& tokens)
  {
    uint32_t e = v->endToken;
    if (e < tokens.size() && tokens[e].is(P::Semi))
      ++e;
    return e;
  }

  // Whether a data member can come, go or change with what reads it found (its name's readers, its
  // class's member list's, its declared type's): one of a plain class's (no template, no union, no
  // typed union, no lambda's closure), not static (a global), no alias or friend. (A declaration of
  // several declarators comes or goes whole: functionsOnly pairs them a declaration at a time.)
  bool plainDataMember(const DeclaredMember& d)
  {
    VariableDecl* v = d.v;
    ClassDecl* c = d.container;
    return !(d.templated || c->isTemplate || !c->templateParams.empty() || c->keyword == Kw::union_ || c->isTypedUnion || c->lambda ||
             !c->isDefinition || c->name.empty() || (v->flags & (DF_Static | DF_Typedef | DF_Friend)) || v->name.parts.size() != 1 ||
             !v->structuredBindings.empty());
  }

  // Whether a function can come or go without anything but its name's readers seeing it: no
  // template, no virtual or special member (layouts, vtables and traits read those without a
  // lookup), not constexpr, no deduced return type, no class or enumeration in its body.
  bool plainFunction(const DeclaredFunction& d, const std::vector<Token>& tokens)
  {
    FunctionDecl* f = d.f;
    const uint32_t excluded = DF_Virtual | DF_Override | DF_Pure | DF_Constructor | DF_Destructor | DF_Conversion | DF_Friend | DF_Constexpr |
                              DF_Consteval | DF_Specialization | DF_ExternC;
    if (d.templated || f->isTemplate || !f->templateParams.empty() || (f->flags & excluded) || f->name.empty() || f->name.last().special ||
        !f->objectArgs.empty())
      return false;
    uint32_t end = f->body ? f->body->token : f->endToken;
    for (uint32_t k = f->token; k < end && k < tokens.size(); ++k)
      if (tokens[k].is(Kw::auto_) || tokens[k].is(Kw::decltype_))
        return false;
    bool localType = false;
    forEachNode(f->body, [&](Node* n)
    {
      if ((n->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n)->lambda) || n->kind == NodeKind::EnumDecl)
        localType = true;
    });
    return !localType;
  }

  bool functionsOnly(Build& kept, size_t i, std::string text, FunctionChange& out, std::string& why)
  {
    const ParsedFile& old = *kept.parsed[i];
    const std::vector<Token>& oldTokens = kept.lexed[i].tokens;
    std::string_view oldText = kept.files[i].text;
    if (!old.diagnostics.empty() || kept.files[i].path.extension() != ".kpp")
      return why = "parse errors, or not a .kpp file", false;
    out.index = i;
    out.text = std::move(text);
    out.lexed = lex(out.text, *kept.symbols);
    out.tokens = out.lexed.tokens;
    out.parsed = std::make_unique<ParsedFile>();
    out.parsed->path = old.path;
    out.parsed->index = old.index;
    out.parsed->isLibrary = old.isLibrary;
    out.parsed->library = old.library;
    out.parsed->libraryScope = old.libraryScope;
    parseFile(out.tokens, out.text, *kept.symbols, *out.parsed, false, &kept.options.configValues);
    if (!out.parsed->diagnostics.empty() || out.lexed.errorCount)
      return why = "the new text has errors", false;
    if (!tokenMap(oldTokens, oldText, out.tokens, out.text, out.map))
      return why = "the two versions differ too much to pair their tokens", false;
    std::vector<DeclaredFunction> oldFunctions, newFunctions;
    std::vector<Decl*> oldContainers, newContainers;
    std::vector<DeclaredMember> oldMembers, newMembers;
    collectFunctions(old.decls, nullptr, false, oldFunctions, oldContainers, &oldMembers);
    collectFunctions(out.parsed->decls, nullptr, false, newFunctions, newContainers, &newMembers);
    // A kept function whose tokens all have places, side by side, at a new function's: the same.
    std::unordered_map<uint32_t, const DeclaredFunction*> newByToken;
    for (const DeclaredFunction& d : newFunctions)
      newByToken[d.f->token] = &d;
    std::unordered_set<const FunctionDecl*> pairedNew;
    std::vector<bool> oldCovered(oldTokens.size(), false), newCovered(out.tokens.size(), false);
    std::vector<const DeclaredFunction*> removed;
    for (const DeclaredFunction& d : oldFunctions)
    {
      uint32_t s = d.f->token, e = d.f->endToken;
      bool same = e > s && e <= out.map.size() && out.map[s] != UINT32_MAX;
      for (uint32_t k = s; same && k < e; ++k)
        same = out.map[k] == out.map[s] + (k - s);
      auto it = same ? newByToken.find(out.map[s]) : newByToken.end();
      if (it != newByToken.end() && it->second->f->endToken == out.map[s] + (e - s))
      {
        pairedNew.insert(it->second->f);
        continue;
      }
      removed.push_back(&d);
      for (uint32_t k = s; k < e && k < oldCovered.size(); ++k)
        oldCovered[k] = true;
    }
    std::vector<const DeclaredFunction*> added;
    for (const DeclaredFunction& d : newFunctions)
      if (!pairedNew.count(d.f))
      {
        added.push_back(&d);
        for (uint32_t k = d.f->token; k < d.f->endToken && k < newCovered.size(); ++k)
          newCovered[k] = true;
      }
    // Data members likewise (a class's layout edit), a declaration at a time: its declarators all
    // start at its first token and share its type's nodes (`MapPosition start, goal;`), so they are
    // kept, removed or added together. A kept declaration whose tokens all have places side by side
    // at a new one's is the same, the others removed, the new ones not paired added.
    struct MemberDeclaration
    {
      std::vector<const DeclaredMember*> declarators;
      uint32_t end = 0; // one past its `;`
    };
    auto declarationsOf = [](const std::vector<DeclaredMember>& members, const std::vector<Token>& tokens)
    {
      std::map<uint32_t, MemberDeclaration> byToken;
      for (const DeclaredMember& d : members)
      {
        MemberDeclaration& m = byToken[d.v->token];
        m.declarators.push_back(&d);
        m.end = std::max(m.end, memberEnd(d.v, tokens));
      }
      return byToken;
    };
    std::map<uint32_t, MemberDeclaration> oldDeclarations = declarationsOf(oldMembers, oldTokens), newDeclarations = declarationsOf(newMembers, out.tokens);
    std::unordered_set<uint32_t> pairedNewDeclarations;
    std::vector<const DeclaredMember*> removedMembers, addedMembers;
    for (auto& [s, m] : oldDeclarations)
    {
      uint32_t e = m.end;
      bool same = e > s && e <= out.map.size() && out.map[s] != UINT32_MAX;
      for (uint32_t k = s; same && k < e; ++k)
        same = out.map[k] == out.map[s] + (k - s);
      auto it = same ? newDeclarations.find(out.map[s]) : newDeclarations.end();
      if (it != newDeclarations.end() && it->second.end == out.map[s] + (e - s) && it->second.declarators.size() == m.declarators.size())
      {
        pairedNewDeclarations.insert(it->first);
        continue;
      }
      removedMembers.insert(removedMembers.end(), m.declarators.begin(), m.declarators.end());
      for (uint32_t k = s; k < e && k < oldCovered.size(); ++k)
        oldCovered[k] = true;
    }
    for (auto& [s, m] : newDeclarations)
      if (!pairedNewDeclarations.count(s))
      {
        addedMembers.insert(addedMembers.end(), m.declarators.begin(), m.declarators.end());
        for (uint32_t k = s; k < m.end && k < newCovered.size(); ++k)
          newCovered[k] = true;
      }
    if (removed.empty() && added.empty() && removedMembers.empty() && addedMembers.empty())
      return why = "no function or data member changed", false;
    // Everything else the same: every token gone is a removed function's, every new one an added one's.
    std::vector<bool> reached(out.tokens.size(), false);
    for (size_t k = 0; k < out.map.size(); ++k)
      if (out.map[k] != UINT32_MAX)
        reached[out.map[k]] = true;
      else if (!oldCovered[k])
        return why = "a change outside whole functions and data members", false;
    for (size_t k = 0; k < reached.size(); ++k)
      if (!reached[k] && !newCovered[k])
        return why = "a change outside whole functions and data members", false;
    for (const DeclaredMember* d : removedMembers)
      if (!plainDataMember(*d))
        return why = "a removed or changed data member of a template, a union, or static, an alias", false;
    for (const DeclaredMember* d : addedMembers)
      if (!plainDataMember(*d))
        return why = "an added or changed data member of a template, a union, or static, an alias", false;
    for (const DeclaredFunction* d : removed)
      if (!plainFunction(*d, oldTokens))
        return why = "a removed or changed function others may read without its name (a template, virtual, special, constexpr)", false;
    for (const DeclaredFunction* d : added)
      if (!plainFunction(*d, out.tokens))
        return why = "an added or changed function others may read without its name (a template, virtual, special, constexpr)", false;
    // A declaration taken out while its out-of-line definition stays: a build from scratch reports it.
    for (const DeclaredFunction* d : removed)
      if (!d->f->body && d->container && d->container->kind == NodeKind::ClassDecl && d->container->ownScope)
        for (FunctionDecl* def : kept.global->outOfLineDefinitions(d->container))
          if (kept.global->functionSymbolOf(def) == kept.global->functionSymbolOf(d->f) &&
              std::none_of(removed.begin(), removed.end(), [&](const DeclaredFunction* r) { return r->f == def; }))
            return why = "a declaration removed whose definition stays", false;
    // Each added function's kept declaration to go into: the new one's, by its first token's place.
    std::unordered_map<uint32_t, Decl*> keptContainerAt; // new token -> kept container there
    for (Decl* c : oldContainers)
      if (c->token < out.map.size() && out.map[c->token] != UINT32_MAX)
        keptContainerAt[out.map[c->token]] = c;
    for (const DeclaredFunction* d : added)
    {
      Decl* keptContainer = nullptr;
      if (d->container)
      {
        auto it = keptContainerAt.find(d->container->token);
        if (it == keptContainerAt.end() || it->second->kind != d->container->kind)
          return why = "an added function's class or namespace is new", false;
        keptContainer = it->second;
      }
      out.added.push_back({d->f, keptContainer});
    }
    for (auto& [b, where] : out.parsed->castCandidates)
      for (const DeclaredFunction* d : added)
        if (where == d->f)
          return why = "an added function has a cast to rewrite", false;
    // A private one's name is in its file's private scope, which only a whole index makes (a name
    // made private or global is one of these: removed as the one, added as the other).
    for (const DeclaredFunction* d : added)
      if (d->f->filePrivate)
        return why = "an added function is private to its file", false;
    for (const DeclaredFunction* d : removed)
      if (d->f->filePrivate)
        return why = "a removed function is private to its file", false;
    // A scoped library's top-level function is in the library's namespace, whose declaration only a
    // whole index makes (design.md 16).
    if (out.parsed->libraryScope != UINT32_MAX)
      for (const auto* list : {&added, &removed})
        for (const DeclaredFunction* d : *list)
          if (!d->container)
            return why = "an added or removed function is a top-level one of a scoped library", false;
    for (const DeclaredFunction* d : removed)
      out.removed.push_back({d->f, d->container});
    for (const DeclaredMember* d : addedMembers)
    {
      auto it = keptContainerAt.find(d->container->token);
      if (it == keptContainerAt.end() || it->second->kind != NodeKind::ClassDecl)
        return why = "an added data member's class is new", false;
      out.addedMembers.push_back({d->v, static_cast<ClassDecl*>(it->second)});
    }
    for (const DeclaredMember* d : removedMembers)
      out.removedMembers.push_back({d->v, d->container});
    for (auto& [b, where] : out.parsed->castCandidates)
      for (const DeclaredMember* d : addedMembers)
        if (where == d->v)
          return why = "an added data member has a cast to rewrite", false;
    return true;
  }

  // The list of a declaration's members (or the file's top level) with the removed functions taken
  // out and the added ones put in, in the order of their tokens.
  Span<Decl*> withFunctions(Span<Decl*> list, const std::unordered_set<const Decl*>& removed, const std::vector<Decl*>& added,
                            Arena& arena)
  {
    std::vector<Decl*> out;
    for (Decl* d : list)
      if (!removed.count(d))
        out.push_back(d);
    out.insert(out.end(), added.begin(), added.end());
    std::stable_sort(out.begin(), out.end(), [](const Decl* a, const Decl* b) { return a->token < b->token; });
    Span<Decl*> span;
    span.count = uint32_t(out.size());
    span.data = static_cast<Decl**>(arena.allocate(sizeof(Decl*) * std::max<size_t>(1, out.size()), alignof(Decl*)));
    std::copy(out.begin(), out.end(), span.data);
    return span;
  }

  // Takes the removed functions out of the kept file and its index, points what it keeps at the
  // new tokens, and grafts the added functions in (indexed, attached as finishIndex attaches them).
  // False (the kept build is then unusable) when that needs what only a whole index gives.
  bool graftFunctions(Build& kept, FunctionChange& c, std::string& why)
  {
    size_t i = c.index;
    ParsedFile& file = *kept.parsed[i];
    GlobalScope& global = *kept.global;
    std::unordered_set<const Decl*> removed;
    std::unordered_set<Node*> dropped;
    for (auto& [f, container] : c.removed)
    {
      removed.insert(f);
      global.unindexFunction(f);
      forEachNode(f, [&](Node* n) { dropped.insert(n); });
    }
    for (auto& [v, container] : c.removedMembers)
    {
      removed.insert(v);
      global.unindexMember(v);
      forEachNode(v, [&](Node* n) { dropped.insert(n); });
    }
    if (!remapKeptFile(kept, file, c.map, dropped, {}, *c.parsed))
      return why = "a kept node's token has no place in the new version", false;
    // The added functions: the kept file's, members of the kept declarations.
    std::unordered_map<Decl*, std::vector<Decl*>> into;
    std::vector<FunctionDecl*> added;
    std::vector<VariableDecl*> addedMembers;
    auto ownNodes = [&](Node* root)
    {
      forEachNode(root, [&](Node* n)
      {
        if (n->kind > NodeKind::PackType && n->kind <= NodeKind::ImportDecl)
          static_cast<Decl*>(n)->file = &file;
        else if (n->kind == NodeKind::LiteralExpr)
          static_cast<LiteralExpr*>(n)->file = &file;
      });
    };
    for (auto& [f, container] : c.added)
    {
      ownNodes(f);
      f->parent = container;
      into[container].push_back(f);
      added.push_back(f);
    }
    for (auto& [v, container] : c.addedMembers)
    {
      ownNodes(v);
      v->parent = container;
      into[container].push_back(v);
      addedMembers.push_back(v);
    }
    std::unordered_set<Decl*> touched;
    for (auto& [f, container] : c.removed)
      touched.insert(container);
    for (auto& [v, container] : c.removedMembers)
      touched.insert(container);
    for (auto& [container, list] : into)
      touched.insert(container);
    for (Decl* container : touched)
    {
      const std::vector<Decl*>& in = into[container];
      if (!container)
        file.decls = withFunctions(file.decls, removed, in, file.arena);
      else if (container->kind == NodeKind::NamespaceDecl)
        static_cast<NamespaceDecl*>(container)->decls = withFunctions(static_cast<NamespaceDecl*>(container)->decls, removed, in, file.arena);
      else if (container->kind == NodeKind::ClassDecl)
        static_cast<ClassDecl*>(container)->members = withFunctions(static_cast<ClassDecl*>(container)->members, removed, in, file.arena);
      else if (container->kind == NodeKind::EnumDecl)
        static_cast<EnumDecl*>(container)->members = withFunctions(static_cast<EnumDecl*>(container)->members, removed, in, file.arena);
    }
    kept.files[i].text = std::move(c.text);
    kept.lexed[i].tokens = std::move(c.tokens);
    kept.lexed[i].lineCount = c.lexed.lineCount;
    kept.lexed[i].errorCount = c.lexed.errorCount;
    file.text = kept.files[i].text;
    file.tokens = &kept.lexed[i].tokens;
    file.declCount = c.parsed->declCount;
    file.functionBodies = c.parsed->functionBodies;
    file.nodeCount = c.parsed->nodeCount;
    file.arenaBytesReported = c.parsed->arena.bytesAllocated();
    kept.grafts.push_back(std::move(c.parsed));
    for (auto& [symbol, where] : kept.instanceStats.unresolvedFirst)
      if (where.file == i)
        return why = "an instance's statistic names a place in the file", false;
    for (auto& [symbol, where] : kept.instanceStats.missingMembersFirst)
      if (where.file == i)
        return why = "an instance's statistic names a place in the file", false;
    global.indexMembers(addedMembers);
    if (!global.indexFunctions(added))
      return why = "an added function needs the whole index (a definition matching no declaration, an object declaration)", false;
    return true;
  }

  // The kept build again with the files' new texts, when they changed inside eligible function
  // bodies only: those bodies parsed and checked again and nothing else, then the reports and
  // codegen as a build from scratch runs them. False when the request is no such build: it is built
  // from scratch then (keptChanged: the kept build was changed on the way, and is dropped).
  // headersSame: the C import's headers are as the kept build read them (lastImportStamps).
  bool incrementalBuild(Build& kept, std::vector<SourceFile>& files, double collectTime, bool headersSame, bool& keptChanged)
  {
    using Clock = std::chrono::steady_clock;
    keptChanged = false;
    std::string why;
    if (!kept.keepable || files.size() + 1 < kept.files.size() || !headersSame)
      return false;
    size_t sources = 0; // the kept files without the C import, which the request's files have not yet
    for (const auto& p : kept.parsed)
      sources += !p->isImported;
    if (files.size() != sources)
      return false;
    for (size_t i = 0; i < files.size(); ++i)
      if (files[i].path != kept.files[i].path)
        return false;
    auto start = Clock::now();
    std::vector<ChangedFile> changed;         // bodies only (3b)
    std::vector<FunctionChange> functionEdits; // whole functions (step 4)
    for (size_t i = 0; i < files.size(); ++i)
      if (files[i].text != kept.files[i].text)
      {
        changed.emplace_back();
        if (bodiesOnly(kept, i, files[i].text, changed.back(), why))
          continue;
        changed.pop_back();
        std::string bodiesWhy = why;
        functionEdits.emplace_back();
        if (!functionsOnly(kept, i, files[i].text, functionEdits.back(), why))
        {
          logBuildNote("not incremental: " + files[i].path.string() + ": " + bodiesWhy + "; " + why);
          return false;
        }
      }
    double parseTime = seconds(Clock::now() - start);
    keptChanged = true;
    // The last build generated no code (parse errors, a clash): what it changed, codegen's kept
    // session has not seen, and this build emits it too. What codegen cannot take as changed bodies
    // and globals (functions added or removed, layouts, instances), or could not follow then, makes
    // it run in full.
    std::vector<FunctionDecl*> carriedBodies;
    std::vector<const Decl*> carriedGlobals;
    bool carriedInFull = false;
    if (kept.codegenBehind)
    {
      carriedBodies = kept.changedBodies;
      carriedGlobals = kept.changedGlobals;
      carriedInFull = !kept.rebuildCodegen || !kept.addedFunctions.empty() || !kept.removedFunctions.empty() || !kept.changedLayouts.empty() ||
                      !kept.instancesOf.empty() || kept.positionalCodegenInFull;
    }
    kept.changedBodies.clear();
    kept.addedFunctions.clear();
    kept.removedFunctions.clear();
    kept.changedLayouts.clear();
    kept.instancesOf.clear();
    kept.changedGlobals.clear();
    kept.positionalCodegenInFull = false;
    bool codegenCanFollow = true; // what changed is functions' code only (rebuildLLVM's input)
    std::string codegenInFullBecause; // the first reason it is not (the server's log names it)
    size_t codegenInFullOthers = 0;   // and how many more there are
    auto inFull = [&](std::string why)
    {
      if (codegenCanFollow)
        codegenInFullBecause = std::move(why);
      else
        ++codegenInFullOthers;
      codegenCanFollow = false;
    };
    auto initializerOf = [&](const Decl* d)
    {
      std::string what = d->kind == NodeKind::StaticAssertDecl ? std::string("a static_assert")
                         : d->kind == NodeKind::UsingNameDecl ? "a using-declaration in " + kept.global->qualifiedName(d->parent)
                         : d->kind == NodeKind::VariableDecl  ? "the initializer of " + kept.global->qualifiedName(const_cast<Decl*>(d))
                                                              : "the walk of " + kept.global->qualifiedName(const_cast<Decl*>(d));
      return what + " (" + (d->file ? kept.files[d->file->index].path.filename().string() : std::string("?")) + ")";
    };
    std::vector<FunctionDecl*> templateBodies; // changed bodies of templates: their instances are checked again
    std::vector<const FunctionDecl*> constantBodies; // changed constexpr bodies: what the evaluator ran them for is checked again
    std::vector<FunctionDecl*> plainBodies; // changed bodies of the others: for withBodyCopies
    for (size_t n = 0; n < changed.size(); ++n)
    {
      if (!graftBodies(kept, changed[n], why))
      {
        logBuildNote("not incremental: " + kept.files[changed[n].index].path.string() + ": " + why);
        return false;
      }
      for (size_t k : changed[n].changed)
      {
        kept.changedBodies.push_back(changed[n].oldBodies[k].f);
        if (changed[n].oldBodies[k].templated)
          templateBodies.push_back(changed[n].oldBodies[k].f);
        else
          plainBodies.push_back(changed[n].oldBodies[k].f);
        if (changed[n].oldBodies[k].constant)
          constantBodies.push_back(changed[n].oldBodies[k].f);
      }
    }
    // A changed function that is no template but has a copy of its body in an instance record: the
    // evaluator ran it from that copy (a constexpr one or not: a static_assert, --run-build) or its
    // `auto` return was typed before its file's walk. The copy is made anew, and what the evaluator
    // ran it for is found, as for a constexpr one (it was kept, the old body's answer: silently).
    for (FunctionDecl* f : kept.global->withBodyCopies(plainBodies))
    {
      templateBodies.push_back(f);
      if (std::find(constantBodies.begin(), constantBodies.end(), f) == constantBodies.end())
        constantBodies.push_back(f);
    }
    GlobalScope& global = *kept.global;
    GlobalScope::recordDependencies(true);
    start = Clock::now();
    // The files to walk again: the changed ones, and (whole functions changed) every file a walk
    // of which read one of the functions' names or the declared type of a removed one.
    std::set<size_t> rewalk;
    for (const ChangedFile& c : changed)
      rewalk.insert(c.index);
    std::vector<GlobalScope::ChangedName> names;
    std::vector<const Decl*> gone;
    std::vector<const void*> readingInstances; // instances that read a changed name, type or body: checked again
    std::vector<const Decl*> typedByWalk;      // reading walks' declarations whose type the walk gives: compared after it
    size_t entriesBefore = global.entryCount();
    for (FunctionChange& c : functionEdits)
    {
      rewalk.insert(c.index);
      for (auto& [f, container] : c.removed)
      {
        names.push_back(global.changedNameOf(f));
        gone.push_back(f);
        kept.removedFunctions.push_back(f);
      }
      for (auto& [f, container] : c.added)
        kept.addedFunctions.push_back(f);
      // A data member removed or changed: its name in its class, its declared type, its class's
      // member list (read by position) are what readers read.
      for (auto& [v, cls] : c.removedMembers)
      {
        names.push_back(global.changedNameOf(v));
        gone.push_back(v);
        kept.changedLayouts.push_back(cls);
      }
      if (!graftFunctions(kept, c, why))
      {
        logBuildNote("not incremental: " + kept.files[c.index].path.string() + ": " + why);
        return false;
      }
      // (once grafted: an added member's class is then the kept one, its lookups' scope)
      for (auto& [f, container] : c.added)
        names.push_back(global.changedNameOf(f));
      for (auto& [v, cls] : c.addedMembers)
      {
        names.push_back(global.changedNameOf(v));
        kept.changedLayouts.push_back(cls);
      }
    }
    std::sort(kept.changedLayouts.begin(), kept.changedLayouts.end());
    kept.changedLayouts.erase(std::unique(kept.changedLayouts.begin(), kept.changedLayouts.end()), kept.changedLayouts.end());
    // Codegen takes changed layouts (rebuildLLVM lays them out again, or declines); it runs in full
    // for local types that moved (remapKeptFile).
    if (kept.positionalCodegenInFull)
      inFull("a local type moved (its name is its place)");
    if (carriedInFull)
      inFull("the last build generated no code and changed what codegen does not follow");
    if (!names.empty() || !constantBodies.empty())
    {
      std::vector<const ParsedFile*> readers;
      std::vector<const Decl*> readingWalks;
      if (!global.invalidateReaders(names, gone, readers, &readingWalks, constantBodies, &why, &readingInstances, &typedByWalk, kept.changedLayouts))
      {
        logBuildNote("not incremental: " + why);
        return false;
      }
      for (const ParsedFile* p : readers)
        if (p && !p->isImported)
          rewalk.insert(p->index);
      // Their functions' code changes with what they read (a call bound to a new overload); a
      // global's initializer that read one is no function codegen can emit again on its own.
      std::unordered_set<const Decl*> removedSet(gone.begin(), gone.end());
      for (const Decl* d : readingWalks)
        if (d->kind == NodeKind::FunctionDecl && static_cast<const FunctionDecl*>(d)->body)
        {
          if (!removedSet.count(d))
            kept.changedBodies.push_back(const_cast<FunctionDecl*>(static_cast<const FunctionDecl*>(d)));
        }
        else if (!removedSet.count(d) && d->kind == NodeKind::VariableDecl)
          kept.changedGlobals.push_back(d); // (its constructor emitted again)
        else if (!removedSet.count(d))
          inFull(initializerOf(d) + " read a changed name");
      // The index line counts the entries as the index has them now: what the functions added and
      // took out (scopes none: no function with a class in its body is grafted).
      kept.indexEntries += global.entryCount() - entriesBefore;
      char line[256];
      snprintf(line, sizeof line, "index:   %.3f s  (+%.3f s for out-of-line class definitions): %zu scopes, %zu entries\n", kept.indexTime,
               kept.finishTime, kept.indexScopes, kept.indexEntries);
      kept.indexLine = line;
      if (kept.options.convertDir.empty())
        kept.clashes = global.topLevelClashes();
    }
    // Walked again: what they read recorded anew, and nothing instantiated that a resolve from
    // scratch would count differently.
    std::vector<size_t> walked(rewalk.begin(), rewalk.end());
    // A walk takes back the errors its units reported (forgetWalks) and reports what it finds; one
    // reported outside any unit (the index's) it cannot, and a file with one is not walked again here.
    for (size_t index : walked)
      if (const std::string& path = kept.parsed[index]->path; global.unitlessErrorIn(path))
      {
        logBuildNote("not incremental: " + path + " is walked again and has an error reported outside its walks");
        return false;
      }
    std::vector<std::vector<const void*>> before(walked.size());
    for (size_t n = 0; n < walked.size(); ++n)
    {
      before[n] = global.instancesWalksUse(*kept.parsed[walked[n]]);
      global.forgetWalks(*kept.parsed[walked[n]]);
    }
    // A reading walk's declaration typed by that walk (decltype, auto): its type before, compared
    // with what the walk gives now (changed, what read it is out of date: from scratch).
    std::vector<const Ty*> typesBefore = global.forgetTypesFromWalks(typedByWalk);
    global.beginRewalk();
    // A template's changed body: its instances copied anew and checked again, their statistics
    // replacing theirs (the kept total is made again from every instance walk's own). Before the
    // walks: one that runs the function in the evaluator (a static_assert) runs the instance's copy,
    // which must be the new body's (after them, it ran the old one's and kept its answer).
    size_t droppedInstances = 0; // codegen's, taken out (codegen makes them anew)
    std::vector<const Decl*> instancesOfBodies, instancesOfReaders;
    bool rechecked = templateBodies.empty() || global.recheckInstancesOf(templateBodies, {}, why, &droppedInstances, &instancesOfBodies);
    for (size_t index : walked)
    {
      ResolveStats s;
      s.fileIndex = uint32_t(index);
      s.collectWhy = kept.options.showWhy > 0;
      global.resolveFile(*kept.parsed[index], kept.lexed[index].tokens, kept.files[index].text, s);
      kept.stats[index] = std::move(s);
    }
    // The instances that read a change (invalidateReaders), checked again once the walks are done.
    rechecked = rechecked && (readingInstances.empty() || global.recheckInstancesOf({}, readingInstances, why, &droppedInstances, &instancesOfReaders));
    {
      // (both lists in their files' order, as recheckInstancesOf gives each)
      std::set<const Decl*> seen;
      for (const std::vector<const Decl*>* list : {&instancesOfBodies, &instancesOfReaders})
        for (const Decl* d : *list)
          if (seen.insert(d).second)
            kept.instancesOf.push_back(d);
      std::stable_sort(kept.instancesOf.begin(), kept.instancesOf.end(), [](const Decl* a, const Decl* b)
      {
        uint32_t fa = a->file ? a->file->index : UINT32_MAX, fb = b->file ? b->file->index : UINT32_MAX;
        return fa != fb ? fa < fb : a->token < b->token;
      });
    }
    // Codegen emits again the functions given, those that inlined one of them, the instances of a
    // changed template body, and every instance of a template one of whose instances read a change
    // (instancesOf: those checked again and those taken out, which it makes anew) (rebuildLLVM).
    bool typesSame = true;
    for (size_t n = 0; n < typedByWalk.size(); ++n)
      typesSame = typesSame && global.typeOfDecl(const_cast<Decl*>(typedByWalk[n])) == typesBefore[n];
    bool walkedSame = global.endRewalk();
    bool same = walkedSame && rechecked && typesSame;
    if (!typesSame)
      why = "a declaration typed by its walk (decltype, auto) got another type";
    if ((!templateBodies.empty() || !readingInstances.empty()) && rechecked)
      kept.instanceStats = global.instanceStatsAtResolveEnd();
    if (!walkedSame)
      why = "the new code instantiates what the old did not (or what only codegen had)";
    // What the old walks asked for and no one asks for now (an old lambda's closure in a template's
    // instance): taken out, as a build from scratch never makes it; the instances the rewalk made
    // counted in. The report's counts and the instances' statistics follow.
    GlobalScope::InstantiationStats droppedCounts{};
    std::vector<const Decl*> droppedTemplates;
    for (size_t n = 0; same && n < walked.size(); ++n)
    {
      const ParsedFile& file = *kept.parsed[walked[n]];
      std::vector<const void*> after = global.instancesWalksUse(file), dropped;
      std::set_difference(before[n].begin(), before[n].end(), after.begin(), after.end(), std::back_inserter(dropped));
      if (!dropped.empty() && !global.dropUnusedInstances(dropped, file, droppedCounts, droppedTemplates))
        same = false, why = "an old body's instance nobody else asks for";
    }
    GlobalScope::InstantiationStats made = global.rewalkCounts();
    if (same && (made.classes || made.functions || made.bodies || !droppedTemplates.empty()))
    {
      kept.instantiation.classes += made.classes - droppedCounts.classes;
      kept.instantiation.functions += made.functions - droppedCounts.functions;
      kept.instantiation.bodies += made.bodies - droppedCounts.bodies;
      kept.instanceStats = global.instanceStatsAtResolveEnd();
      // (codegen drops what it emitted for the old closures itself, rebuildLLVM: not every instance of their templates)
    }
    if (!same)
    {
      logBuildNote("not incremental: " + why);
      return false;
    }
    kept.resolveTime = seconds(Clock::now() - start);
    // The reports a build from scratch prints. The lexer's counts come from lexing every file again
    // into a table of its own (the kept table holds the old texts' identifiers too); with --quiet
    // there is no report, and nothing is lexed again (an editor's check, kppc --lsp).
    start = Clock::now();
    if (!quietOutput)
    {
      SymbolTable fresh;
      std::vector<LexResult> relexed(kept.files.size());
      parallelFor(kept.files.size(), kept.options.threads, [&](size_t i)
      {
        if (!kept.parsed[i]->isImported)
          relexed[i] = lex(kept.files[i].text, fresh);
      });
      double lexTime = seconds(Clock::now() - start);
      FileStats total;
      for (size_t i = 0; i < kept.files.size(); ++i)
        if (!kept.parsed[i]->isImported)
        {
          total.bytes += kept.files[i].text.size();
          total.tokens += relexed[i].tokens.size();
          total.lines += relexed[i].lineCount;
          total.errors += relexed[i].errorCount;
        }
      reportLex(sources, total, fresh.size(), kept.options.threads, collectTime, lexTime);
    }
    double lexTime = seconds(Clock::now() - start);
    report("%s", kept.importsLine.c_str());
    reportParse(kept, parseTime);
    report("%s", kept.indexLine.c_str());
    for (const std::string& clash : kept.clashes)
    {
      fprintf(stderr, "%s\n", clash.c_str());
      codegenExit = 1;
    }
    logBuildNote("incremental: " + std::to_string(changed.size() + functionEdits.size()) + " file(s) changed (" + std::to_string(functionEdits.size()) + " with whole functions), " + std::to_string(walked.size()) + " file(s) walked again" +
                 (readingInstances.empty() ? std::string() : ", " + std::to_string(readingInstances.size()) + " instance(s) that read a change checked again") +
                 (droppedInstances ? " (" + std::to_string(droppedInstances) + " made by codegen dropped, to be made anew)" : std::string()) +
                 (kept.changedLayouts.empty() ? std::string() : ", " + std::to_string(kept.changedLayouts.size()) + " class layout(s) changed"));
    {
      // (the server's own part of the build's time, which the report's lines, a build from scratch's, do not show)
      char times[160];
      snprintf(times, sizeof times, "incremental: read the sources %.3f s, parse the changes %.3f s, walks and checks again %.3f s, lexed again for the report %.3f s",
               collectTime, parseTime, kept.resolveTime, lexTime);
      logBuildNote(times);
    }
    // Codegen follows through its kept session when every module's object is where the last build
    // left it (an unchanged module is not assembled again, so it has no text to compile now).
    // The functions whose walks read an edited function's name are emitted again with it: an edit
    // that makes the function trivial makes its callers inline it (codegen's record knows only the
    // callers that inline it already).
    {
      std::vector<GlobalScope::ChangedName> edited;
      for (FunctionDecl* f : kept.changedBodies)
        edited.push_back(global.changedNameOf(f));
      // (An instance whose code holds an edited body, inlined or constant-folded, codegen finds itself:
      // its inlined-body record, cef267b.)
      for (const Decl* d : global.walksReading(edited))
        if (d->kind == NodeKind::FunctionDecl && static_cast<const FunctionDecl*>(d)->body)
          kept.changedBodies.push_back(const_cast<FunctionDecl*>(static_cast<const FunctionDecl*>(d)));
        else if (d->kind == NodeKind::VariableDecl)
          kept.changedGlobals.push_back(d); // (its constructor emitted again)
        else
          inFull(initializerOf(d) + " read an edited function");
    }
    // (and what the last build changed that generated no code: none of it removed since, as a removal
    // there makes codegen run in full)
    {
      std::unordered_set<const FunctionDecl*> removed(kept.removedFunctions.begin(), kept.removedFunctions.end());
      for (FunctionDecl* f : carriedBodies)
        if (!removed.count(f))
          kept.changedBodies.push_back(f);
      for (const Decl* d : carriedGlobals)
        if (std::find(kept.changedGlobals.begin(), kept.changedGlobals.end(), d) == kept.changedGlobals.end())
          kept.changedGlobals.push_back(d);
    }
    std::sort(kept.changedBodies.begin(), kept.changedBodies.end());
    kept.changedBodies.erase(std::unique(kept.changedBodies.begin(), kept.changedBodies.end()), kept.changedBodies.end());
    // An added function is emitted as added, never as a changed one too (its walk may read an edited
    // name: an overload that calls the function it overloads; codegen then emitted it twice).
    {
      std::unordered_set<const FunctionDecl*> added(kept.addedFunctions.begin(), kept.addedFunctions.end());
      std::erase_if(kept.changedBodies, [&](FunctionDecl* f) { return added.count(f) != 0; });
    }
    for (size_t k = 0; codegenCanFollow && k < kept.moduleBytes.size(); ++k)
    {
      std::error_code ec;
      std::string base = kept.options.exe + "." + std::to_string(k);
      if (!kept.options.check && (kept.options.exe.empty() || !std::filesystem::exists(base + ".o", ec) || !std::filesystem::exists(base + ".hash", ec)))
        inFull("module " + std::to_string(k) + "'s object is missing");
    }
    kept.rebuildCodegen = codegenCanFollow && kept.codegen && (!kept.options.exe.empty() || kept.options.check);
    if (!kept.rebuildCodegen)
      logBuildNote(std::string("codegen in full: ") + (!kept.codegen ? "no kept session" : !codegenCanFollow ? codegenInFullBecause + (codegenInFullOthers ? " (and " + std::to_string(codegenInFullOthers) + " more)" : std::string()) : "no executable"));
    noteIncrementalBuild();
    reportResolve(kept);
    kept.retiredCodegen.reset(); // (the client has its answer: reportResolve ends with buildDone)
    return true;
  }
}

int compile(int argc, char** argv);

bool withKeptProgram(const std::function<void(const KeptProgram&)>& f)
{
  std::lock_guard lock(keptMutex);
  if (!keptBuild || !keptBuild->global)
    return false;
  KeptProgram program;
  for (const std::unique_ptr<ParsedFile>& p : keptBuild->parsed)
    program.files.push_back(p.get());
  program.symbols = keptBuild->symbols.get();
  program.global = keptBuild->global.get();
  f(program);
  return true;
}

struct KeptState
{
  std::unique_ptr<Build> build;
  std::string importStamps; // lastImportStamps, which tell its next build whether the C headers changed
};

std::shared_ptr<KeptState> takeKeptState()
{
  std::lock_guard lock(keptMutex);
  auto state = std::make_shared<KeptState>();
  state->build = std::move(keptBuild);
  state->importStamps = std::move(lastImportStamps);
  lastImportStamps.clear();
  return state;
}

void putKeptState(std::shared_ptr<KeptState> state)
{
  std::unique_ptr<Build> previous;
  {
    std::lock_guard lock(keptMutex);
    previous = std::move(keptBuild);
    keptBuild = state ? std::move(state->build) : nullptr;
    lastImportStamps = state ? std::move(state->importStamps) : std::string();
  }
}

// compile's options and how many values each takes, which a project's command line passes on to it
// (project.cpp); compile's own loop below reads them. An option compile reads and this table lacks is
// an error in a project's build.
namespace
{
  const std::pair<const char*, int> compileOptions[] = {
    {"-j", 1}, {"--ext", 1}, {"--repeat", 1}, {"--dump", 0}, {"--lex-only", 0}, {"--errors", 1}, {"--unresolved", 1},
    {"--why", 1}, {"--why-at", 1}, {"--access-warnings", 0}, {"--struct-rule", 0}, {"--c-type-warnings", 0},
    {"--quiet", 0}, {"--msvc-diagnostics", 0}, {"--diagnostic-columns", 0}, {"--check", 0}, {"--emit-llvm", 1},
    {"--exe", 1}, {"--run-build", 1}, {"--std", 1}, {"--no-std", 0}, {"--cinclude", 1}, {"--incremental-link", 0}, {"--defer-link", 0},
    {"--toolchain", 1}, {"--debug-info", 0}, {"--optimize", 0}, {"--no-optimize", 0}, {"--target", 1},
    {"--sysroot", 1}, {"--keep-going", 0}, {"--stub-missing", 0}, {"--no-inline", 0}, {"--clang-flags", 1},
    {"--codegen-threads", 1}, {"--lib", 1}, {"--exclude", 1}, {"--library-root", 2}, {"--unscoped-library-root", 2},
    {"--config", 1}, {"--release", 0}, {"--tests", 0}, {"--modules", 1}, {"--convert", 1},
    {"--verify-incremental", 0}, {"--overrides", 1}, {"--convert-base", 1}, {"--c-sources", 0}};
}

int compileOptionValues(const std::string& arg)
{
  for (const auto& [name, values] : compileOptions)
    if (arg == name)
      return values;
  return -1;
}

int optionError(const std::string& arg)
{
  int values = compileOptionValues(arg);
  if (arg == "--version" || arg == "--help")
    fprintf(stderr, "error: %s takes no other arguments\n", arg.c_str());
  else if (values > 0)
    fprintf(stderr, "error: %s needs %s\n", arg.c_str(), values == 1 ? "a value" : "two values");
  else
    fprintf(stderr, "error: unknown option %s\n", arg.c_str());
  fprintf(stderr, "kpp --help lists the options\n");
  return 2;
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0); // progress lines survive a crash
  std::set_terminate([]
  {
    try
    {
      if (auto e = std::current_exception())
        std::rethrow_exception(e);
    }
    catch (const std::exception& e) { fprintf(stderr, "fatal: %s\n", e.what()); }
    catch (...) { fprintf(stderr, "fatal: unknown exception\n"); }
    abort();
  });
  // argv[0] as the executable's full path, also when found on PATH: a compile server, which runs
  // from a copy of kppc, finds the standard library next to the client's (source.hpp).
  static std::string self = compilerPath().string();
  if (argc > 0 && !(argc == 3 && std::string(argv[1]) == "--serve"))
    argv[0] = self.data();
  // The compile server (server.hpp): `--server` anywhere makes this a client, `--stop-server`
  // stops the server of the same arguments, `--serve KEY` is the server a client started.
  if (argc == 3 && std::string(argv[1]) == "--serve")
    return runServer(argv[2], compile);
  // `--version` and `--help` alone, in a project's directory too; with other arguments, an error
  // (optionError).
  if (argc == 2 && std::string(argv[1]) == "--version")
    return printVersion();
  if (argc == 2 && std::string(argv[1]) == "--help")
    return usage(stdout), 0;
  // The language server (lsp.hpp): `kppc --lsp`, started by an editor, JSON-RPC on stdin and stdout.
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--lsp")
      return runLanguageServer(argc, argv, compile);
  // A project's build.kpp (project.hpp): `kpp` with no sources in its directory, `kpp new`.
  if (int code = runProject(argc, argv, compile); code >= 0)
    return code;
  // Project files for an IDE (generate.hpp) take the build's own arguments.
  for (int i = 1; i < argc; ++i)
    if (isGenerateOption(argv[i]))
      return generateProjectFiles(argc, argv);
  // `--run` and `--test` (run.hpp): the arguments after the sources are the program's.
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--run" || std::string(argv[i]) == "--test")
      return runProgram(argc, argv, i);
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--server" || std::string(argv[i]) == "--stop-server")
    {
      bool stop = std::string(argv[i]) == "--stop-server";
      std::vector<char*> rest(argv, argv + argc);
      rest.erase(rest.begin() + i);
      rest.push_back(nullptr);
      return stop ? stopServer(argc - 1, rest.data()) : runClient(argc - 1, rest.data(), compile);
    }
  return compile(argc, argv);
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
    else if (arg == "--keep-going")
      options.keepGoing = true;
    else if (arg == "--stub-missing")
      options.stubMissing = true;
    else if (arg == "--no-inline")
      options.noInline = true;
    else if (arg == "--clang-flags" && i + 1 < argc)
      options.clangFlags = argv[++i];
    else if (arg == "--codegen-threads" && i + 1 < argc)
      options.codegenThreads = unsigned(atoi(argv[++i]));
    else if (arg == "--lib" && i + 1 < argc)
      options.libs.push_back(argv[++i]);
    else if (arg == "--exclude" && i + 1 < argc)
      options.excluded.push_back(argv[++i]);
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

  // Phase 0: find and read all files. Reading is I/O bound, so it is timed separately.
  auto startCollect = Clock::now();
  std::vector<std::filesystem::path> paths;
  std::vector<FileStamp> stamps; // (paths[i]'s)
  std::vector<int> pathLibrary;  // (paths[i]'s: the index of its options.libraryRoots entry, -1 for the program's)
  auto collect = [&](const std::filesystem::path& root, int library)
  {
    auto found = collectFiles(root, options.extensions, &stamps);
    paths.insert(paths.end(), found.begin(), found.end());
    pathLibrary.resize(paths.size(), library);
  };
  // The standard library first, then the libraries in the order given, then the program.
  size_t stdRoots = options.libraryRoot.empty() ? 0 : 1;
  for (size_t i = 0; i < stdRoots; ++i)
    collect(options.roots[i], -1);
  for (size_t i = 0; i < options.libraryRoots.size(); ++i)
    collect(options.libraryRoots[i].dir, int(i));
  for (size_t i = stdRoots; i < options.roots.size(); ++i)
    collect(options.roots[i], -1);
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
        stamps[kept++] = stamps[i];
      }
    if (kept == paths.size())
    {
      fprintf(stderr, "error: --exclude %s names no file under the sources\n", excluded.c_str());
      return 1;
    }
    paths.resize(kept);
    pathLibrary.resize(kept);
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
  // stands (an overlay, kppc --lsp), is the kept build's text: not read again (the listing gave its
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
    if (options.incrementalLink && bundledToolchain())
    {
      fprintf(stderr, "--incremental-link is Visual Studio's link.exe's, which the bundled toolchain does not have (its lld-link "
                      "links whole): add --toolchain msvc\n");
      return 2;
    }
    // config::platform is the target's.
    const char* platform = options.target.os == Target::Os::Linux ? "Linux" : options.target.os == Target::Os::FreeBSD ? "FreeBSD" : "Windows";
    if (!options.configValues.count("platform"))
      options.configValues["platform"] = parseConfigValue(platform);
    else if (options.configValues["platform"].text != platform)
    {
      fprintf(stderr, "--config platform=%s: the platform is the target's (--target), %s\n", options.configValues["platform"].text.c_str(), platform);
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
    SourceFile config;
    config.path = "<config>";
    config.text = configSource(options.configValues);
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
    }
  }
  // A compile server's build whose every input is as the previous one's (step 3a of the
  // persistent compiler, docs/compile-speed.md): the previous build's output again.
  if (servingBuild() && options.convertDir.empty())
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
    std::vector<std::string> outputs;
    for (const std::string& o : {options.emitLLVM, options.exe})
      if (!o.empty())
        outputs.push_back(std::filesystem::absolute(o).string());
    // What the build leaves: its outputs, but under --defer-link the link command in place of the
    // executable, which kpp links (and links again when it is missing, project.cpp), and the modules'
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
    if (keptBuild && !replayDisabled() && options.repeat == 1 && !options.dumpTokens && options.parse) // (--verify-incremental's build is from scratch)
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
        build->keepable = options.repeat == 1 && options.convertDir.empty() && !options.dumpTokens;
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
