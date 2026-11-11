# Building kpp programs

How a kpp program is built today, where FASTBuild fits in, the build description (`build.kpp` in
kpp, decided 2026-10-31: the layout, adding libraries, configurations), and the Visual Studio
integration (`tools/vs/`). Open details are the questions at the end.

## Today

**One command builds a program.** `kpp [options] <file-or-dir>...` compiles the whole program
in one process. It takes every `.kpp` under the given roots plus the standard library (`stdlib/`
next to kpp, or `--std DIR`, or none with `--no-std`). It runs the front end and code generation
on all threads and splits the IR into modules (two per thread by default, `--modules N`). It
compiles each module, skipping modules whose IR hash is unchanged (the `.N.hash` and `.N.o` files
next to the executable are the object cache), and links. A debug build for Windows (windows or
windows-gnu) compiles its modules in kpp, with LLVM's back end linked in (src/backend.cpp,
docs/codegen-speed.md), when kpp was built with it; every other build runs the bundled clang per
module on its `.N.ll` file. Either way LLVM's verifier checks every module first (clang with
`-fverify-intermediate-code`, which a release clang leaves off: it compiled an IR bug of kpp's at
-O0 and hung on it at -O2; tests\verifier.ps1), at no measurable cost (the game's release modules
compiled in 9.3 to 10.2 s with it, 9.3 to 9.7 s without). There
are no headers, no per-file compile steps and nothing like make: a "build system" for kpp code is
only the choice of kpp's arguments.

**What controls a build: command-line options only.**

| Option | What it does |
|---|---|
| sources | directories (recursive) and files; every one is part of the one program |
| `--exe F` | the executable; the object cache sits next to it |
| `--config N=V` | a constant `config::N` (bool, integer, enumerator, string) the code reads with `if constexpr` |
| `--release` | the same as `--config debug=false` |
| `--optimize`, `--no-optimize` | clang `-O2` (ThinLTO, a whole link) or `-O0`, whatever `config::debug` is; unset, `-O2` when `config::debug` is false |
| `--lto thin\|full` | an optimized build's optimization across modules, in lld's link: ThinLTO (the default) or full LTO, one optimization over the whole program ("Whole-program optimization" below) |
| `--lto-partitions N` | full LTO's code generated in N partitions (the default: one per thread); 1 when C libraries built with `-flto` join it |
| `--opt-level N` | clang's `-O` for an optimized build's modules and its link: 2 (the default) or 3 |
| `--lib F` | passed to the link: a `.lib`, `-lname`, `-Wl,/OPT`, a `.res` |
| `--cinclude D` | a directory searched by the C headers the code imports |
| `--clang-flags F` | more flags for clang's compile of the modules |
| `--sanitize L` | sanitizers, `address`, `thread` (clang's), `undefined` and `overflow` (kpp's own) ("Sanitizers" below); `config::sanitizeAddress`, `config::sanitizeThread`, `config::sanitizeUndefined` and `config::sanitizeOverflow` say which |
| `--incremental-link` | debug builds only: link.exe `/INCREMENTAL`, so the next link redoes only changed objects; the modules have no section per function (the link keeps every function) |
| `--file-prefix-map FROM=TO` | the source paths the program's text holds (an assertion's message, a `CallerLocation`'s file) start with TO in place of the directory FROM, `/` between names; diagnostics and debug info keep theirs. kpp's release and final builds pass the standard library's as `stdlib`, each kpp library's as its name, and the project's as nothing (`src/main.kpp`), so that a release binary holds no path of the machine that built it |
| `--no-inline` | kpp emits no trivial function into its callers (a diagnostic tool; inlining is on by default) |
| `--keep-going`, `--stub-missing` | bring-up tools: link what was emitted despite errors, stub what is missing |
| `--server` | build through a persistent compile server ([compile-speed.md](compile-speed.md)) |
| `--msvc-diagnostics` | errors as `path(line): error: ...`, which Visual Studio's Error List reads |

**Two switches: `config::debug` and optimization.** `config::debug` defaults to true, and
optimization follows it unless `--optimize` or `--no-optimize` says otherwise (build.kpp's
fastdebug is `config::debug` true, optimized):

| | `config::debug` true (default) | false (`--release`) |
|---|---|---|
| `assert` | compiled in | compiles to nothing (`release_assert` stays) |
| clang (`--optimize` decides) | `-O0` | `-O2`, ThinLTO when there are several modules (lld runs the back ends) |
| link (`--optimize` decides) | link.exe, `--incremental-link` allowed | lld, whole program; `--incremental-link` is an error |
| debug info | only with `--debug-info` (CodeView line tables, a PDB; ee17f9a8) | the same |
| kpp inlining of trivial functions | on | on |

**Whole-program optimization (2026-11-04).** `--lto full` is kpp's part of factorio-in-kpp's final
configuration, kpp's FinalRelease ([final-release.md](final-release.md)): the official Factorio builds
are compiled with `/GL` and linked with `/LTCG`, one optimization over the whole program, its C and
C++ libraries included. With `--lto full` the modules become bitcode for full LTO (`-flto`, not
`-flto=thin`), and lld merges them, with the C libraries a configuration builds with `-flto`
(build.kpp's `cFlag`), into one module, optimizes it once, and generates its code in one partition
per thread (`/opt:lldltopartitions`, as `/LTCG`'s `/CGTHREADS`; lld's MinGW driver has no such
option, so windows-gnu generates it in one). C libraries built with `-flto` join it only with one
partition (`--lto-partitions 1`): to generate the partitions in parallel, LLVM's module splitting makes
every static two partitions share external under its own name, and the game's SDL (hidapi's
`CM_Locate_DevNodeW`, `PKEY_DeviceInterface_Bluetooth_*`) then collides with SETUPAPI.lib's and
uuid.lib's symbols of those names (B, 2026-11-04; the game measured no gain from its C libraries in
the LTO). ThinLTO, the release default, optimizes each module
with what it imports from the others, in parallel, and keeps the unchanged modules' results in a
cache. `--opt-level 3` makes the modules' and the link's optimization `-O3` (lld's `/opt:lldlto=3`,
`--lto-O3`). Either way the C runtime is the static one (`/MT`: clang links libcmt, and the
executable imports no VCRUNTIME or api-ms-win-crt DLL), and lld's LTO puts every function and datum
in a section of its own, as `/Gy` and `/Gw` do, for `/OPT:REF`. kpp compiles every C library
without stack cookies (`/GS-`, clang's `-fno-stack-protector`), as both of Factorio's C++ builds do;
clang-cl's default put them in until 2026-11-04. tests\lto.ps1 builds a program both ways for Windows
and windows-gnu; tests\project.ps1 builds a configuration with `--lto full` and C flags of its own.

**The compile server.** `--server` anywhere on the line makes kpp a client. The server is keyed by
kpp's path, the working directory and the argument list, so every distinct argument list has its
own server. A server keeps the last build's program in memory, rebuilds only what an edit touched,
and ends after an hour idle, or as soon as the kpp it was started from changes (a rebuild, an
install). `--stop-server` with the same arguments stops it; `--stop-all-servers` alone (kpp's or
kpp's) stops every compile server of this user. `--msvc-diagnostics` is left out of the key: it changes
only the form a diagnostic's place is printed in, which the client gives it (the server marks each
place), so a Visual Studio build and the command line's share one server and its kept build, each
reading its errors in its own form.

**Long paths (2026-11-08).** Windows' tools open no path of 260 characters or more (MAX_PATH),
whatever the system's LongPathsEnabled says: kpp has no long-path manifest, and link.exe (MSVC
14.51) cannot open its output there even when given it as a `\\?\` path (LNK1104); lld-link and
clang can. The longest name kpp writes beside the executable is `NAME.failed-functions.txt`, so on a
Windows host an `--exe` whose full path is longer than 238 characters is a command-line error
before any work (exit 2, naming the limit); it used to fail with "cannot write" for each module's
file. kpp does not rewrite its paths as `\\?\` ones: the MSVC toolchain's link would still fail, and
every file kpp touches would need it. The spec runner keeps its paths short instead: a snippet's
build directory is `<area>.<name>`, a name of more than 40 characters cut to 32 and its SHA-256's
first 8 hex digits, and its program is `program.exe` (an 80-character name in a worktree under
`C:\kpp\.claude\worktrees` made `<area>.<name>\<area>.<name>.exe.link.rsp` 260 characters).
tests\longpaths.ps1 builds an executable at 238 characters, expects the error at 239, and runs that
snippet under a 140-character output directory.

**The allocator (2026-11-09).** kpp's malloc is rpmalloc 1.4.5 (public domain), vendored unchanged
in compiler/third_party/rpmalloc (its README names the archive and its SHA-256), on every host,
with LLVM or without. Until then only kpp built with LLVM had it, by accident: LLVM's Windows
release links rpmalloc into LLVMSupport.lib, whose malloc the linker took before the C runtime's.
That copy was worth about 35% of the front end (A's measurement: resolve 1.40 to 0.87 s, instance
checking's CPU 28.5 to 18.2 s), and a build without LLVM, a Linux build or a changed link order
lost it without a word. compiler/src/allocator.c compiles rpmalloc into kpp:
- Windows: kpp is built with the static C runtime (/MT) with LLVM or without, since only that lets
  an executable replace malloc. rpmalloc's own malloc.c defines malloc and the runtime's
  `_malloc_base` family, which the linker takes before libucrt's (and before LLVMSupport.lib's
  copy, which is no longer linked). A thread's heap is made at its first allocation and released
  at its exit by a fiber-local storage callback: LLVM's configuration.
- Linux and FreeBSD: allocator.c defines malloc, free, calloc, realloc, the aligned ones and
  malloc_usable_size in the executable, where they interpose libc's for every library. It does not
  use rpmalloc's preload mode, which replaces pthread_create through dlsym and finalizes the
  allocator from a destructor at exit, while kpp's detached threads (the compile server's idle
  check) may still allocate. Instead a thread's heap is made at its first allocation and released
  at its exit by a pthread key's destructor. FreeBSD is compiled against its sysroot but not run
  (kpp does not run there yet).

`kpp --version` checks it: each way kpp allocates (malloc, calloc, realloc, operator new and
new[], the C runtime's strdup) must return the block rpmalloc's heap of that thread hands out
next. Its last line is `allocator: rpmalloc 1.4.5`, or `allocator: not rpmalloc 1.4.5: ...`
naming the ones that went elsewhere, with exit code 1. run_tests' project test and CI's Linux
jobs run it. A build with the override taken out fails it on all six.

**Sanitizers (2026-11-11).** `--sanitize address` builds with AddressSanitizer: every load and
store of kpp's code is checked, and a use after free, a heap or stack overflow or a use after return
stops the program with ASan's report and exit code 1. `--sanitize thread` builds with ThreadSanitizer,
for Linux only (clang has no runtime for Windows): an error for another target, never a build without
it. kpp writes `sanitize_address` (or `sanitize_thread`) on every function it defines and passes
`-fsanitize=...` to clang's compile and link. The attribute is what clang's instrumentation looks for:
clang's front end adds it to C++, and kpp's IR had none, so until 2026-11-11 `--clang-flags
-fsanitize=address` (with `--lib -fsanitize=address`) linked ASan's allocator alone, which finds a
double free and nothing that reads or writes. clang's `-fsanitize=undefined` does nothing on kpp's IR
at all: UBSan's checks are written by clang's front end, not by an LLVM pass, so kpp has its own.
- `--sanitize undefined` (kpp's own, every target, either back end, no runtime library): what C++
  leaves undefined in integer arithmetic and conversions is checked where it runs, and calls the
  assert hook with the file, line and column and a message in the constant evaluator's words: a
  division or remainder by zero and INT_MIN / -1 (`a division by zero`), and a float or
  double converted to an integer that cannot hold it (`a floating value out of the range of uint8`:
  Factorio's own UBSan build checked this alone). `--sanitize overflow` adds signed overflow of
  `+ - *` and unary minus (`signed overflow in int32 (addition)`), a sanitizer of its own because kpp's
  signed arithmetic wraps, as Factorio's builds do with -fwrapv (its circuit network relies on it).
  Unsigned arithmetic wraps, as defined, and imported C keeps C's behaviour. A hook that returns (a
  test's) goes on: the arithmetic wrapped, a division by 1 in place of the processor's trap. A shift
  count is no check: kpp takes it modulo the width, defined (design.md, "Sanitizers and debug checks"). A check in a trivial callee inlined in its caller holds the callee's
  line, which a compile server patches or emits again when an edit moves it (2d4a165c9). `config::sanitizeUndefined` and
  `config::sanitizeOverflow` say they are on. Both go with `address` or `thread`.
- The game's tests under `--sanitize undefined,overflow` (factorio-in-kpp 6245f6046, debug, 2026-11-11):
  264 of 9481 fail, at 13 places. `undefined` alone: SmallProgressValue's `u8(value * 254)` given 254
  where a byte was meant (222 tests; the value came from a miscompiled copy assignment,
  spec classes/copy_assignment_beats_assignment_from_conversion, fixed by D) and the arithmetic
  combinator's shifts by 32 or more (2 tests: C++'s undefined behaviour, which x86 masks, and the
  game's tests expect; kpp has taken a shift count modulo the width since). The other 11 places are
  signed overflow the game means to wrap (the circuit network's arithmetic, Math::addSat,
  RandomGenerator's seed, TechnologyGraphGui's empty layer range, StringUtil's -INT64_MIN).
- The sanitizer is part of each module's cache key (it goes with `--clang-flags`), and a sanitized
  build is clang's: the back end in kpp steps aside, as for `--clang-flags`.
- Windows (MSVC): ASan is linked statically, with `/NOIMPLIB /NOEXP` (its runtime exports its
  interface from the executable, and link.exe would write an import library beside it on every
  build). windows-gnu: MinGW-w64's ASan is a DLL only, so kpp copies llvm-mingw's
  `libclang_rt.asan_dynamic-x86_64.dll`, `libc++.dll` and `libunwind.dll` beside the executable.
  Linux: clang links it statically (Ubuntu's clang-19 has it in libclang-rt-19-dev). FreeBSD and
  macOS: an error, kpp's toolchain has no runtime for them.
- C libraries kpp compiles and the prebuilt libraries it links are not instrumented (a
  configuration's `cFlag("-fsanitize=address")` adds kpp's C libraries); their memory still comes
  from ASan's malloc.
- A configuration in build.kpp: `project.configuration("asan", "debug").sanitize("address");`. Such a
  configuration never links incrementally (link.exe's /INCREMENTAL and ASan's runtime do not go
  together).
- Measured on the game's tests (tests.cmd, factorio-in-kpp 6eb91726a, 16 shards, one round each,
  clang's back end both): the build 17 s debug, 31 s with ASan; the run 72 s, 191 s; factorio-test.exe
  144 MB, 410 MB; 9481 tests passed both ways, with no ASan report.

**Programs describe their builds in .cmd scripts.** `programs/factorio/link-settings.cmd` sets
`KPP`, `CINC` (8 `--cinclude`) and `LIBS` (19 libraries of the game's C++ build, 21 Windows
libraries, the shims and the resource file). `link.cmd` and `run.cmd` both call it and then each
write their own kpp line: 6 source roots and 4 (link) or 9 (run) more options.
`tools/vs/factorio/generate.cmd` is a third copy of run.cmd's line. Three copies of one build is what a project file removes.

## FASTBuild

**FASTBuild plays no part in building kpp code.** kpp never calls it and has no use for it: there
is one compile per program, already parallel and cached, so a distributed per-file build tool has
nothing to schedule. FASTBuild remains Factorio's C++ build (`C:\Factorio\fastbuild\fbuild.bff`).

**Who builds the C and C++ libraries the game links** (settled by what exists, 2026-11-06):

- **`kpp` builds them itself.** factorio-in-kpp's build.kpp lists them as `cLibrary` entries, built
  from `libraries\` by kpp with clang-cl into `build\clib\` (7074036f, factorio-in-kpp 709fbd7f, both
  2026-10-31), so a kpp build of the game follows the source it was converted from. `--set
  PREBUILT=DIR` links a Factorio checkout's C++ Release `.lib` files instead.
- **The scripts link FASTBuild's.** convert.cmd, run.cmd and tests.cmd (link-settings.cmd) take the
  26 `.lib` files in `C:\Factorio\vs2022\Releasex64`, outputs of the C++ Release build. FASTBuild may
  write there (its build outputs, CLAUDE.md, 2026-10-31): those files were rebuilt on 2026-10-31 at
  4c178a5b915, Factorio's HEAD, and no commit has touched `libraries/` since. They were April's
  before that, 54 `libraries/` commits behind. After a pull that touches `libraries/`, they need
  that FASTBuild run again, or the game compiles against new headers and links old code.
- `programs/factorio/native/build.cmd` builds the shims with clang-cl directly, not with FASTBuild.

## The build description

**Decided by kovarex (2026-10-31):**
- A project's builds are described in `build.kpp`, a kpp file at the project's root.
- The project has `src/` for its code and `libraries/` for its own libraries and forks.
- One project may build several executables from shared sources. A target leaves files out
  (Factorio's `Main`) instead of having its `main` replaced.

The details below may still change. What kovarex asked for: one description of a project's builds
(targets, steps, libraries) for every tool, not only Visual Studio; no special build language
such as CMake's or a script's; starting must take nothing; and adding a library must be easy.

### How others describe builds

| | Build file | What it may do | With no file | A changed copy of a library |
|---|---|---|---|---|
| CMake | its own language | anything: files, processes | nothing builds | by hand |
| Cargo (Rust) | `Cargo.toml` data, plus `build.rs`, a Rust program | `build.rs` runs unsandboxed before the build | `cargo new` writes 6 lines; `src/main.rs` by convention; `dev` and `release` built in | `[patch]` replaces it in the whole graph |
| Go | none; `go.mod` is data (module, requirements) | nothing; `go generate` steps are run by hand | a directory is a package; `go build` | `replace` in go.mod |
| Zig | `build.zig`, Zig compiled and run natively; `build.zig.zon` data for dependencies | anything; it builds a graph of steps that the build runner executes | `zig run file.zig`; `zig init` writes about 80 lines | a path dependency |
| Jai | a `#run` metaprogram in the program's language | anything, at compile time | `jai main.jai` | by hand |
| Nim | NimScript (`config.nims`, `.nimble`): Nim run by the compiler's own VM | the VM has `exec` and file I/O | `nim c main.nim` | by hand |
| Swift | `Package.swift`, Swift evaluated into a JSON description | evaluated in a sandbox (macOS); build steps are plugins with declared inputs and outputs | `swift package init` writes about 20 lines | `.package(path:)` |
| Bazel, Buck | Starlark, a deterministic Python dialect without I/O | only declares; the declared actions run with declared inputs and outputs | nothing builds | a local repository override |
| Meson | its own language, on purpose not Turing-complete | declares; custom targets run commands | nothing builds | subprojects |

What the comparison says:
1. **The description should only describe.** Running a tool is a step that names its inputs and
   outputs, and the build tool runs it and caches the result (Bazel, Swift's plugins, Zig's step
   graph). Where the description language can act (Cargo's `build.rs`, Zig, Jai, Nim), every
   dependency runs code on the developer's machine, and the build depends on the machine it ran
   on.
2. **Defaults carry most programs.** Go needs no file. Cargo's is 6 lines. Jai and Zig compile a
   lone file.
3. **Tools must edit the dependency list.** Zig put it in a data file (`build.zig.zon`) next to
   the code. Swift's `Package.swift` is code, and editing it from a tool came years late, through
   a Swift parser.

kpp is placed unusually well for point 1. Its compile-time interpreter (design.md,
"Compile-time evaluation") already runs ordinary kpp with no I/O and no C calls, under step and
memory limits: Starlark's sandbox, without a second language.

### build.kpp

- **What it is.** kpp compiles `build.kpp` on its own, with the standard library and a small
  `build` package (`Project`, `Executable`, `Step`), and runs its `build` function in the
  interpreter once, for every target and configuration (below, "Targets and configurations"). The
  function can compute (loops, functions, the standard
  library), but it can only describe: it fills in a `Project`, and kpp builds what that says.
- **It cannot act.** It cannot read a file, start a process or call C. That is the interpreter's
  existing limit, so no separate sandbox is needed. What a used library's description can still point
  kpp at (its roots, system libraries and use() paths) is confined, and the rest of a used library's
  description is not taken: design.md, "What a library's build.kpp can do".
- **Its errors are kpp errors**, with the same Error List, colouring and IDE navigation.
- **The program does not see build.kpp's declarations.** What crosses over are the `config::`
  constants the build sets.
- **Inputs from outside come only from the command line.** `kpp --set FACTORIO=D:\snapshot` is
  read as `project.setting("FACTORIO", "C:/Factorio")`, so a build depends only on its files and
  its arguments. The arguments are part of the server's key.
- **Its cost:** about 2 s for factorio-in-kpp's (2.2 s, best of four, `kpp --run-build` through
  the compile server on 2026-11-08; a build.kpp of one executable 0.12 s), and the result is kept
  until the file changes.
- **Its budget:** `build()` and `describe()` run as one evaluation of at most 32 million steps and
  32 million values made, where a constant expression has a million steps (the interpreter's
  ConstBudget). factorio-in-kpp's takes about 2 million (a String's append is a hundred steps or
  two), so it has sixteen times its room, and a loop that never ends stops at its line with "an
  evaluation of more than 32 million steps" (a bare one within 2 s). Until 2026-11-08 each
  statement of `build()` started a new count of a million, and one function of 2,000 `exclude()`
  lines did not fit in it (run_tests' runbuild check describes that function now).

```cpp
// build.kpp of programs/factorio
void build(Project& project)
{
    std::StringView factorio = project.setting("FACTORIO", "C:/Factorio");
    project.use("sdl3", "3.4");                // for every target
    project.use("lua");
    project.use("factorio-native");            // libraries/factorio-native
    for (std::StringView lib : {"SDL3/include", "SDL_Mixer/include", "Lua", "", "curl/include"})
        project.cIncludes.add(path(factorio, "libraries", lib));
    project.configuration("fastdebug", "debug").optimize = true;

    Executable& game = project.executable("factorio", {"src"});
    game.steps.add(Step::copy(game.output, path(project.setting("GAME", "C:/factorio-in-kpp"), "bin/x64")));

    Executable& tests = project.executable("factorio-test", {"src", "tests"});
    tests.exclude("src/Main.kpp");
    tests.use("unit-test");
}
```

- **The command line.** In a directory with build.kpp, or below one, the compiler with no
  sources builds the project:

  ```
  kpp                     every target, debug
  kpp release             every target, release
  kpp factorio fastdebug  one cell: a target in a configuration
  kpp factorio --run      build it, then run it
  kpp --test              build and run the tests
  ```

  debug is the default and may be written out (`--debug` and `--release` say the same). `kpp` is the name kovarex used, the
  compiler's own since 2026-11-07 (question 2: "kpp everywhere"). A file or directory given on the command line still means what it
  means today: build these sources, with no build.kpp.
- **Targets.** `project.executable(name, roots)` is one program. It gets the project's settings and
  libraries, plus its own. Each target is a whole program: two executables from one `src/` share
  the front end through the server, but each generates its own code (a cold build of both is
  about twice one). `exclude` leaves files out, so each program has exactly one `main`.
- **Configurations.** `debug` and `release` are built in. `project.configuration("asan", "debug")`
  adds one based on another ("Targets and configurations", below).
- **Steps** are for what is not compiling kpp or C: generating code, copying into a game
  directory. A step is a built-in (`Step::copy`) or a kpp program in the project
  (`Step::run("tools/generate_imgui.kpp", inputs, outputs)`). kpp compiles and runs it with the
  full standard library, and only when its inputs changed. Only the root project declares steps:
  a library's build.kpp may not, so adding a library runs none of its code on your machine
  (library-registry.md, decision 5).
- **With no build.kpp** the project is the defaults: the directory's name, one executable from
  `src/`, debug and release, no libraries. `kpp new hello` writes `src/main.kpp` and a
  `.gitignore` with `build/`.

### Version 0, working now

`compiler/src/Project/` (project.cpp until 2026-11-05) and `projectlib/Project.kpp` (2026-10-31); `tests/project.ps1` in
run_tests.cmd.
- **The commands.** In a directory with build.kpp, or below one, `kpp` takes executables' names,
  not sources:
  - `kpp [targets] [configurations]` builds those cells, every target when none is named, debug
    when no configuration is;
  - `kpp NAME --run ARGS` builds and runs one; one built for another platform (`--target freebsd`,
    linux) is an error before the build, as `kpp --run`'s: build it without `--run`;
  - `kpp --run FILE.kpp ARGS`, with no target or configuration named before `--run`, is the one
    exception to names: it runs that file with ARGS, as outside a project, and a FILE.kpp that is not
    there is an error (2026-11-06: it built and ran the project, with the file among its arguments,
    so one command ran another program depending on the directory). Any other first argument
    (`kpp --run input.txt`, a directory) goes to the project's program, and `kpp NAME --run FILE.kpp`
    gives it a .kpp file;
  - `--generate-visual-studio-files` and `--generate-vs-code-files` write one solution for all
    executables, whose builds run `kpp NAME CONFIGURATION --msvc-diagnostics`, so an edit of build.kpp
    needs no new project files;
  - `--set NAME=VALUE` gives a setting; `--no-server` builds without the compile server, and
    `--stop-server` stops the project's servers, every one whose arguments name a file of the
    project (a build with options of its own, `--verbose` or another `--toolchain`, has a server of
    its own), and says which it stopped; `--verbose` shows kpp's report of each build (its
    phases' times), which a project's builds otherwise keep quiet;
  - `--rebuild` builds the cells asked for from scratch, and `--clean` only takes their kpp build
    state away (kovarex 2026-11-08: Visual Studio's Rebuild only built, its Clean deleted the
    executable alone, so the objects and the compile server's kept build survived both): every
    compile server whose build writes the cell's executable stopped, then the executable and what kpp
    and the link write beside it deleted: under its name (`EXE.N.o` and `.hash`, the modules'
    objects; `.ll`; `.link.pending`, `.command`, `.inputs` and the response files; `.debug`) and
    under its stem (`.pdb`, `.ilk`, `.natvis`, `.lib`, `.exp`). It says how many files went. The C
    libraries' objects stay (`build\clib\`, shared by every cell: a Rebuild of one Visual Studio
    project rebuilds no other project, and `kpp measure`'s clean build keeps them too), and so does
    the opt-in shared object cache (KPP_OBJECT_CACHE: keyed by all an object is made of, the compiler
    included, so it gives a rebuild nothing stale);
  - `--check` runs kpp's check of each cell (its front end, which writes no executable) and nothing
    else: no C library compiles, no link, no after-build step; it says `kpp: checked NAME
    (CONFIGURATION, ...)` and exits 0 when the check found no error (`kpp factorio --check` said
    "factorio.exe was not written" after 0 errors before, 2026-11-08);
  - `kpp` alone in a console shows a menu of these, each with its command, and so in Git Bash
    (mintty) and Cygwin, whose terminals give programs a pipe, not a console (its name,
    `\msys-...-pty0-from-master`, says it is one; 2026-11-04). With input from a file or another
    pipe, it lists the commands, says it asked nothing, and exits with 2. `kpp new [DIR]` writes
    build.kpp, `src\main.kpp` and a `.gitignore`.
  An argument that is an existing file or directory makes it a plain build of those sources, as
  before.
- **How build.kpp runs.** kpp runs `kpp --run-build matrix` on build.kpp, projectlib and a
  generated `build\kpp\settings.kpp` (the `--set` values and the project's name), through the
  compile server. The interpreter (GlobalScope::runBuild) default-constructs a Project, runs
  build(project) and then projectlib's `String describe(const Project&)` (which runs configure()
  for every cell), and
  kpp prints that text, which kpp reads. Nothing is linked or run. An I/O call in build() is an
  error naming it: `build.kpp:4: a call to puts, a C function, which has no body to run at
  compile time`. The result is kept, keyed by build.kpp, projectlib, the settings and kpp itself.
- **projectlib** (`projectlib/Project.kpp`). Lists are `Vector<String>`, paths join with `+`
  (`project.cInclude(project.setting("ROOT", "C:/x") + "/include")`), and executables and C
  libraries are Deques, so the reference `executable()` returns stays valid. Commands are set by
  methods: `game.afterBuild("copy ...")`, `game.debug(command, arguments, directory)`,
  `library.standard("c++20")`. `{exe}` (the configuration's executable) and `{configuration}` are
  expanded in the steps, and in debug()'s command, arguments and directory as well (2026-11-02).
  `kpp NAME --run`, F5 in the generated Visual Studio project and VS Code's launch.json then start
  each configuration's own program: `game.debug("copies/{configuration}/game.exe")`. A name per
  configuration that is not the configuration's own is made in configure(Cell&), below. Factorio's
  `bin\Releasex64` is one, made as `"bin/" + name + "/factorio.exe"` from
  `cell.configuration.name`. tests/project.ps1 checks both.
- **Measured** on a `kpp new` project (2026-10-31, PC-PRACOVNA): the first `kpp --debug` took
  1.9 s (compiling build.kpp and the program); with nothing changed it took 0.2 s.
- **What version 0 lacks:**
  - Steps are `beforeBuild` and `afterBuild` commands of an executable, run by cmd in the
    project's directory.
  - `use()` finds libraries in directories only (below); the registry, build.lock and `kpp add`
    do not exist yet.

### kpp libraries (working now, 2026-10-31)

**A library is a project without an executable.** Its build.kpp declares it with
`project.library(NAME)` instead of `project.executable(...)`, and its sources join the program of
every executable that uses it. Tested by `tests/project.ps1`.

```cpp
// libraries/json/build.kpp
void build(Project& project)
{
  Library& json = project.library("json");
  json.version = "0.1.0";        // major.minor.patch: required
  json.license = "Apache-2.0 WITH LLVM-exception"; // SPDX
  json.minimumKpp = "0.1.0";     // optional: the lowest kpp it compiles with
  json.root("src");              // its sources; none given: src
  json.test("tests");            // sources only its own tests build
  json.use("base64");            // a library it uses
}
```

- **The fields** are the registry's manifest (library-registry.md, "What a library is"): `name`
  (from `library()`), `version`, `license`, `minimumKpp`, the source roots and the test roots.
  `version` must be `major.minor.patch`. kpp compares `minimumKpp` with its own version ("Versions"
  below: `kpp --version`'s; a build with changes, 0.2.N+dirty, counts as 0.2.N) and refuses a
  library that needs a newer one. A kpp built outside a git checkout has no version: a library with a
  `minimumKpp` is then an error, not a guess. (Until 2026-11-03 the version compared was a constant,
  0.1.0, so every library needing 0.2 was refused.)
- **Using one:** `project.use("json")` for every executable, `game.use("json")` for one, and
  `json.use("base64")` in a library. `use("json", "0.2")` also states the lowest version that will
  do; a lower one is an error. `use("json", local("../json"))` gives the library's directory,
  relative to the build.kpp that says it.
- **Where `use("json")` looks,** in order:
  1. the project's `libraries/json` (the root project's, for the whole graph: so it is also the
     override of a library a library uses, and of the registry's later);
  2. for a library in a directory named `libraries`, next to it (`libraries/base64` for
     `libraries/json`), so the libraries of one `libraries/` directory use each other and each
     builds and tests on its own;
  3. kpp's own libraries: `libraries\` beside `stdlib\`, a release's (the official libraries,
     this release's versions: so the installed kpp alone builds a project that uses httpclient,
     with no checkout; kovarex, 2026-11-04), or a checkout's for a build of kpp. `kpp --version`
     names the directory and its libraries (`libraries: C:\Program Files\kpp\libraries (base64,
     bcrypt, ...)`). The project's own copy, by 1 or by `local(...)`, comes first;
  4. later, the registry; now a missing library is an error naming where it looked.
- **What kpp does with it:** it describes the library's build.kpp (the same configuration and
  `--set` settings; the description is kept in the using project's `build\kpp\uses\NAME`, never in
  the library's directory, which may be read-only: an installed kpp's libraries, under
  `C:\Program Files\kpp`, failed every project that used one with no word, 2026-11-05; a file kpp
  cannot write is an error that names it), checks the version and minimumKpp, gives kpp its roots as the library's, with
  its scope ("Library scopes" below), and does the same for the libraries it uses, depth first. A
  library rooted at its own directory (`root(".")`, a converted C library's) keeps its build.kpp out
  of its sources (kpp passes `--exclude`); the cell's kpp options apply to its roots too, so a cell's
  `--ext .kpp` leaves out the C headers beside a converted library's kpp. One name in two directories is an error: a
  program has one copy of each library. Only the library part of its build.kpp is read: its
  executables and their commands are never built or run, and C libraries (`cLibrary`) in a used
  library are an error until they are supported.
- **System libraries** (2026-11-04, for httpclient's OpenSSL on FreeBSD): `lib.systemLibrary("ssl")`
  is linked by every program that uses the library, its own tests included, as
  `Executable::systemLibrary` is for one executable (kpp's `--lib -lssl`). With a platform,
  `systemLibrary("ssl", Platform::FreeBSD)`, only a build for that platform (its `--target`, from
  build.kpp's arguments or the command line; windows without one) links it. Each name is linked once
  however many libraries name it. Tested by `tests/project.ps1` (a library linking version.dll for
  Windows, and a library no platform has for FreeBSD, left out; with FreeBSD's sysroot beside kpp,
  `kpp --target freebsd` fails to link that one and leaves out the same library for Linux).
  An executable's own takes a platform the same way (`app.systemLibrary("ssl", Platform::FreeBSD)`,
  2026-11-05), and is linked once with its libraries' of the same name.
- **A program's own .natvis** (thread F, 2026-11-05): `game.natvis("tools/factorio.natvis")`, a file
  relative to the project with Visual Studio's display rules for the program's types, is linked into
  the PDB of a `--debug-info` build for Windows beside the rules kpp writes for the standard library
  (kpp's `--natvis FILE`; link.exe's or lld-link's `/NATVIS`, lld's MinGW driver's `--Xlink`).
  Visual Studio finds it with the program (debugging.md, "Values in Visual Studio"). A missing file
  fails the link. Tested by `tests/natvis.ps1` (natvis\project). A change of the file alone links
  again: kpp keeps the stamps (size and time) of the link's inputs besides kpp's objects, the
  .natvis files and the libraries named by path (`linkFile`), in `<exe>.link.inputs`, and links when
  one differs. Before, a build through the compile server, which replays an unchanged build, kept the
  old rules in the PDB (project.ps1's linkinputs part).
- **`kpp --test [names] [-- runner arguments]`** builds and runs the tests (design.md, "Native
  tests") of the executables and libraries named, all when none is: each one's roots, a
  library's test roots too, through `kpp --test` with its build's options. `kpp --test -- --filter
  "json.kpp > parse"` runs some. The exit code is 1 when a test failed. The `test` blocks in the
  source roots of the libraries a program uses are in its test build too, so a library keeps its
  tests in its test roots when they should not run in its users' tests.
- **In an editor** (`kpp --lsp`), a library project is checked as its sources and test sources,
  with its `test` blocks.
- **A library's own build** (`kpp` in its directory) builds nothing: it has no executable.

`Project::library(path)`, the C link file, is now `Project::linkFile(path)` (and
`Executable::linkFile`; `Executable::library` stays its older name until factorio-in-kpp's
build.kpp moves).

#### Library options (decided 2026-11-01, working now)

kovarex, 2026-11-01: "when I add new library to my project, I should have some clear way to get
noticed of what config values should be provided to the library, with defaults or something."

```cpp
// libraries/zlib/build.kpp
Library& zlib = project.library("zlib");
zlib.option("debug", false, "Extra consistency checks (ZLIB_DEBUG)");   // bool, its default
zlib.option("window", 15, "Window bits");                               // int
zlib.option("strategy", "default", "Compression strategy", {"default", "filtered", "huffman"});
zlib.requiredOption("level", "int", "Compression level, 0 to 9");       // no default: users must set it

// the project's build.kpp
project.use("zlib").config("level", 6);              // for whatever uses it here
project.configuration("debug").config("zlib::debug", true);
```

- **A library declares its options** in its build.kpp, each with a type (bool, int, string), a
  default and a description. A string option may list its values. An option with no default
  (`requiredOption`) is required.
- **The library's code reads `config::zlib::debug`,** its own scope, so libraries never collide
  with each other or with the program's constants. It is an ordinary `config::` constant: in
  expressions, `if constexpr` and `conditional(...)`, at declaration scope too (kpp's
  `--config zlib::debug=true`; tests/spec/kpp/config_library_scope).
- **The user sets one** with `use("zlib").config(NAME, VALUE)`, or as a constant named
  `zlib::NAME` of the project, a configuration, a target or a cell (`config("zlib::debug", true)`),
  or with `kpp --config zlib::debug=true`. The later one wins, in that order, over the default.
- **No silent fallbacks.** These are errors, each naming the build.kpp, the cell and who set the
  value:
  - a required option nothing sets (the message names the option, its type, its description and
    the three ways to set it);
  - an option the library does not declare (the message lists the ones it does);
  - a `zlib::` constant when no target uses zlib;
  - a value not of its type, or not among a string option's values.
  They are checked for every cell whenever kpp reads the description: building, listing or
  checking in an editor.
- **`kpp options [targets] [configurations]`** lists each cell's library options: the value, the
  type, the default (or "required"), who set it ("the default", `use("zlib") in ...build.kpp`,
  "the configuration release", "the command line (--config)") and the description. The language
  server gets the same per cell (`ProjectCell::options`, project.hpp), and the registry lists a
  release's options (library-registry.md).
- **A library's own tests** (`kpp --test` in its directory) build with its defaults; a required
  option needs `--config` there.
- **Interpreter gaps found** (pending specs for thread D; projectlib is written around them):
  `language/consteval_returned_deque_reference_chained` (so each `use()` and `option()` emplaces in
  itself) and `language/consteval_member_call_on_subscript_result`. Two calls chained on use()'s
  result, `use("zlib").config("a", 1).config("b", 2)`, still fail at compile time ("memory ... is
  never freed"), not yet reduced to a snippet: write one `config()` per statement until D's fix.

#### config:: defaults in source (decided 2026-11-04, working now)

```cpp
// libraries/zlib/src/Deflate.kpp
// How hard deflate tries, 1 to 9.
config i32 level = 6;
config bool checked;          // no default: its users must set it

// the program's own, src/BuildConstants.kpp
// Leaves out the developer-only code, as Factorio's FinalRelease build.
config bool finalRelease = false;
```

- **A declaration** stands at a file's top level, first on its line: `config TYPE NAME = DEFAULT;`,
  or `config TYPE NAME;` for a required one. Types: bool, the integer types (i8 to u64, int,
  unsigned...; the default must fit), StringView (a plain `"..."` default) and an enumeration
  (`Platform`, `zlib::Mode`: named as from the global scope, the default `TYPE::NAME`). The `//`
  lines right above it are its description.
- **Its name** is `config::NAME` in the program, `config::LIB::NAME` in a library (its options'
  scope). The constant has the declared type.
- **Who wins:** its default is the lowest value. A library's are its options, as build.kpp's
  `option()`s are: set by `use("zlib").config(...)`, a `zlib::NAME` constant of the project, a
  configuration, a target or a cell, or `kpp --config zlib::level=9`, and listed by `kpp options`
  ("set by the default", with the declaration's FILE:LINE). The program's are set by its own
  constants of build.kpp and `--config`, as before. kpp alone takes them too: `kpp --exe a.exe
  src` with no `--config` builds with the defaults.
- **Errors**, each at the declaration: a required one nothing sets; two declarations of one name
  that differ (both named); a value of another type (`--config level=x`, an i8's 300, a bool's 1);
  `debug`, `platform`, `mingw` or `tests` (kpp's own); a library's name declared both in source and
  by `option()`; a malformed declaration, one not first on its line, and one anywhere but a file's
  top level. A top-level `config` followed by a name is always a declaration.
- **How:** kpp reads the declarations from the tokens of the files that may hold one (src/config.cpp:
  a text check picks them, the lexer decides, so a commented-out one is not one), before any file is
  parsed, and writes them into `<config>` with their types; the parser drops their tokens. A compile
  server's build whose `<config>` changed (an edited default) is not incremental. kpp reads a used
  library's declarations the same way from its roots.
- **Open:** whether these replace build.kpp's `option()` (both work today).

#### Library scopes (decided 2026-11-01, design.md 16; the lookup is not in kpp yet)

kovarex, 2026-11-01: "adding library means that I have to change unrelated code to remove
ambiguity is not nice"; "libraries can decide".

```cpp
// libraries/zlib/build.kpp: scoped, the default
Library& zlib = project.library("zlib");   // its users write zlib::compress, zlib::Stream

// libraries/util/build.kpp: a project's own foundation library
Library& util = project.library("util");
util.unscoped = true;                       // its names are bare everywhere, as the stdlib's
```

- **Scoped by default:** a program or another library names a library's declarations through its
  name, `zlib::compress`; inside the library (its roots and its test roots) they are bare. The
  name is the qualifier, so it must be an identifier: `project.library("te-xt")` is an error, and so
  is a library name that is also a C++ namespace or a top-level name of the program (`zlib::x` would
  be ambiguous; thread A's check).
- **`lib.thisReference = true`:** the library's sources and tests are written with `this` the object
  (`this.x`, `&this`, [design 23d](design.md)), whoever uses it; a program that keeps the pointer can use
  it. Since 2026-11-10 that is the only meaning: accepted, no change (design 23d).
- **`lib.unscoped = true`:** its names are reachable only bare, never as `NAME::x` (its name is for
  diagnostics only), and share the one global scope with the
  program, the other unscoped libraries and the C imports; a clash is a compile error. The
  standard library is unscoped. Meant for a project's own libraries and foundations; the registry
  shows it on the library's page.
- **Adding a scoped library never changes existing code's meaning.** There is no per-file
  `using`: a name means the same in every file.
- **What a project build gives its compile,** per root of each library a cell uses, in resolveUses' order:
  `--library-root NAME DIR` (scoped) or `--unscoped-library-root NAME DIR` (kpp's options, thread
  A's lookup); the program's own roots stay plain arguments, and the stdlib, which kpp adds itself,
  is unscoped. A library's own tests (`kpp --test` in its directory) build its roots and test roots
  as the program, so its names are bare there; the libraries it uses come as library roots. Visual
  Studio's project lists the libraries' files too. Tested by tests/project.ps1 (a bare use of a
  scoped library's function is "did you mean greet::greeting").
- **`kpp options`** lists each cell's libraries, `library zlib, scoped: zlib::NAME` or `library
  util, unscoped: its names are bare`, before their options; the language server gets the same
  (`ProjectCell::libraries`, project.hpp; `libraries` in `kpp/cells`, editors.md).
- **Not yet:** the converted C libraries (zlib, ogg, ...) are still roots of the game in
  factorio-in-kpp's build.kpp, so their names are bare; each becomes a scoped library of its name,
  with the converter qualifying the game's calls (thread C).

### C and C++ libraries (proposed 2026-10-31, not implemented)

The case: factorio-in-kpp (C's layout, root-relative) links 18 C/C++ libraries that today come
prebuilt from C:\Factorio\vs2022\Releasex64, which a clone does not have, plus the shims in
libraries/kpp-native. Whether build.kpp builds them from libraries/ or the repository carries
.lib files is kovarex's choice. A C library is therefore one kind of target, buildable either
way, and an executable links it by name:

```cpp
void build(Project& project)
{
  CLibrary& zlib = project.cLibrary("zlib");
  if (project.setting("PREBUILT", "").size() != 0)
    zlib.prebuilt(project.setting("PREBUILT", ""), "/zlib.lib");   // a .lib (or several) as given
  else
  {
    zlib.source("libraries/zlib");             // every .c/.cpp under it, or files one by one
    zlib.include("libraries/zlib");            // its own include directories
    zlib.define("ZLIB_CONST");
  }
  CLibrary& native = project.cLibrary("kpp-native");
  native.source("libraries/kpp-native");
  native.include("libraries/imgui");
  native.language("c++17");                    // c11, c17, c++17, c++20
  native.resource("libraries/kpp-native/factorio.rc");

  Executable& game = project.executable("factorio", {"src", "libraries/CommonUtil" /* ... */});
  game.link("zlib");
  game.link("kpp-native");
  game.systemLibrary("gdi32");
}
```

- **Building one.** kpp compiles each source with the clang it already uses (clang-cl driver
  mode), in parallel, with the library's includes, defines and language. It caches each object
  by the hash of its source, its flags and the headers clang reports (-MD). It archives the
  objects into `build\<configuration>\<name>.lib` with llvm-lib, again whenever an object was
  compiled or the objects are not the ones the archive was made of (`objects.rsp` beside it: a source
  removed or excluded with nothing else to compile left its object in the archive, where the link
  could still take it, until 2026-11-08). A library's error is reported at its file and line in the
  same Error List.
- **Its configuration.** C libraries are compiled as the game's prebuilt ones are, /MT (one CRT is
  linked) and NDEBUG; an optimized configuration's at /O2, an unoptimized one's (debug) at /Od
  since 2026-11-07 (kovarex: "debug builds means everything debug", as Factorio's C++ Debug build
  compiles its libraries; libraryFlags in Project/CLibraryBuild.cpp), each in a directory of its
  flags' own.
- **The flags are a short list:** include, define, language, `warning(off)`. There is no
  free-form flag, so a library's build stays readable and the registry (library-registry.md, "C
  dependencies") can take the same list.
- **Factorio's libraries** are `cLibrary` entries in factorio-in-kpp's build.kpp, whose sources and
  defines come from fbuild.bff's Library() sections: 18 at first (709fbd7f), 13 now, as a library
  converted to kpp leaves the list for the game's roots.
- **Linux and FreeBSD (thread B, 2026-11-05).** A program built for another platform (its
  `--target`, build.kpp's or the command line's) has its C libraries built for that platform: clang
  for the target's triple with the target's sysroot (the program's `--sysroot`, else the one beside
  kpp), `-c -O2 -DNDEBUG -w -fno-stack-protector`, ELF objects and an archive llvm-ar makes,
  `build\clib-linux\NAME.a` (`build\clib-freebsd\...`), so that building for one platform leaves
  another's objects as they are. Every entry of a library may name its platform, as
  `systemLibrary(NAME, PLATFORM)` does: `source`, `exclude`, `include`, `define`,
  `prebuiltLibrary` and `resource` with a `Platform` second argument count only in a build for that
  platform (`numbers.define("PLATFORM_ID=2", Platform::Linux);`, kept as "linux PLATFORM_ID=2",
  compiler/src/Project/CLibraryBuild.cpp's forPlatform), and an entry without one counts on every
  platform. So does an include directory of the C import, an executable's or the project's
  `cInclude` (kpp's `--cinclude`): `game.cInclude("libraries/libbacktrace", Platform::Linux)` for
  the import only Linux's files have (2026-11-05). An include directory that starts with `=` is in the target's sysroot, as GCC's `-I=`
  (which Visual Studio's clang does not take with a Windows sysroot path): SDL3's
  `include("=/usr/include/dbus-1.0", Platform::Linux)`; in a build for Windows it is an error. A
  resource script in a build for Linux or FreeBSD is an error (only a Windows program links a
  .res). A library with C++ files links its C++ library, which an ELF object does not name
  as a COFF one names MSVC's: libstdc++ statically for Linux (`-l:libstdc++.a`, as Factorio's Linux
  build links it: the program needs no particular libstdc++ of the system it runs on), libc++ and
  libcxxrt for FreeBSD. The Linux sysroot carries libstdc++'s headers and libraries for it
  (tools/sysroot/linux.ps1). Test: tests/targets.ps1's C library project (tests/targets/clibrary,
  a C file, a C++ file and a define per platform), built and run for Windows, built and run under
  WSL for Linux, and built and linked statically for FreeBSD: `42 windows 10`, `42 linux 10`.
- **A precompiled header (2026-11-07, thread E, from B's whole-build profile: the C libraries were 214
  s of the whole build's 507 s of CPU, SDL 118 s of it, mostly Windows' and SDL's headers).**
  `sdl.precompiledHeader("SDL_internal.h")`: the header, the file the library's include directories
  give, is compiled once per language its sources are in (C, C++), and every source whose first
  directive is `#include` of it as written (comments and blank lines before it allowed) uses it:
  clang-cl's `/Yc` and `/Yu` in MSVC's dialect, `-include-pch` in GNU's (windows-gnu, Linux, FreeBSD).
  A source that includes something else first, defines a macro first, or has a file of that name
  beside it (which its `#include "..."` would find first) compiles as it is. The precompiled headers
  are built first in the libraries' pool, a source that uses one waits for it, and a header rebuilt
  (one of the files it read changed) recompiles every source that uses it. **The objects are the
  same:** the game's 422 SDL and curl objects compiled with it equal those compiled without but for
  the COFF header's time stamp and, in 2 of them, the order of two inline functions of the C
  runtime's headers; the game's release factorio.exe linked from them is the same but for the link's
  time stamps. Measured (a project of the game's sdl and curl entries as its build.kpp has them,
  `python cpu.py`'s job object, two interleaved rounds): 170 and 174 s of CPU without, 94 and 95.5 s
  with. Tests: tests/project.ps1's pch section; tests/targets/clibrary's numbers library uses one on
  every platform (targets.ps1, windowsgnu.ps1, CI's Linux host).

### Adding a library

**Writing the line adds the library.** `project.use("json")` (or `game.use(...)` for one target)
is the whole act. On the next build kpp does the following:
1. It evaluates build.kpp for every configuration and collects the `use` calls.
2. It finds each library:
   - first in `libraries/<name>`;
   - then in the shared cache;
   - then in the registry, downloading it into the cache.
3. It picks the versions (minimal version selection) and checks the libraries' top-level names
   for clashes.
4. It records each library's exact version and hash in `build.lock`, and prints
   `added json 1.4.2 (sha256 3f9c...) to build.lock`.

So there is no separate `kpp add` that has to edit a program. Clicking is a convenience on top of
the same line: the IDE or `kpp add json` inserts `project.use("json");` after the project's
last `use` call. kpp parses kpp code, so it knows where that is.

- **Versions.**
  - `use("json")` takes the newest release the first time, and build.lock pins it from then on.
  - `use("json", "1.4")` states a minimum.
  - `kpp update [json]` moves the lock forward.
  - A build never changes a pinned version on its own.
- **Other sources:** `use("glm", git("https://github.com/...", "4c1f2e0"))` or
  `use("lua", local("../lua"))`.
- **Which project gets it.** The build.kpp you write it in. A library's own `use` calls come with
  it. One version of each library per program (the global scope).
- **Forks.** A library's name resolves to `libraries/<name>` before the registry, so a fork changes
  no `use` line.
  - `kpp fork json` copies the locked version there and records where it came from. From that
    record, kpp can show the fork's diff and say when upstream has a newer release.
  - Deleting the directory goes back to the registry's version.
- **Safety.**
  - Downloading on a build runs nothing: a library's build.kpp is interpreted and declares no
    steps, and its code runs only when the program does.
  - A typo (`use("jsno")`) fails as an unknown name, or downloads the wrong library, which the
    printed line and the lock's diff show in review.
  - `kpp --locked`, for continuous integration, refuses any change to build.lock.
- **Offline.** A library missing from the cache is an error that names it.

### The layout

```
factorio/
  build.kpp          optional
  build.lock         written by kpp when there are libraries; committed
  src/               the project's code
  tests/             more source roots, as build.kpp names them
  libraries/         libraries kept in this repository: your own, and forks; committed
    factorio-native/
      build.kpp      a library: project.library(...) instead of executable
      src/
  build/             outputs, never committed: build/<configuration>/<target>.exe
```

- **Registry libraries are not in the tree.** They sit in a read-only shared cache
  (`%LOCALAPPDATA%\kpp\libraries\<name>-<version>-<hash>\`), pinned in build.lock, so they are 1:1
  with what the registry serves.
- **The IDE's package tree** shows *Program*, *Libraries* (own, and forks marked as such),
  *Registry* (read-only) and *Standard Library*.
- **One-file programs** stay one command: `kpp --run hello.kpp` needs no directory, and means the
  same in a project's directory.

### Targets and configurations (decided 2026-11-01)

kovarex, 2026-11-01:
- "One axis is built targets, libraries etc. Second axis is configuration, aka debug, relase, fast
  relase, valgrind etc. Each named configuration contains a list of config constant defining it
  (mainly). When selecting what to build, we select target + configuration, like release tests, or
  debug main game etc. When it comes to caching, each configuration needs to be separated in the
  compiler... In visual studio, you have a dropdwon where you select these targets, and which is
  the context of editing the code (for intellisense, and your language server), and also for what
  we build."
- "Consider, that we might want to do things for some specific configurations, like for example.
  If configuration is SteamRelease and BuildTarget is FactorioMain: do something special (like
  custom config, or eventually some other code which can do something)".

**The rule.** A project is a matrix: its targets (executables) are one axis and its named
configurations the other. One cell, a target in a configuration, is what is built.

```cpp
void build(Project& project)
{
  project.config("space_age", true);                    // every cell
  Configuration& release = project.configuration("release");
  release.config("packaged_build", true);
  Configuration& fast = project.configuration("fastdebug", "debug"); // based on debug
  fast.optimize = true;                                  // asserts in, optimized
  Executable& game = project.executable("factorio", {"src"});
  Executable& tests = project.executable("factorio-test", {"src", "tests"});
  tests.config("unit_tests", true);                      // this target, every configuration
}

// Optional: once per cell, after build().
void configure(Cell& cell)
{
  if (cell.configuration.name == "steam-release" && cell.target.name == "factorio")
    cell.config("steam", true);
}
```

- **build() runs once** and declares the whole matrix as data. Project has no `configuration`
  member any more: what differs by configuration is in the configuration, or in configure().
- **Configurations.** `debug` and `release` are built in. `project.configuration(NAME)` returns
  one (made if new, with no base: not optimized, no debug info, `config::debug` true as kpp's
  default), `project.configuration(NAME, BASE)` makes one based on another: the base's
  settings come first, then its own, resolved after build() (a change to debug later in build()
  reaches fastdebug too). A configuration holds:
  - `config(NAME, VALUE)`: a `config::` constant (bool, integer or string);
  - `optimize`: clang -O2 and ThinLTO and a whole link (kpp `--optimize`), or -O0 and an
    incremental link;
  - `debugInfo`: line tables and a PDB (Linux and FreeBSD: DWARF; an optimized build's in NAME.debug beside the executable, which keeps a .gnu_debuglink to it);
  - `clangFlag(F)` (kpp `--clang-flags`) and `argument(OPTION)` (any kpp option);
  - `sanitize(NAME)`: a sanitizer, `address`, `thread`, `undefined` or `overflow` (kpp `--sanitize`, "Sanitizers" above);
  - `cFlag(F)`: one more flag for its C libraries' compiles, after kpp's own (clang-cl's, or clang's
    for windows-gnu). A configuration with C flags builds its C libraries in a directory of those
    flags' own, `build\clib\flags-HASH`, which configurations with the same flags share: building it
    leaves the others' objects as they are. With `-flto` its C libraries join a full LTO, at one
    partition ("Whole-program optimization" below).

  The built-in two (thread H's contents, adopted):

  | | debug | release |
  |---|---|---|
  | `config::debug`, so `assert` | true: asserts in | false: `assert` compiles to nothing, `release_assert` stays |
  | `optimize` | false: `-O0`, incremental link | true: `-O2` + ThinLTO, lld, a whole link |
  | `debugInfo` | true | true: line tables in a file beside the executable, the PDB (Linux, FreeBSD: NAME.debug), which it names without its directory; its code unchanged (kovarex 2026-11-07, question 1) |
  | kpp trivial-function inlining | on | on |
  | its C libraries | unoptimized: `/Od` (`-O0` for clang's GNU driver) after kpp's `/O2`, in `build\clib\flags-HASH` (kovarex 2026-11-07: "debug builds means everything debug", as Factorio's C++ Debug build compiles its libraries) | `/O2`, in `build\clib` |
- **config constants** are set for the project (`project.config`), for a configuration, for a
  target (`tests.config`) and for a cell (configure()); each later one overrides: project,
  configuration, target, cell. The command line's `kpp --config NAME=VALUE` overrides them all.
  They reach the program as kpp's `--config NAME=VALUE`, so the program reads `config::NAME`.
- **configure(Cell& cell)** is optional. The interpreter runs it once per cell after build(), on
  a copy of the cell's target and configuration: `cell.target` and `cell.configuration` are
  readable and changeable for that cell only (`cell.target.linkFile(...)`,
  `cell.target.afterBuild(...)`, `cell.configuration.clangFlag(...)`), and `cell.config(NAME, VALUE)`
  sets a constant for the cell. It still cannot act. `cell.platform` is the platform the cell is
  built for (`Platform::Windows`, `Linux`, `FreeBSD`): the last `--target` of the project's, the
  configuration's and the target's arguments, the command line's over them all (`kpp --target linux`,
  which kpp gives build.kpp in build\kpp\settings.kpp, so the description is computed again when it
  changes), Windows without one. A step only one platform can run is chosen by it, as the game's
  cmd steps (mklink, config.ini) are Windows': `if (cell.platform == Platform::Windows)
  cell.target.afterBuild(...)`. describe.txt keeps it (`platform linux`), and kpp checks it against
  the platform it builds the cell for: a `--target` that configure(cell) adds itself comes too late
  for `cell.platform`, and that build stops before it starts, never a Linux build that runs Windows'
  steps (2026-11-05, for the game's Linux build). `cell.unavailable(REASON)` says the cell does not
  exist: `kpp TARGET CONFIGURATION` is the error `TARGET has no CONFIGURATION configuration: REASON`
  (exit code 2) instead of a build, a build of every target leaves it out with the notice `kpp:
  TARGET (CONFIGURATION) left out: REASON`, and the target's Visual Studio project has no such
  configuration (2026-11-10: factorio-test in final, whose code FinalRelease leaves out, as Factorio's
  own build has no FinalRelease of its tests). describe.txt keeps it (`unavailable REASON`). A build.kpp without one gets projectlib's,
  a template that does nothing (a function is chosen over a template). It runs for every cell, not
  lazily for the ones built: the description is computed once and kept (above), the Visual Studio
  files and the language server need every cell, and a cell costs little. Measured 2026-11-01 on
  factorio-in-kpp's build.kpp (`kpp --quiet --run-build matrix projectlib build.kpp
  build\kpp\settings.kpp`, 15 interleaved rounds, the machine shared with other builds): 272 ms at
  best with 4 cells (2 targets, debug and release), 299 ms with 6 (fastdebug added).
- **The command line.** `kpp TARGET CONFIGURATION`, in either order: `kpp factorio-test release`.
  A name is a configuration's or a target's, never both (a description naming both is an error).
  No target: every target; no configuration: debug. Several names build every cell they make;
  `--debug` and `--release` still mean `debug` and `release`. `kpp --test [names]` runs the tests
  of those targets and libraries in those configurations. A name that is also an existing file or
  directory is still a plain build of those sources, so a target is best not named like a
  directory (`tests`).
- **Outputs** go to `build/<configuration>/<target>.exe` (`build/<configuration>/<target>` for Linux and
  FreeBSD, whose programs have no extension), with kpp's object cache beside it. A cell's
  kpp arguments differ from every other cell's (its configuration's options and constants, its
  output), so each has its own compile server and its own cache: switching from debug to
  fastdebug and back rebuilds nothing.
- **Visual Studio** gets one project per target and one solution configuration per named
  configuration: the dropdown chooses the configuration, the startup project the target, and
  Build builds that cell (`kpp TARGET CONFIGURATION --msvc-diagnostics`). A configuration's
  Visual Studio name is its own with a capital first letter (Debug, Fastdebug). The language server
  checks the code as the chosen cell compiles it (projectCheckArguments takes the configuration by
  either name, and the target; the editor's side is thread F's).
- **The converted game's defines** (_DEBUG, LUA_USE_APICHECK) are still the conversion's: the
  converter preprocesses with them, so they are not a configuration of kpp's. A configuration's
  `config::` constants are a kpp program's own. factorio-in-kpp's build.kpp has debug, release and
  fastdebug.
- **Interpreter gaps found** (pending specs, worked around in projectlib):
  `language/consteval_struct_copy_assignment` (a struct with a String copied by `=` or a member
  initializer frees twice), `language/consteval_string_assigned_through_reference` (String's
  `this != &other` across frames), `language/consteval_emplace_back_result_as_reference_argument`.

### What it needs, in order

1. **Allocation in the interpreter.** `new` and `delete` are not evaluated today (C++20's
   transient allocation), so a build file could not use `Vector` or `String`. This is thread D's
   work.
2. **The `build` package** in the standard library (thread C's).
3. **The driver** (B's and E's): evaluate build.kpp, read the `Project` back, and build each
   target. Also the command line above and `kpp new`, which find build.kpp upward from the current
   directory.
4. **`use` resolution**: `libraries/` and local paths first (no registry needed), then the cache
   and the registry with library-registry.md's phase 1.
5. **Project files for an IDE** (below) are generated from build.kpp's targets, and
   `tools/vs/factorio/generate.cmd` disappears into factorio's build.kpp.

### The whole build (2026-11-02)

A program's build is its C libraries and kpp's work at once, then the link:
- Every library the program links has its changed files (and resources) compiled in **one pool**
  on all cores, no library waiting for another, while kpp builds the program (its front end,
  code generation and modules need no library). Each library is archived once its files are. On
  Windows the compiles run below kpp's threads until kpp is done, then at the normal priority (a job
  object's priority class, raised for the ones still running): factorio.exe's whole build 0.7 s faster,
  kpp's part of it 0.6 to 0.9 s longer than a clean build's instead of 2.4 s (codegen-speed.md section
  5, 2026-11-08). KPP_CLIB_PRIORITY=normal builds as before.
- kpp leaves the link to kpp (`--defer-link`): it writes the link command to `EXE.link.command`,
  and `EXE.link.pending` when its objects are new. kpp links once both are done, also when only a
  C library changed (a compile server replays an unchanged program's build, which does not link).
- kpp's line gives the toolchain the build took, the whole build's time and its split, `kpp: built
  ...\factorio.exe (debug, msvc toolchain, total 21.2 s: C libraries 18.8 s alongside kpp 18.3 s,
  link 2.4 s)`, and a command that builds several programs ends with `kpp: built 2 programs, total N s`.
- Measured on factorio-in-kpp's debug build from scratch (d84869e1, its 641 C/C++ files, PC-PRACOVNA,
  3 rounds interleaved): 32.8, 39.1, 34.0 s with the libraries one after another before kpp;
  22.3, 27.6, 26.2 s with the pool (the last two on a machine 80 to 100% busy). The machine is busy
  all the time either way: the libraries take as long beside kpp as alone, and so does kpp.

### kpp's own steps in an edit build (2026-11-05)

An edit build is kpp's steps around kpp's and the link: build.kpp's description (cached in
`build\kpp\describe.txt`), each used library's, the C libraries' check, the after-build step.
`KPP_STEP_TIMES=1` (an environment variable, so that the build keeps its compile server) prints
where they went: `kpp steps: 0.039 s before the build (build.kpp described at 0.001 s, its libraries at
0.039 s), kpp 0.056 s (the C libraries' check alongside, 0.085 s), link 0.000 s, ...`. Measured on
factorio-in-kpp's `kpp factorio debug --msvc-diagnostics` (Visual Studio's build) with nothing changed:
- the C libraries' check (18 libraries: each object against its source and the headers its `.d`
  names) ran before kpp, one library after another, 0.10 to 0.19 s of every build; it now runs on
  a thread per library alongside kpp, and only the link waits for it;
- the libraries' config declarations (design.md, "config:: defaults in source") were read from every
  file of the six used libraries, 25 ms; they are kept in `build\kpp\uses\LIBRARY\config-LIBRARY.txt` with each
  file's time and size, 8 to 10 ms;
- the whole build with nothing changed: median 0.46 s before, 0.20 s after (paired binaries in
  alternating blocks, a loaded machine).

### kpp measure (2026-11-03)

`kpp measure [--rounds N] [--compare DIR] [--check]` in a project times its builds on this machine, as a user
runs them, and prints a table to paste (the readme's, from a factorio-in-kpp checkout; kovarex,
2026-11-03: one command, no script). build.kpp says what it edits and compares:

```
Measure& m = project.measure("factorio", "debug"); // the cell; "": the first executable, debug
m.edit("Edit of one function", "src/AnimationBank.kpp", "text there once", "its replacement");
m.compare("Edit of one function", "cd fastbuild && FBuild.exe ...", "src/AnimationBank.hpp");
m.compare("clean", "...");          // the clean build's other build
m.compare("whole factorio", "..."); // an executable's whole build's
```

- **Rows**, in each round (3 unless `--rounds`), one after another:
  - per `edit`: the file's text replaced, `kpp TARGET CONFIGURATION` (through the compile server, the
    program built before the round, untimed), the file put back, built again (the row ", undone");
  - the clean build: `kpp --stop-server`, `build\CONFIGURATION` deleted, the C and C++ libraries
    (`build\clib`) and the import cache kept, then the build, which starts the server again;
  - per executable, its whole build: the server stopped, `build\CONFIGURATION` and `build\clib`
    deleted, `kpp EXECUTABLE CONFIGURATION`.
  Each time is the build command's wall time from start to exit, the link and the after-build step
  included, and every build must write its executable anew (else the measurement stops).
- **`--compare DIR`**: after each row, its `compare` command (by the edit's row, `clean`, or `whole
  EXECUTABLE`), run by cmd in DIR, after its file there, if any, is touched (its time set: what
  includes it is built again, its contents unchanged); a second column.
- **Printed**: kpp's version and commit, the machine (processor, cores, threads, memory), the
  project's commit, the files it edits and how, how busy the machine was in the second before each
  round, each build's time as it goes, and at the end the table: each row's average over the rounds,
  one decimal ("average of N rounds" in its heading; kovarex, 2026-11-03: no range, an average), the
  --compare column the same; each round's own time is in the lines above it.
  Each build's own output goes to `build\measure.txt`.
- **Safe for the checkout**: it starts only when each file it edits holds its text exactly once and
  has no changes of its own (`git status`: it needs git); it puts the files back after each build, at
  the end, on an error, and on Ctrl+C or Ctrl+Break (the console's handler). It never edits the
  compared checkout, only sets the time of the files `compare` names. A text a file does not hold
  exactly once is named with its edit's row and how many times the file holds it (0: the source
  changed under build.kpp, as the game's `this->x` became `this.x`; 2026-11-10).
- **`--check`**: only those checks (every edit's text once in its file, no changes of its own, the
  --compare files there), then stops: nothing built or edited. Run it after a change to the sources
  build.kpp's edits name.
- Checked by tests\project.ps1 (a `kpp new` project measured in two rounds: its rows, each the
  average of its rounds' times, the file as committed after; a changed file refused, nothing built;
  `--check`, and an edit whose text the file does not hold, named by its row). Ctrl+Break during a round's edit put the
  file back (by hand, 2026-11-03).

## Project files for an IDE

These build, run and debug. IntelliSense (errors as you type, hover, go to definition, completion)
comes from the language server, `kpp --lsp`: [editors.md](editors.md) has it for VS Code, Visual
Studio and other editors.

**kpp generates them** (decided by kovarex 2026-10-31: no hand-written project files):

```
kpp --generate-visual-studio-files --generate-vs-code-files <build options> --exe OUT.exe SOURCES...
```

- **Visual Studio:** `build\visual-studio\<name>.sln`, `.vcxproj` and `.vcxproj.filters`. Every
  source file is listed, in folders by source root and directory, the standard library as
  *Standard Library*, each used kpp library as *Libraries\NAME* (its roots' names below that when it
  has several; 2026-11-05: by its root's name alone, gorating's src and every library's src were one
  folder). Each configuration's Build, Rebuild and Clean: a project's `kpp NAME CONFIGURATION
  --msvc-diagnostics`, the same with `--rebuild`, and with `--clean`; a build of sources (no build.kpp)
  has Clean stop its compile server (`kpp --stop-server` with the build's arguments) and delete the
  files `--clean` deletes, and Rebuild do that, then build. Project files written before 2026-11-08
  rebuild without cleaning: generate them again.
- **VS Code:** `.vscode\tasks.json`, `launch.json` and `settings.json`. A `.vscode` file kpp
  did not write is never overwritten. A build task starts kpp itself (`"command"`: kpp's path,
  `"args"`, `"options": { "cwd" }`), no cmd between: VS Code quotes a process task's arguments
  for CreateProcess, so a command line handed to `cmd /c` in one argument reached cmd with its
  quotes as `\"`, which cmd does not read, and every kpp under a path with blanks (an installed
  one, in Program Files) failed (kovarex, 2026-11-02). A build with before- and after-build steps
  (kpp's own `--before-build`, `--after-build`) needs cmd: its command is a `.cmd` file in
  `.vscode`, which the task hands to cmd as a path alone.

Both have debug (`--debug-info`, incremental link) and release (`--release`), each with its
executable in `build\<configuration>\`. They build through the compile server with
`--msvc-diagnostics`. Sources are passed as absolute paths, so errors carry absolute paths: the
IDE opens a relative path against the project file's directory, which is not the build's.
Options that build.kpp's targets and steps will give later:
- `--debug-command`, `--debug-arguments`, `--debug-directory`: what the debugger starts, with
  `{exe}` and `{configuration}` in them too;
- `--before-build` and `--after-build`: commands around the build, with `{exe}` and
  `{configuration}` in them.

The code is `compiler/src/generate.cpp`; `tests/generate.ps1` (in run_tests.cmd) does the
following:
1. Generates both for tests/run/hello.kpp.
2. Builds debug and release with MSBuild and runs each program.
3. Checks Debug's Clean and Rebuild command lines, then runs MSBuild's Clean (no `hello.*` left in
   build\debug, Release's program kept) and Rebuild (the program built again).
4. Checks that an error reaches MSBuild as `<absolute path>(line): error :`.

tests/project.ps1's matrix section checks a project's three command lines in every configuration,
that `kpp main --rebuild` compiles every module (a build after a build compiled none), and that
`kpp main --clean` leaves no `main.*` in build\debug and the other cells' executables in place, and
that `kpp main --check` succeeds without writing the executable.

Usage: [tools/vs/README.md](../tools/vs/README.md). programs/factorio's are written by
`tools\vs\factorio\generate.cmd`. Measured on its generated solution, MSBuild from the command
line, warm server, the converted tree of 783d327 (whose 123 errors against today's stdlib are the
tree's age, and `--keep-going` links anyway): 10 s, including native\build.cmd's check and the
copy into the game's directory. VS Code is not installed on PC-PRACOVNA: its files are checked
as JSON only, not run.

**Why a Makefile `.vcxproj`.** It is Visual Studio's own "external build" project.
- Ctrl+Shift+B runs kpp through its server.
- F5 builds, then starts the program under the debugger.
- The Debug/Release dropdown maps to the configurations.
- `.kpp` files are in Solution Explorer, and no C++ IntelliSense runs on them.

The alternative, Open Folder with `.vs/tasks.vs.json` and `launch.vs.json`, keeps its files in the
hidden, per-user `.vs/` directory. It has no configuration dropdown, and nothing documents that
it fills the Error List from a task's output. It was not built or measured.

**The Error List.** MSBuild reads errors from a tool's output only in the MSVC form,
`file(line[,col]): error|warning [code]: text`. Verified with MSBuild 17 on a Makefile project:
kpp's `path:line: error:` gave only `MSB3073 exited with code 1`, and `path(line): error:` gave
an error with file and line. Hence `--msvc-diagnostics`. Every diagnostic's place comes from one
formatter, `sourceLocation` (source.hpp), which gives `path:line`, or `path(line)` with the
option. Every diagnostic carries its label: the code generator's gaps ("does not support ...
yet") now print as `error:` too. run_tests.cmd checks both forms on tests/diagnostics: a parse,
a checker, a codegen and a static_assert diagnostic. kpp's messages have no column, so the Error
List shows lines only. (The first version, 456eda12, rewrote kpp's output through a pipe. That
was replaced because it could misread a program's own output under `--run`.)

**Measurements** (hand-written projects, before generation;
2026-10-30, the machine shared with seven other threads, so noisy):

| | command | times |
|---|---|---|
| hello, edit→build | `kpp --quiet --exe hello.exe hello.kpp` | 130–420 ms (10 runs) |
| hello, edit→build | the same with `--server` | 170–400 ms |
| hello, edit→build | `MSBuild.exe hello.vcxproj` (hand-written then; standalone, so its process start included) | 1.2–2.5 s |
| factorio, cold (server start) | `MSBuild.exe tools\vs\factorio\factorio.vcxproj` | 38.4 s |
| factorio, warm, a comment edit | `tools\vs\factorio\build.cmd` | 6.5, 7.1, 7.9, 9.0 s |
| factorio, warm, a comment edit | `MSBuild.exe factorio.vcxproj` | 6.4, 6.5, 9.8, 21.9 s |

Of factorio's warm build, `native\build.cmd`'s up-to-date check is 0.9 s and the copy into the
game's directory 0.16 s; the rest is kpp (front end, codegen, link). Inside Visual Studio the
build runs in-process, with no MSBuild.exe start. That measurement is still to do:
`measure.ps1` opened Visual Studio, which stopped at a "Microsoft Visual Studio Account Settings"
dialog (a sign-in prompt) that only kovarex can answer. After he has signed in once:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\vs\measure.ps1 -Project <a generated .vcxproj> -EditFile <one of its sources>
```

**Debugging.** F5 starts the executable under Visual Studio's debugger. The Debug configuration
builds with `--debug-info` (ee17f9a8, [debugging.md](debugging.md)): hello's PDB names
`hello.kpp`. Stepping and breakpoints in Visual Studio have not been tried yet, for the same
sign-in reason as the timings. programs/factorio builds Debug with it too:
all 64 modules, 29.1 s through MSBuild on a warm server, a 268 MB PDB naming the converted
`.kpp` files, and the game runs. Before B's fix edb182a4 (an exception leaving a catch handler
unwound to the caller, a miscompile in every build), clang rejected most modules with debug
info. That failure was hidden by `--quiet`.

**Colouring.** `tools/vs/kpp/Syntaxes/kpp.tmLanguage.json`, a TextMate grammar: Visual Studio
reads these from `%USERPROFILE%\.vs\Extensions\` when it starts, with no extension to build or
install. `install.cmd` links it there.

## Installing

kovarex's goal (2026-10-31): clone a project such as factorio-in-kpp, install a kpp release, run
`kpp` in the project's root, and build or generate Visual Studio files from there.
- **A release** is `kpp.exe`, `stdlib\`, `projectlib\`, `libraries\`, the
  bundled toolchain (below), the TextMate grammar, kpp-vscode.vsix, kpp-visualstudio.vsix and a README. kpp finds `stdlib\`,
  `projectlib\` and `bin\` next to itself. An installation needs nothing else: no Visual Studio,
  Build Tools or Windows SDK (design.md 17, stage 2; `--toolchain msvc` uses an installed Visual
  Studio's clang on request, as the game's build does).
- **FreeBSD as installed** (kovarex, 2026-11-04): a release brings FreeBSD 14.3's headers and static
  libraries in `sysroots\freebsd-x86_64`, so `kpp --target freebsd` builds and links with no
  `--sysroot` (kpp takes `sysroots\NAME` beside itself for `--target freebsd` and `linux`, where
  tools\sysroot\freebsd.ps1 also puts it in a build of kpp; a link without either is an error naming
  the path; `kpp --version` names it). package.ps1 copies a cut of freebsd.ps1's sysroot: the
  headers, the start files and every static library, no shared libraries (kpp links FreeBSD programs
  statically), with FreeBSD's COPYRIGHT, LLVM's licence for libc++, libcxxrt and compiler-rt, and a
  NOTICE naming OpenSSL 3.0.16 (Apache-2.0).
  - Sizes measured 2026-11-04: the whole sysroot 250.5 MB (zip 76.6 MB); this cut 90.6 MB, 3,253
    files (zip 24.2 MB, 16.5 MB in the installer's lzma2); a minimal cut (libc, libm, libthr,
    libgcc, libssl, libcrypto, libc++, libz...) 45.0 MB (zip 12.2 MB, 8.9 MB), with which the whole
    FreeBSD spec built and linked too. The release grew from 47 to 60 MB (installer) and 76 to 97 MB
    (zip).
  - The installer has it as a component, chosen by default; a later install without it removes it.
  - Not downloaded on first use: base.txz is 210 MB, from FreeBSD's archive over http only, and
    needs xz.
- **The official libraries as installed** (kovarex, 2026-11-04: the installed kpp alone is enough,
  until the registry exists): a release brings `libraries\` (json, sha256, base64, html, markdown,
  url, bcrypt, net, mariadb, http, httpclient: each one's tracked files, 92 files, 0.95 MB, its tests
  and third-party licence texts included), and `use("NAME")` finds them there when the project has
  no copy of its own ("Where `use("json")` looks" above). They are the release's versions: a
  project that needs another keeps its own copy. package.ps1's zip test unpacks the zip outside the
  repository and builds and runs a project using httpclient and mariadb with it; the nightly release
  check does the same with the installer's installation.
- **`kpp --version`** prints the version and commit kpp was built from ("Versions" below), the
  compiler's path, the toolchain a build takes without `--toolchain`, and the standard library.
  `kpp --help` lists the options. Both work only alone; in a project's directory they print and
  build nothing.
- **Options** (2026-11-02, after `kpp --version` built kovarex's project): an option that is neither
  kpp's (`--run`, `--test`, `--set`, `--no-server`...) nor kpp's is an error with a hint at
  `kpp --help`, and so is one missing its value. It is never passed on or ignored. A project's
  command line passes kpp's options on to each cell's build with their values (the table
  `compileOptions` in main.cpp, which kpp's own parsing must match).
- **The bundled toolchain in a release** is llvm-mingw's x86-64 part at its own relative paths, so
  clang finds the rest from its `bin\`: `bin\` (clang.exe, clang-19.exe, ld.lld.exe, llvm-ar.exe,
  llvm-rc.exe, llvm-dlltool.exe, libLLVM-19.dll, libclang-cpp.dll, libc++.dll, libunwind.dll and the
  two .cfg files), `include\`, `lib\clang\19\include\`,
  `lib\clang\19\lib\windows\libclang_rt.builtins-x86_64.a`, `x86_64-w64-mingw32\lib\` and
  `x86_64-w64-mingw32\share\mingw32\` (MinGW-w64's COPYING files). Also included:
  `LICENSE-llvm-mingw.txt` (the archive's LICENSE.TXT) and `NOTICE-toolchain.txt`, which gives each
  part's licence. The i686, armv7 and aarch64 trees, python, lldb, clangd and clang-tidy are left
  out.
- **`tools\release\package.cmd`** packages a built kpp into `build\release\`, named by kpp's own
  version ("Versions" below; `-Version` is an error). It runs
  package.ps1 with `-ExecutionPolicy Bypass`, because Windows' default policy forbids running
  scripts (kovarex's other PC, 2026-11-02). `tools\sysroot\llvm-mingw.cmd` does the same for its
  script. It takes the toolchain from `tools\sysroot\llvm-mingw.ps1` (pinned, SHA-256 checked, unpacked once into
  build\llvm-mingw). Its smoke test runs `kpp new` and `kpp --no-server --run` with the packaged kpp
  outside the repository, as on a machine without Visual Studio: PATH cut to System32 and the
  Windows directory, with no INCLUDE, LIB, LIBPATH or KPP_CLANG. It checks three things:
  - the link response file has `--target=x86_64-w64-windows-gnu` and `-fuse-ld=lld` (a project
    build is `--quiet`, so it has no toolchain line, and only the bundled toolchain builds
    windows-gnu);
  - the output;
  - that the program imports only Windows' own DLLs (llvm-objdump from the unpacked llvm-mingw,
    not shipped).

  It then writes the zip, and the installer when Inno Setup 6 is installed (`-IsccPath` names an
  ISCC elsewhere). kpp-vscode.vsix needs Node.js (npm). Without npm, a package made by hand leaves it out,
  with one line saying so: its README says so too, and its installer does not offer it. In CI
  (`CI=true`) a missing npm is an error. kpp-visualstudio.vsix (tools/vs/vsix, its build.cmd) needs the .NET 8
  SDK and Visual Studio's MSBuild, and is left out likewise without them (an error in CI). It is built
  from a copy at the repository's paths under `build\vsix` whose manifest carries kpp's version (the
  source keeps its own). Every step's failure stops the script.
- **How long it takes** (PC-PRACOVNA, 2026-11-02, `tools\release\package.cmd`, which prints each
  step's time on its last line): about 80 s, from 234 to 332 s before.

  | step | before (2 runs) | after (3 runs) |
  |---|---|---|
  | toolchain (llvm-mingw, unpacked once into build\llvm-mingw) | 0.0 s | 0.2-0.4 s |
  | copying (Copy-Item -Recurse, now robocopy /MT:16) | 10-17 s | 3-6 s |
  | kpp-vscode.vsix (npm ci 15 s, vsce 6 s) | 34-40 s | 3.5-13 s waited for (a job of its own; npm ci skipped while package-lock.json is unchanged) |
  | smoke test | 1-3 s | 3-6 s |
  | zip (Compress-Archive, now .NET ZipFile) | 118-193 s | 35-58 s |
  | installer (lzma2 solid, now in 8 threads) | 60-84 s | 16-17 s |
  | total | 234-332 s | 77-86 s |

  - **The installer:** compression settings measured on the 322 MB folder:

    | setting | time | size |
    |---|---|---|
    | lzma2 (its best), solid, 1 thread (before) | 68 s | 44.7 MB |
    | the same in 4, 8 or 16 threads | 16-28 s | 45.2 MB |
    | lzma2/normal | 11 s | 48.1 MB |
    | lzma2/fast | 8 s | 62.3 MB |
    | not solid | 48 s | 50.5 MB |
    | zip | 15 s | 69.7 MB |

    lzma2/ultra64 runs out of memory in ISCC. Chosen: the best in 8 threads, 45.2 MB.
  - **The zip:** alone, .NET's ZipFile takes 8-13 s for the same 72.1 MB as Compress-Archive's
    104 s. tar -a is 6 s, and ZipFile's Fastest is 5 s at 83 MB. In the package it takes 35-58 s,
    because the zip is the first to read the freshly copied files: a fresh copy zipped at once took
    37 s, and then 8 s twice. That is most likely the antivirus scanning new files on their first
    read. Hard links to the unpacked llvm-mingw instead of copies zipped in 9 s against 21 s, but
    only once the cache itself had been scanned (21 s both on the first round), and never in CI,
    whose files are all new; not done.
  - **Not parallel:** the zip and the installer at once took 43-49 s against 26-29 s one after the
    other, since the installer's 8 threads already fill the machine.
  - **`-NoSmokeTest`** leaves the smoke test out when iterating by hand. In CI (`CI=true`) it is an
    error.
  - **Sizes:** zip 75.6 MB, installer 47.4 MB (46.9 MB in one thread).
- **`tools/release/kpp.iss`** is the installer. It installs into Program Files\kpp and adds that
  directory to the system PATH (the uninstaller removes it). It checks for nothing: until stage 2,
  it warned when no Visual Studio with the Clang component was found. When VS Code is found
  (`code.cmd` on the PATH, else `%LOCALAPPDATA%\Programs\Microsoft VS Code\bin`, else Program
  Files'), it offers to install kpp-vscode.vsix into it; when vswhere finds Visual Studio 2022 17.14
  or later, it offers kpp-visualstudio.vsix, installed by that Visual Studio's VSIXInstaller
  (`/quiet`, as the user who started the installer). Each editor's extension is named for it
  (kpp-vscode.vsix, and Visual Studio's kpp-visualstudio.vsix): Windows opens every .vsix in Visual
  Studio's installer, which failed on the VS Code one when it was kpp.vsix (kovarex, 2026-11-02). An
  upgrade deletes an earlier release's kpp.vsix.
- **`.github/workflows/release.yml`** is started by hand (Actions > release > Run workflow): it
  builds kpp on a windows-2022 runner, packages it, and publishes the zip and installer as the
  prerelease `build-<run number>`, named by kpp's version. It ran on every master commit from
  e0ca8975 until the coordinator made it manual (2026-10-31): master gets dozens of commits a day,
  and no run had been checked. The automatic releases come from ci.yml (below): a numbered full release from
  every green master run. The repository is private, so its
  runs use the account's Actions minutes (Windows minutes count double) and its releases are
  visible only to its collaborators.

### The toolchain (design.md 17)

A release carries its own back end (design.md 17, decided 2026-11-02): it builds and runs a program
on a Windows with no Visual Studio, Build Tools or Windows SDK.
- **The bundled toolchain** is llvm-mingw 20250114 (github.com/mstorsjo/llvm-mingw, the UCRT
  variant for x86-64): clang and lld of LLVM 19.1.7 with MinGW-w64's headers and import libraries,
  which link the UCRT's ucrtbase.dll that every Windows 10 and 11 has. Its `bin\` is next to kpp.exe
  in an installation. `tools\sysroot\llvm-mingw.ps1` downloads the pinned release once (SHA-256
  checked) into build\llvm-mingw, for the tests and the package; `tools/sysroot/llvm-mingw.sh` the same
  release's Linux build into build/llvm-mingw-linux, with which kpp on Linux builds windows-gnu
  programs (KPP_CLANG=ROOT/bin/clang; design.md, stage 5). Licences: LLVM's Apache-2.0 WITH
  LLVM-exception, MinGW-w64's headers and libraries public domain or ZPL (the DirectX headers from
  Wine LGPL-2.1+), llvm-mingw's own scripts ISC.
- **Windows' GNU dialect** (`--target windows-gnu`, the bundled toolchain's own): the same ABI, data
  layout and SEH exceptions as MSVC's, MinGW-w64's headers in the C import (their `__attribute__`,
  `__extension__` and `__restrict__` taken as MSVC's are), lld's GNU driver (`--gc-sections`,
  `-municode` for a wmain). A program imports only Windows' own DLLs (ucrtbase, the api-ms-win-crt
  set, kernel32...). `config::mingw` is true for it (CWindows.kpp's fpclassify returns MinGW-w64's
  FP_ codes). Two pieces kpp makes once into the import cache with the toolchain's tools: an import
  library of ucrtbase.dll's functions MinGW-w64's leave out (`__uncaught_exceptions`, strtold), and
  math.h's long double functions on doubles: kpp's long double is a double, as MSVC's, and
  libmingwex's are x87's 80 bits.
- **Which toolchain a build uses:** `--toolchain bundled` or `--toolchain msvc` (Visual Studio's
  clang and its MSVC dialect, `--target windows`), and an error when that one is not there, never the
  other in its place. Visual Studio's clang is that of the newest Visual Studio with its "C++ Clang
  Compiler for Windows" component (vswhere), a prerelease one (Preview, Insiders) only when no other
  has it; the error names every Visual Studio here and what to install. A list in the order
  preferred, `--toolchain msvc,bundled`, takes the first of them this machine has (every name checked
  first; an error when none is): a build.kpp that prefers Visual Studio's builds where only the
  bundled one is (kovarex, 2026-11-03). kpp warns of such a build, why the first one is not taken
  (`warning: game (debug): --toolchain msvc,bundled takes bundled: --toolchain msvc needs Visual
  Studio's "C++ Clang Compiler for Windows" ...`), in Visual Studio's Error List too: a build of the
  other dialect, whose C imports and libraries differ, is never a silent one. `kpp --version` says
  whether Visual Studio's is here where the default is another (`toolchain msvc: ...`).
  Without it: KPP_CLANG, else the installation's `bin\` when it has one, else Visual Studio's. The
  bundled toolchain with `--target windows` (it has no MSVC headers) and Visual Studio's clang with
  `--target windows-gnu` are errors. kpp's build line names the one taken (`(debug, bundled
  toolchain, total ...)`). For Linux and FreeBSD it names that toolchain's clang, which links with
  ld.lld and the target's sysroot, whole in debug too (`(debug, freebsd: Visual Studio's clang and
  ld.lld, total ...)`). The build's report says more (`toolchain: ... (bundled, windows-gnu: links
  with lld)`). A compile server takes `bin\` next to its client's kpp.exe. A project's C libraries compile with clang's own flags under the
  bundled toolchain, with clang-cl's (`clang --driver-mode=cl`) under Visual Studio's.
- **The link:** the bundled toolchain links with lld, debug builds too. `--incremental-link`
  (link.exe `/INCREMENTAL`, the debug edit loop's link) is Visual Studio's: with the bundled
  toolchain it is an error, and a project's debug build leaves it out. Measured on the converted
  game (2026-11-02, the compile server's leaf and wide edits and each link alone, interleaved):
  lld-link's debug link with a PDB takes 5.7 to 9.7 s, 80% of it merging the 5.2 M type records of
  the Visual Studio-built libraries' debug information (the PDB 485 MB, link.exe's 77 MB), against
  0.4 to 1.5 s for link.exe's incremental relink; without a PDB lld-link takes 1.1 to 1.4 s. The
  game prefers `--toolchain msvc,bundled` (factorio-in-kpp's build.kpp): Visual Studio's, with
  `--incremental-link`, where it is here, else the bundled one, with which it builds, links Factorio's
  Visual Studio-built C++ libraries, imports only Windows' DLLs and passes its 9,501 tests and the 45
  menu maps' checksums (release) as with Visual Studio's (2026-11-10). The release link (ThinLTO) is the same with lld 19.1.5 and 19.1.7: 52.9 to
  66.6 against 53.6 to 74.9 s on a busy machine, the executable 44.0 MB both.
- **A symbol kept in the link** whatever the toolchain: `--keep-symbol NAME` links in what defines
  NAME although nothing names it, a C library's object that registers itself at startup (kpp-native's
  crash report, `kpp_crash_report_install`): link.exe's and lld-link's `/INCLUDE:NAME`, the GNU
  driver's `-u NAME`. A build.kpp gives it as an argument, never as a linker flag of one toolchain.
- **Checked:** tests\toolchain.ps1 (an installation made of llvm-mingw and this kpp: its default,
  `--toolchain msvc`, a list with and without Visual Studio (KPP_VSWHERE), the errors, a project's
  debug link, `--keep-symbol` with both toolchains, and that its programs import Windows' DLLs
  alone), tests\windowsgnu.ps1 (the 45 end-to-end programs in the GNU dialect), and
  `run_spec.py --target windows-gnu` (KPP_CLANG naming llvm-mingw's clang; 2026-11-02: every
  snippet the default target passes).

## Versions

design.md 18 (decided 2026-11-02): MAJOR.MINOR.PATCH.
- **MAJOR.MINOR** is the file `VERSION` at the root. It changes only by kovarex's decision.
- **PATCH** counts the commits since VERSION last changed:
  `git rev-list --count --first-parent <base>..HEAD`, where the base is
  `git log -1 --first-parent --format=%H -- VERSION`. The commit that changes VERSION is X.Y.0, and
  every later commit adds 1 with nothing edited.
- **The build:** `compiler/version.cmake` runs at every build of kpp (CMake's target
  `kpp_version`). It rewrites `compiler\build\generated\version.hpp` only when something changed,
  so only version.cpp is compiled again.
  - "+dirty" marks a build in which a tracked file differs from HEAD, or VERSION is not committed
    yet.
  - Outside a git checkout (a source zip, or no git) the version is "unknown".
  - A shallow clone that lacks VERSION's last change is an error: `git fetch --unshallow`, or
    `fetch-depth: 0` in CI.
- **`kpp --version`** prints `kpp 0.2.5, commit <hash>`, then the compiler, the toolchain and the
  standard library.
- **The objects** of a `--debug-info` build name their compiler `kpp 0.2.5` (CodeView's S_COMPILE3,
  DWARF's DW_AT_producer): the version without the commit and "+dirty", so one version's objects are
  the same (kovarex, 2026-11-03). LF_BUILDINFO stays empty.
- **A release** is named by it, always. `tools\release\package.cmd` reads it from the kpp it
  packages (`kpp --version`), and it names the files, the README, the installer and the VS Code
  extension. Nothing takes a version by hand (kovarex: "take the version from the versioning system
  always, not from command line"). `-Version` is an error, and kpp.iss without one is an error. A
  local package of uncommitted changes is `X.Y.Z+dirty`. CI's release is v<version>, and a "+dirty"
  or unknown version fails its Version step.
- **0.2 to start:** VERSION began at 0.2 because CI had published v0.1.141 to v0.1.146 under the
  earlier run-number scheme, and the first versioned build, 0.2.0, sorts above them.

## Continuous integration

`.github/workflows/ci.yml` (thread H, 2026-10-31) runs kpp's tests on windows-2022 runners,
or on the runner named by the repository variable `KPP_CI_RUNNER`, as parallel jobs (kovarex,
2026-11-02: the repository is public, so GitHub's standard runners cost nothing):
- **build:** kpp, once, with `compiler\build.cmd` through ccache (below), handed to the other
  jobs as the artifact `kpp` (compiler\build\kpp.exe, kept 3 days).
- **run_tests:** `compiler\tests\run_tests.cmd --full --part 1` and `--full --part 2` in 2 jobs,
  after build: part 2 is determinism, concurrency, generate, project and format ours, part 1 everything else
  (about 2 minutes each on the runner). Each ends with its own line, as `run_tests: ALL PASSED (part
  2/2: checks 4/4)`; without `--part` it runs both, as by hand. run_tests runs its checks side by
  side, each a process of its own (tests\run_tests.ps1 lists them; `--jobs N` at once, by default a
  third of the logical processors, 4 to 12), and prints each one's output when it ends.
  `--full` is all of it, as CI and thread H's batch run on master run it. Without it, the quick
  gate's run (kovarex, 2026-11-04: speed over zero risk): concurrency, incremental and windows-gnu
  left out, the Linux targets not run under WSL, and the determinism check's quick mode: the test
  programs at -j 1 and -j 32 and the game's front end at -j 8 and -j 32, against -j 1, -j 8 and -j
  32 and the game's front end and IR at -j 1, -j 8 and twice at -j 32 in full (12 s against 73 s,
  both with their builds run at once, on a loaded machine).
- **Before a push (by hand):** `compiler\tests\gate.cmd`, run_tests' quick run and the spec side by
  side, ending `gate: ALL PASSED (...)` and exit 0: about 3 minutes on PC-PRACOVNA with other
  threads' work running, against about 14 for run_tests and then the spec one after another before
  (2026-11-05). Beside them it builds the `*_release.kpp` snippets for Linux and runs them under WSL
  (a few seconds; skipped with a notice when WSL does not answer). What it leaves out runs here on
  every push.
- **spec:** `compiler\tests\spec.cmd --no-doc --shard I/N`, in debug (2 shards), `--release`,
  `--debug-info` and windows-gnu (4 each): 14 jobs, after build. A shard is
  every Nth snippet of the sorted list (run_spec.py), so each gets a share of every area; each ends
  with its own `spec: ALL PASSED (...; shard I/N)` line and exit code, and the run is green only
  when all of them are.
- **linux_build, linux_spec, linux_host:** kpp on a Linux host (ubuntu-24.04, 4 CPUs). linux_build
  builds kpp with clang 19 and hands it on as the artifact `kpp-linux-compiler`; after it, at once,
  linux_spec runs the spec in debug (2 shards) and in `--release` (4 shards) and linux_host its smoke tests (hello
  world, the compile server, the language server, a project with C libraries, windows-gnu from Linux)
  and packages kpp's Linux release. Until 2026-11-08 one job ran the build, the spec and its release
  one after another, 19 to 26 minutes, every other job ending by 9. Their tools are
  .github/actions/linux-tools.
- **package:** on master only, after build and beside the tests (kovarex, 2026-11-02): the package
  (below), handed on as the artifact `package` (zip, installer, kpp-vscode.vsix, kpp-visualstudio.vsix). It publishes nothing.
- **publish:** on master only, and only when the jobs it needs are green (run_tests' halves, every
  spec shard, package, linux_spec, linux_host): the release and kpp.net (below). If one of them is red or
  cancelled it publishes nothing: no release, no tag, no upload, and Latest stays where it was. It
  does not wait for the FreeBSD jobs, so a red one of those alone does not stop a release. It checks
  the artifacts it downloads against the SHA-256 their jobs wrote (`tools\release\check-files.ps1`),
  downloads them once more if they differ, and publishes nothing if they differ again. A pull
  request packages nothing.
- **libraries_freebsd_build, libraries_freebsd_run:** on every run, after build; publish
  does not wait for them (kovarex, 2026-11-04: FreeBSD binaries' correctness belongs to kpp's own
  tests). `libraries\target-tests.ps1 -Target freebsd` builds every library's tests and kppsite's for
  FreeBSD 14.3 (`kpp --target freebsd --no-run --test` in each project; a project that does not
  build fails the job unless `libraries\target-known-failures.txt` lists it, and a listed one that
  builds fails it too); the bundle (each test program, `run.sh`) then runs in a FreeBSD 14.3 VM
  (vmactions/freebsd-vm, as the spec's FreeBSD run) from the checkout, httpclient's HTTPS test
  included (`HTTPCLIENT_TEST_HTTPS`: the VM reaches the internet). Its last line, `libraries freebsd:
  ALL PASSED (12 programs, 70 tests)`, goes to the run's summary. The same scripts run the Linux
  build in WSL by hand (`-Target linux`, then `sh run.sh CHECKOUT` there).

**The release check** (`.github/workflows/release-check.yml`, kovarex, 2026-11-03) tests what was
published, not the checkout: daily at 04:17 UTC (an hour after the nightly run below publishes), or
by hand for any tag (Actions > release check > Run workflow), on a clean windows-2022 runner.
- It renames Visual Studio's two directories and the Windows SDK's (`Windows Kits`) away first, so
  nothing can use them (kpp finds Visual Studio through vswhere, which is in one of them), and runs
  no vcvars.
- It downloads the Latest release's `kpp-<version>-setup.exe` and installs it silently (`/VERYSILENT
  /SUPPRESSMSGBOXES`). Program Files\kpp must hold kpp.exe and be on the system PATH.
- `tools/release/check-release.ps1` then uses it with PATH cut to the installation, System32 and the
  Windows directory; VSINSTALLDIR, INCLUDE, LIB, KPP_CLANG and the like set are a failure. It checks:
  - `kpp --version`: its first line is `kpp <version>, commit <the tag's commit>`, and its toolchain
    is the installation's `bin\clang.exe` (bundled);
  - `kpp new hello`, then `kpp hello debug --toolchain bundled --run` and the same in release;
    `kpp hello --verbose`'s toolchain line says `(bundled, windows-gnu: links with lld)`;
  - a project with a C library built from its source, whose define comes from a build.kpp setting:
    debug with the default, release with `--set PLUS=2`, each with `--toolchain bundled --run`;
  - that every program it built imports only Windows' own DLLs (read from the executable's import
    table: the UCRT, kernel32 and the like).
- A failure is a red run, with each check's line and the failures in the run's summary. It opens no
  issue and mails no one (GitHub mails a failed scheduled run to whoever last changed its cron line,
  if their notification settings say so). It takes under a minute: 51 s, the first run (v0.2.116,
  run 41610046288).

Triggers (kovarex, 2026-10-31, chosen from the costs below):
- **Every push to master and every pull request, nightly at 03:17 UTC, and when started by hand**
  (Actions > ci > Run workflow): every job, run_tests --full, the spec's three configurations, 4
  shards each (12 spec jobs), and the FreeBSD jobs (kovarex, 2026-11-05: "having all the tests in CI
  is fine, as they don't block pushing more things"; until then a push ran the debug spec alone and
  the rest was nightly). The nightly time is outside 06:00 to 09:00 Pacific.
- **What it leaves out:** `programs\factorio\convert.cmd` and everything else of the game. They need
  C:\Factorio, private source that is not on the runner; factorio-in-kpp's own CI is the game's check.
- **SDL3 headers:** the two interop snippets that import SDL3/SDL.h get upstream SDL 3.4.18's headers
  through `KPP_SDL_INCLUDE` (run_spec.py). That is the game's release; the game's copy adds only
  `SDL_GetDisplayInternalDriverData`. The zip is SDL's release on github.com, with the SHA-256 in
  ci.yml (`SDL_SHA256`), or the step fails.
- **Pinned downloads:** the release files CI takes from github.com (SDL3's zip, ccache's) go through
  `.github/actions/pinned-download`: actions/cache keeps each under its SHA-256 (`pinned-<sha256>`), so
  a job downloads it only on a cache miss, four tries 15, 30 and 45 s apart, and saves it at once,
  before the job's later steps can fail. A cached copy is checked against the SHA-256 too. GitHub's
  release downloads answered 503 and 504 for over an hour on 2026-11-08 and turned three master runs
  red; the cache replaced kpp.net's mirror of the SDL3 zip, which needed CI to write to kovarex's
  server (kovarex 2026-11-08, through the coordinator).
- **Clang for the snippets' C sources:** clang comes from vswhere through `KPP_CLANG`, because the
  runner's Visual Studio is Enterprise. Node.js must be on the runner, for run_tests' LSP smoke test
  and the package's VS Code extension; the step fails without it.
- **Publishing:** every green run on master (push, nightly or by hand) installs Inno Setup
  (`choco install innosetup`, as release.yml does) and packages kpp with `tools/release/package.ps1`,
  including the package's own smoke test: the zip, the installer `kpp-<version>-setup.exe` and
  kpp-vscode.vsix and kpp-visualstudio.vsix. The version is kpp's own ("Versions": VERSION and the commits since it changed; the
  checkout fetches the whole history for it). `tools/release/publish.ps1`
  then publishes the three files to two places, each only when its credential is there:
  - **kpp.net** ([library-registry.md](library-registry.md), "Hosting on kpp.net"; the store's
    layout in [kppnet/registry/layout.md](../kppnet/registry/layout.md)). Installers and
    factorio-in-kpp's CI read this copy.
    - The files go to `registry/kpp/releases/<version>/` over SSH. A file already there is kept,
      which is rsync `--ignore-existing`'s rule.
    - `registry/kpp/latest.json` is written last, as a temporary file renamed into place:
      `{"version", "commit", "zip", "sha256"}`, with `zip` relative to `registry/kpp/`.
    - It uses OpenSSH's ssh and scp, because Windows runners have no rsync. Every file is sent
      under a temporary name and renamed, so a reader never sees half of one.
  - **The forge's full release**, made through the forge's REST API with the job's token: every
    green master run's, push, nightly or by hand (kovarex, 2026-11-02), titled "kpp <version>",
    with the three files. A push or hand-started run's tag is `v<version>`; the nightly run's is
    `nightly-<date>` (UTC, as `nightly-20261103`; it builds a commit a push run has released, so
    its version is that one's; until 2026-11-02 it was `v0.1.<date>`), titled "kpp <version>
    (nightly <date>)", one durable release per day (a second nightly run on the same day replaces it). On GitHub a release that
    exists is updated in place (its tag moved, the new files uploaded beside the old ones, then the
    old ones removed), so a failed step leaves the old release whole. GitHub refuses the job's
    token a tag on a commit whose `.github/workflows` differ from master's at that moment (403
    "Resource not accessible by integration"): a run whose commit predates a workflow change pushed
    while it ran cannot publish, and says so; the next run publishes. A call that fails on
    GitHub's side is tried again, up to 4 times, 10, 30 and 60 s apart: a call that got no answer
    or a 5xx, and an upload after any failure but 401, 403 and 404, the release's half-made file of
    that name deleted first; a retried release creation first takes the release of its tag, which
    the lost answer may have made (2026-11-05: two runs in a row lost their release to one 500 and
    one failed upload). run_tests' publish check runs it against a fake GitHub that fails a call
    of each kind once (`tools/release/publish-test.ps1`).

  `tools/release/settle.ps1` then makes the release of the highest version GitHub's "Latest",
  which https://github.com/kovarex/kpp/releases/latest and the repository's sidebar show, and
  factorio-in-kpp's CI downloads. It takes the version from the release's title ("kpp 0.2.5") and
  compares it as numbers, so 0.2.0 is above 0.1.148 and 0.2.10 above 0.2.3; of a push and a
  nightly release of one version, the push one (`v<version>`) is Latest. It goes by version, not by
  which run finished last: push, nightly and hand-started runs have concurrency groups of their
  own, so an older commit's run can finish after a newer one's, and it then leaves the newer one
  Latest. It also deletes the push releases (`v<version>`) beyond the 50 highest versions, each
  with its tag, but never one `tools/release/keep.txt` names: the releases known projects' CI pins
  (gorating's broke on 2026-11-05 when its pin, v0.2.242, went with the 20 kept then; a project that
  pins a release adds its tag there, or pins a nightly one). The nightly ones are never deleted. From 2026-11-02 until this, a push run published only the rolling
  prerelease `dev` and only the nightly a numbered release, so a green push left Latest old; the run
  deletes `dev` now. Releases by hand stay with release.yml.
- **What kovarex sets up for kpp.net:** until then, the step says "kpp.net: skipped" and CI works
  as before. Nothing in the repository holds a secret.
  - the repository secret `KPP_NET_SSH_KEY`: an Ed25519 private key, its public half added to
    NFS's keychain for the site;
  - the repository variable `KPP_NET_SSH`: the SSH login,
    `<user>_<site>@ssh.<host>.nearlyfreespeech.net`;
  - the repository variable `KPP_NET_KNOWN_HOSTS`: the server's host key line, from
    `ssh-keyscan ssh.<host>.nearlyfreespeech.net`, checked against what NFS shows. ssh runs with
    `StrictHostKeyChecking=yes`, so a run never trusts a key it has not been given.

  Only the SDL3 step and the publish step see the key, not the steps that build and run the pushed
  code. Each green master run uploads 3 files and latest.json, and runs only as often as the
  concurrency rule lets them. That stays under NFS's limit of one uploaded file per five minutes on
  average (288 a day).
- **Caches** (actions/cache, each keyed on what it holds):
  - **ccache** (4.10.2, downloaded by the build job and checked against its pinned SHA-256): the
    compiler's objects, keyed on the hash of compiler/src, CMakeLists.txt and the .cmake files,
    restored from the newest entry when they changed. A cached CMake build directory would not do:
    checkout gives every source a new time, so Ninja rebuilds everything. `compiler\build.cmd` takes
    a launcher from `KPP_CCACHE` when it configures build\. version.cpp is compiled again on every
    commit (its header names the commit).
  - **LLVM** (compiler\llvm.ps1's unpacked libraries and headers of the pinned clang+llvm release,
    256 MB, for the back end inside kpp), keyed on the release's SHA-256, which the build job reads
    from llvm.ps1; without it every run downloads the 845 MB archive.
  - **llvm-mingw** (build\llvm-mingw, for run_tests and the package), keyed on
    tools/sysroot/llvm-mingw.ps1, which pins the release and its SHA-256 and checks it.
  - **The SDL3 zip and headers**, keyed on `SDL_SHA256`.
  - **Debian 11's Linux sysroot** (compiler\build\sysroots\linux-x86_64, for targets.ps1), keyed on
    tools/sysroot/linux.ps1, which pins its packages' SHA-256.
  - **The Linux jobs' apt packages** (the 16 .deb files of clang 19, lld, ninja and cmake): a hit installs
    them with dpkg, no Ubuntu mirror. Files that no longer install go to the mirror, and that run saves its
    own under a new key (the run's id; linux_build alone saves), the newest being restored.
  - kpp's import cache (%TEMP%\kpp-imports) is not worth one.
- **Concurrency:** one run at a time per trigger and branch. A push run on master that has started
  finishes, so a green one publishes; of the pushes meanwhile, only the newest waits (GitHub
  cancels an older waiting run), so at most one run waits. Until 2026-11-02 a newer push cancelled
  the running one, and while pushes came every few minutes nearly every push run was cancelled
  before it could publish. A pull request's newer push still cancels its older run. The nightly and
  hand-started runs have groups of their own, so a push does not cancel them.

### GitHub Actions and Forgejo Actions

The repositories' home is to be a Forgejo forge on kpp.net, with GitHub as a mirror (kovarex,
2026-10-31; thread I designs it). So ci.yml runs on both:
- every step is a script (Windows PowerShell 5.1 or cmd) except `actions/checkout@v4`,
  `actions/cache@v4` and `actions/upload-artifact@v4` / `download-artifact@v4`;
- it names no repository URL: the forge's API comes from `github.api_url` and the repository from
  `github.repository`;
- publishing is `tools/release/publish.ps1 -ApiUrl URL -Repository owner/name -Tag T -Commit SHA
  -Title T -Files F...`, with the token in `KPP_FORGE_TOKEN`. It speaks GitHub's API, or
  Forgejo's (Gitea's) when the API URL ends in `/api/v1` (`-Forge github|forgejo` overrides). The
  kpp.net path needs only ssh and scp. Both forge paths were checked against a mock of the two
  APIs (create, then replace). The kpp.net path was checked against a mock ssh and scp writing a
  local directory:
  - the files are uploaded;
  - an existing file is kept;
  - latest.json is replaced;
  - a mirror file goes under its SHA-256;
  - there is no upload without the key;
  - it stops with an error when the key is set but known_hosts is not.
- What is not portable, or not checked on Forgejo:
  - **`actions/checkout@v4`:** Forgejo fetches it from its own mirror of the actions (its runner's
    default actions URL), not from github.com.
  - **The artifact `kpp`:** Forgejo's artifact store speaks the protocol of upload-artifact@v3 and
    download-artifact@v3, not v4's; there the two are written @v3. actions/cache works against the
    runner's own cache server.
  - **`runs-on`:** a Forgejo runner is self-hosted, with labels of its own, so the repository
    variable `KPP_CI_RUNNER` must name its label. Whether Forgejo expands `vars` in `runs-on` has not
    been checked; if it does not, the label is written in place of the expression.
  - **The runner itself:** Forgejo hosts no runners. A Windows machine with Visual Studio 2022 and
    its Clang component, Node.js and Python runs `forgejo-runner` in host mode.
  - **`concurrency`:** GitHub's one run at a time, with one waiting (and a pull request's older run
    cancelled). Forgejo Actions may not honour it; there, every push runs, side by side.
  - **The SDL3 headers** come from libsdl-org's release on github.com (`SDL_URL`), kept by the
    cache (pinned-download); whether Forgejo's actions/cache keeps them has not been checked.
  - **The job's token:** `github.token` on both. On Forgejo it must be allowed to write releases.

Cost, estimated before any runner run. These are measured on PC-PRACOVNA pinned to 2 logical CPUs
(`start /affinity 3`), a stand-in for the 2-vCPU Windows runner a private repository gets, and on
the full machine:

| Step | 2 CPUs | 32 CPUs |
|---|---|---|
| clean kpp build | 68 s | |
| run_tests.cmd | 208 s | 252 s |
| spec.cmd (debug, `--jobs 2`) | 1,168 s | 269 s (39.5 s on a quiet machine: the 32-CPU column ran beside other threads' builds) |
| spec.cmd --release | | 275 s |
| spec.cmd --debug-info | | 263 s |

A run with all three spec configurations is about 64 minutes on that stand-in, and likely more on
the runner's slower cores. Windows minutes bill at twice the rate on a private repository, so one
run is about 130 to 250 billed minutes. With commits every few minutes, the master queue would run
back to back, about 22 runs a day, which far exceeds an included monthly quota (3,000 minutes on
Pro) within a day. So each push runs only the debug spec (about 25 to 45 runner minutes), and the
three configurations run nightly. A larger runner (billed per minute, outside the included
minutes) is the other option.

On the runner (windows-2022, run 41367545233, 2026-11-02): build 123 s, run_tests 304 s, spec
586 s, about 17 minutes for a push run; the nightly adds two more spec runs. While pushes come
faster than that, the push queue keeps the runner busy whether runs are cancelled or finished
(a cancelled run bills the minutes it ran). Running more snippets at once does not shorten spec on
2 CPUs: spec's library snippets pinned to 2 CPUs took 17.5 / 17.3 s with `--jobs 2`, 15.7 / 15.3 s
with 4 and 15.5 / 15.6 s with 8 (the default). Splitting spec over several runners would shorten a
run but bill each runner's checkout and 2-minute kpp build again.

Measured on the public repository's windows-2022 runners (4 CPUs, 2026-11-02), from the first
job's start to the run's end, a push run (debug spec):

| | one job (run 164, a840908e) | parallel jobs, caches warm (run 192, f79d7624) | run_tests in halves, package beside the tests (run 201, 44d0f0fa) |
|---|---|---|---|
| build | 56 s | 62 s job: LLVM 1 s (cached), kpp 35 s (ccache: 17 of 19 compiles hit) | 51 s job |
| run_tests | 269 s | 260 s job, beside the spec | 2 jobs: 137 s and 115 s |
| spec (debug) | 229 s | 4 shards at once, 59 to 74 s per job (spec itself 40 to 55 s) | 54 to 78 s per job |
| package | 121 s | in the publish job: 73 s | 111 s job, beside the tests |
| publish | 95 s | 255 s job (with the package; upload 159 s) | 190 s job (the artifact's download, upload 165 s) |
| **the run** | **13 min 43 s** | **9 min 43 s** | **6 min 23 s** |

The first parallel runs, before the caches held anything, took longer in their build job: D's
LLVM step downloads and unpacks the 845 MB archive in 56 to 65 s, and kpp's clean build takes 91 s
(run 191, 9 min 47 s in all). Now the run is build, then run_tests' longer half, then publish, half
of it GitHub's upload of the three files (about 146 MB), which alone varies from 95 s to 3 min 40 s.

## Deploying a kpp web app

kovarex, 2026-11-04: "one simple command on the prod server to pull the new version and download
the new binary as deploy". A kpp web app (gorating.com first, kpp.net later) is a CGI program and
its static files; its CI builds the program for the server's target with a pinned kpp release,
tests it there, and publishes a package as a GitHub Release asset: a tar.gz holding `public/` (the
program and the static files, as the web root has them), `migrations/` (SQL, run in name order),
and `MANIFEST` (`app NAME`, `version`, `migration FILE` lines), with a `.sha256` file beside it.
Nothing in CI connects to the server.

`tools/deploy/kpp-deploy.sh`, run by hand on the server (POSIX sh, FreeBSD's base tools, or
Linux's curl, sha256sum and GNU stat), installs one:
- `kpp-deploy.sh [VERSION]` (the latest release without one), or `--from FILE` for a package file;
- it checks the SHA-256 (integrity: both files come from GitHub) and that the MANIFEST names the
  app;
- migrations not in the database's `deploy_migration` table stop the install and are listed;
  `--migrate` runs them in order and records each; none runs silently. `--mark-applied NAME...`
  records ones applied by hand (and makes the table);
- the configuration's mode and group are checked (`-rw-r-----`, `web` on NearlyFreeSpeech.NET),
  never its contents;
- the static files are copied, then the program as `PROGRAM.new` with mode 755, the current one
  kept as `PROGRAM.previous`, and `.new` renamed over it (atomic);
- each smoke path (`SMOKE`) runs through the program as Apache runs it; one that does not answer
  200 puts the previous program and static files back by itself;
- `--rollback` does the same by hand (the replaced program stays as `PROGRAM.failed`); `--status`
  names the installed version and the migrations applied. The database is never rolled back.

The app's settings are the script's first lines (gorating's: its repository, asset, web root,
program, configuration, the database client's defaults file, the smoke paths), or a file named by
`DEPLOY_CONF`. `tools/deploy/test-deploy.sh PACKAGE DATABASE` runs it against a package in a
made-up server (a fresh install refused without the migrations' table and with pending ones, then
`--migrate`; the program's mode, the static files, a second install, `--rollback`, a broken
program rolled back by itself, a wrong SHA-256): gorating's CI runs it in its FreeBSD VM against
its package and MariaDB; it passed on Linux (WSL) with a made-up package and database client
(2026-11-04).

## Questions for kovarex

Answered 2026-10-31: the build description is `build.kpp` in kpp, with `src/` and `libraries/`
(above); project files for an IDE are generated by kpp (54e30458), so there are no hand-written
`.vcxproj` files. Settled by what exists (2026-11-06): who builds the C libraries the game links
("FASTBuild", above). Answered 2026-11-07: release builds have debug information by default, "for
crash reports the same way as in factorio" (kovarex): `debugInfo` is true in release, so in final and
every configuration based on it unless one sets it false. It is a file beside the executable (the
PDB; for Linux and FreeBSD NAME.debug, split out by llvm-objcopy, the executable keeping a
.gnu_debuglink), and an optimized build is line tables only; the executable names its PDB without the
directory (/PDBALTPATH:%_PDB%) and its text keeps the --file-prefix-map paths, so it still holds no
path of this machine (question 1, d8ce47da). Answered 2026-11-07, question 2, the command's name:
"kpp everywhere" (kovarex). The repository builds `compiler\build\kpp.exe` (`compiler/build/kpp` on
Linux), the name a release installs; it built kppc.exe until then. Every script, doc, spec snippet and
CI step says kpp (the `// kpp:` directive of a snippet, KPP and KPP_EXTRA in the scripts), and the
compile servers live in `%TEMP%\kpp-server` (`/tmp/kpp-server-UID`): a kppc server still running from
before is in kppc-server, never taken for one of kpp's, and exits when idle. No kppc.exe is left
beside it, so a stale script fails loudly instead of running an old compiler.

No question is open.
