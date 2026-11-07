// The driver: the command line's options, the report, usage and the version; parallelFor; its
// declarations in Options.hpp (Driver.hpp). A translation unit of its own since 2026-11-05
// (main.cpp split, stage 2), moved without a change.
#include "Driver.hpp"

namespace driver
{
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

bool isCSource(const std::filesystem::path& path)
{
  return path.extension() != ".kpp" && path.string().rfind("<", 0) != 0;
}

void usage(FILE* out)
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
          "  --override-warnings  warn of each class with a virtual function whose destructor is not virtual, which\n"
          "                kpp makes virtual (docs/design.md 23b); no error, no other change\n"
          "  --explicit-by-default  a constructor or conversion operator converts only when marked implicit (design.md,\n"
          "                \"Explicit by default\": the flip before it is the default), and its errors are on\n"
          "  --this-warnings  warn of each unqualified name that finds a data member or a member function, which\n"
          "                required `this.` writes `this.x` (the readme's Planned); no error, no other change\n"
          "  --plain-enum-warnings  warn of each plain (unscoped) enum and each use of its enumerators by kind (bits,\n"
          "                integer, array index or size: kpp has only enum class, docs/design.md 8.4); no other change\n"
          "  --this-reference-warnings  warn of each use of `this` that changes once it is a reference (`this->`,\n"
          "                `*this`, its address, `[this]`, a null test: docs/design.md 23d); no error, no other change\n"
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
          "  --natvis F    the program's own .natvis (Visual Studio's rules for its types), linked into the PDB of\n"
          "                a --debug-info build for Windows beside the standard library's (repeatable; build.kpp's natvis)\n"
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
          "  --target T    build for T: windows (the default), linux, freebsd, macos, macos-x64, or a triple\n"
          "              (docs/design.md, \"Targets and hosts\"); sets config::platform and config::arch\n"
          "  --sysroot D   the target's C headers and libraries, for a target other than the host\n"
          "                (tools/sysroot/ makes one for Linux from WSL); for macos, the macOS SDK (or SDKROOT)\n"
          "  --convert D   rewrite the C++ inputs as kpp sources under D (see docs/path-to-factorio.md)\n"
          "  --convert-base B  with --convert: each file at its input's path below B, not below its input\n"
          "  --convert-release with --convert: preprocess as the game's Release (NDEBUG), not FastDebug\n"
          "  --convert-final   with --convert: preprocess as the game's FinalRelease (Release, FINAL_RELEASE, ALL_IN_ONE)\n"
          "  --convert-marks   with --convert: mark each #if group, for --convert-merge\n"
          "  --convert-platform windows|linux  with --convert: preprocess as the game's build for that platform\n"
          "  --convert-predefines FILE  with --convert-platform linux: the compiler's predefined macros (clang -dM -E)\n"
          "  --convert-merge-platform W L OUT  one configuration's marked Windows and Linux conversions as one, by config::platform\n"
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
          "                it builds through the compile server unless --no-server is among the options;\n"
          "                in a project's directory F is a .kpp file, any other first argument the project's\n"
          "  --tests       build the program's `test \"name\" { }` declarations with the test runner as main\n"
          "                (config::tests is true; docs/design.md 8.6)\n"
          "  --test F... [-- ARGS]  build the tests of F (files or directories) with the options before --test\n"
          "                and run them; ARGS go to the runner: --filter TEXT, --list, --report FILE\n"
          "  --no-run      with --run or --test: build only, and name the executable (one for another\n"
          "                --target, which this machine does not run, needs it)\n");
}

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
  // (the toolchain's default target: the bundled one's windows-gnu, else the host's own platform)
  std::string byDefault = std::string("builds ") + hostTarget().name + " by default";
  std::string kind = bundledToolchain()      ? std::string("bundled: builds windows-gnu by default")
                     : getenv("KPP_CLANG") ? std::string("KPP_CLANG's")
                     : hostTarget().windows() ? "Visual Studio's: " + byDefault
                                              : "the system's: " + byDefault;
  std::error_code ec;
  if (!problem.empty())
    printf("toolchain: none: %s\n", problem.c_str());
  else if (std::filesystem::exists(clang, ec))
    printf("toolchain: %s (%s)\n", clang.c_str(), kind.c_str());
  else
    printf("toolchain: none: %s (%s) does not exist\n", clang.c_str(), kind.c_str());
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
  // (sorted: a directory's order is its file system's, alphabetical on NTFS and not on Linux's)
  std::vector<std::string> found;
  for (std::filesystem::directory_iterator i(libraries, ec), end; !ec && i != end; i.increment(ec))
    if (std::filesystem::is_regular_file(i->path() / "build.kpp", ec))
      found.push_back(i->path().filename().string());
  std::sort(found.begin(), found.end());
  std::string names;
  for (const std::string& n : found)
    names += (names.empty() ? "" : ", ") + n;
  printf("libraries: %s (%s)\n", libraries.string().c_str(), names.empty() ? "none" : names.c_str());
  return 0;
}
}
