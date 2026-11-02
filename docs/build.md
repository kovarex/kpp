# Building Kpp programs

How a Kpp program is built today, where FASTBuild fits in, the build description (`build.kpp` in
Kpp, decided 2026-10-31: the layout, adding libraries, configurations), and the Visual Studio
integration (`tools/vs/`). Open details are the questions at the end.

## Today

**One command builds a program.** `kppc [options] <file-or-dir>...` compiles the whole program
in one process. It takes every `.kpp` under the given roots plus the standard library (`stdlib/`
next to kppc, or `--std DIR`, or none with `--no-std`). It runs the front end and code generation
on all threads and splits the IR into modules (two per thread by default, `--modules N`). It
compiles each module with the bundled clang, skipping modules whose IR hash is unchanged (the
`.N.hash`, `.N.ll` and `.N.o` files next to the executable are the object cache), and links. There
are no headers, no per-file compile steps and nothing like make: a "build system" for Kpp code is
only the choice of kppc's arguments.

**What controls a build: command-line options only.**

| Option | What it does |
|---|---|
| sources | directories (recursive) and files; every one is part of the one program |
| `--exe F` | the executable; the object cache sits next to it |
| `--config N=V` | a constant `config::N` (bool, integer, enumerator, string) the code reads with `if constexpr` |
| `--release` | the same as `--config debug=false` |
| `--optimize`, `--no-optimize` | clang `-O2` (ThinLTO, a whole link) or `-O0`, whatever `config::debug` is; unset, `-O2` when `config::debug` is false |
| `--lib F` | passed to the link: a `.lib`, `-lname`, `-Wl,/OPT`, a `.res` |
| `--cinclude D` | a directory searched by the C headers the code imports |
| `--clang-flags F` | more flags for clang's compile of the modules (`-fsanitize=address`) |
| `--incremental-link` | debug builds only: link.exe `/INCREMENTAL`, so the next link redoes only changed objects |
| `--no-inline` | kppc emits no trivial function into its callers (a diagnostic tool; inlining is on by default) |
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
| kppc inlining of trivial functions | on | on |

**The compile server.** `--server` anywhere on the line makes kppc a client. The server is keyed by
kppc's path, the working directory and the argument list, so every distinct argument list has its
own server. A server keeps the last build's program in memory, rebuilds only what an edit touched,
and ends after an hour idle. `--stop-server` with the same arguments stops it. A Visual Studio
build (`--msvc-diagnostics`) therefore has a server of its own, apart from the command line's.

**Programs describe their builds in .cmd scripts.** `programs/factorio/link-settings.cmd` sets
`KPPC`, `CINC` (8 `--cinclude`) and `LIBS` (19 libraries of the game's C++ build, 21 Windows
libraries, the shims and the resource file). `link.cmd` and `run.cmd` both call it and then each
write their own kppc line: 6 source roots and 4 (link) or 9 (run) more options.
`tools/vs/factorio/generate.cmd` is a third copy of run.cmd's line. Three copies of one build is what a project file removes.

## FASTBuild

**FASTBuild plays no part in building Kpp code.** kppc never calls it and has no use for it: there
is one compile per program, already parallel and cached, so a distributed per-file build tool has
nothing to schedule. FASTBuild remains Factorio's C++ build (`C:\Factorio\fastbuild\fbuild.bff`).

**What the Kpp game still takes from it:** the C and C++ libraries it links. The 26 `.lib` files in
`C:\Factorio\vs2022\Releasex64` (SDL, curl, zlib, zstd, ogg/vorbis/opus, re2, imgui, glad,
minizip...) are outputs of the C++ Release build by FASTBuild, dated 2026-04-21. Nothing in this
repository rebuilds them. `programs/factorio/native/build.cmd` builds the shims with clang-cl
directly, not with FASTBuild.

**This is a problem now.** 54 commits of `C:\Factorio` have touched `libraries/` since those
.libs were built, among them SDL 3.4.18 (2026-10-29) and zlib 1.3.2 (2026-10-10). The game is
compiled against the headers at HEAD (`--cinclude C:\Factorio\libraries\...`) but linked with
April's libraries. The rule is "always the latest pull", and this does not follow it. Rebuilding
them with FASTBuild writes into `C:\Factorio`, which is read-only. See question 3.

## The build description

**Decided by kovarex (2026-10-31):**
- A project's builds are described in `build.kpp`, a Kpp file at the project's root.
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

Kpp is placed unusually well for point 1. Its compile-time interpreter (design.md,
"Compile-time evaluation") already runs ordinary Kpp with no I/O and no C calls, under step and
memory limits: Starlark's sandbox, without a second language.

### build.kpp

- **What it is.** kppc compiles `build.kpp` on its own, with the standard library and a small
  `build` package (`Project`, `Executable`, `Step`), and runs its `build` function in the
  interpreter once, for every target and configuration (below, "Targets and configurations"). The
  function can compute (loops, functions, the standard
  library), but it can only describe: it fills in a `Project`, and kppc builds what that says.
- **It cannot act.** It cannot read a file, start a process or call C. That is the interpreter's
  existing limit, so no separate sandbox is needed.
- **Its errors are Kpp errors**, with the same Error List, colouring and IDE navigation.
- **The program does not see build.kpp's declarations.** What crosses over are the `config::`
  constants the build sets.
- **Inputs from outside come only from the command line.** `kpp --set FACTORIO=D:\snapshot` is
  read as `project.setting("FACTORIO", "C:/Factorio")`, so a build depends only on its files and
  its arguments. The arguments are part of the server's key.
- **Its cost:** about 0.3 s for factorio-in-kpp's (below), and the result is kept until the file
  changes.

```cpp
// build.kpp of programs/factorio
void build(Project& project)
{
    StringView factorio = project.setting("FACTORIO", "C:/Factorio");
    project.use("sdl3", "3.4");                // for every target
    project.use("lua");
    project.use("factorio-native");            // libraries/factorio-native
    for (StringView lib : {"SDL3/include", "SDL_Mixer/include", "Lua", "", "curl/include"})
        project.cIncludes.add(path(factorio, "libraries", lib));
    project.configuration("fastdebug", "debug").optimize = true;

    Executable& game = project.executable("factorio", {"src"});
    game.steps.add(Step::copy(game.output, path(project.setting("GAME", "C:/factorio-in-kpp"), "bin/x64")));

    Executable& tests = project.executable("factorio-tests", {"src", "tests"});
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

  debug is the default and may be written out (`--debug` and `--release` say the same). `kpp` is the name kovarex used; it would
  replace `kppc` (question 2). A file or directory given on the command line still means what it
  means today: build these sources, with no build.kpp.
- **Targets.** `project.executable(name, roots)` is one program. It gets the project's settings and
  libraries, plus its own. Each target is a whole program: two executables from one `src/` share
  the front end through the server, but each generates its own code (a cold build of both is
  about twice one). `exclude` leaves files out, so each program has exactly one `main`.
- **Configurations.** `debug` and `release` are built in. `project.configuration("asan", "debug")`
  adds one based on another ("Targets and configurations", below).
- **Steps** are for what is not compiling Kpp or C: generating code, copying into a game
  directory. A step is a built-in (`Step::copy`) or a Kpp program in the project
  (`Step::run("tools/generate_imgui.kpp", inputs, outputs)`). kppc compiles and runs it with the
  full standard library, and only when its inputs changed. Only the root project declares steps:
  a library's build.kpp may not, so adding a library runs none of its code on your machine
  (library-registry.md, decision 5).
- **With no build.kpp** the project is the defaults: the directory's name, one executable from
  `src/`, debug and release, no libraries. `kpp new hello` writes `src/main.kpp` and a
  `.gitignore` with `build/`.

### Version 0, working now

`compiler/src/project.cpp` and `projectlib/Project.kpp` (2026-10-31); `tests/project.ps1` in
run_tests.cmd.
- **The commands.** In a directory with build.kpp, or below one, `kpp` takes executables' names,
  never sources:
  - `kpp [targets] [configurations]` builds those cells, every target when none is named, debug
    when no configuration is;
  - `kpp NAME --run ARGS` builds and runs one;
  - `--generate-visual-studio-files` and `--generate-vs-code-files` write one solution for all
    executables, whose builds run `kpp NAME CONFIGURATION --msvc-diagnostics`, so an edit of build.kpp
    needs no new project files;
  - `--set NAME=VALUE` gives a setting; `--no-server` builds without the compile server, and
    `--stop-server` stops the project's servers;
  - `kpp` alone in a console shows a menu of these, each with its command; `kpp new [DIR]` writes
    build.kpp, `src\main.kpp` and a `.gitignore`.
  An argument that is an existing file or directory makes it a plain build of those sources, as
  before.
- **How build.kpp runs.** kpp runs `kppc --run-build matrix` on build.kpp, projectlib and a
  generated `build\kpp\settings.kpp` (the `--set` values and the project's name), through the
  compile server. The interpreter (GlobalScope::runBuild) default-constructs a Project, runs
  build(project) and then projectlib's `String describe(const Project&)` (which runs configure()
  for every cell), and
  kppc prints that text, which kpp reads. Nothing is linked or run. An I/O call in build() is an
  error naming it: `build.kpp:4: a call to puts, a C function, which has no body to run at
  compile time`. The result is kept, keyed by build.kpp, projectlib, the settings and kpp itself.
- **projectlib** (`projectlib/Project.kpp`). Lists are `Vector<String>`, paths join with `+`
  (`project.cInclude(project.setting("ROOT", "C:/x") + "/include")`), and executables and C
  libraries are Deques, so the reference `executable()` returns stays valid. Commands are set by
  methods: `game.afterBuild("copy ...")`, `game.debug(command, arguments, directory)`,
  `library.standard("c++20")`.
- **Measured** on a `kpp new` project (2026-10-31, PC-PRACOVNA): the first `kpp --debug` took
  1.9 s (compiling build.kpp and the program); with nothing changed it took 0.2 s.
- **What version 0 lacks:**
  - Steps are `beforeBuild` and `afterBuild` commands of an executable, run by cmd in the
    project's directory.
  - `use()` finds libraries in directories only (below); the registry, build.lock and `kpp add`
    do not exist yet.

### Kpp libraries (working now, 2026-10-31)

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
  `version` must be `major.minor.patch`. kpp compares `minimumKpp` with its own version (0.1.0
  now, raised with each release) and refuses a library that needs a newer one.
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
  3. later, the registry; now a missing library is an error naming where it looked.
- **What kpp does with it:** it describes the library's build.kpp (the same configuration and
  `--set` settings), checks the version and minimumKpp, gives kppc its roots as the library's, with
  its scope ("Library scopes" below), and does the same for the libraries it uses, depth first. A
  library rooted at its own directory (`root(".")`, a converted C library's) keeps its build.kpp out
  of its sources (kpp passes `--exclude`); the cell's kppc options apply to its roots too, so a cell's
  `--ext .kpp` leaves out the C headers beside a converted library's Kpp. One name in two directories is an error: a
  program has one copy of each library. Only the library part of its build.kpp is read: its
  executables and their commands are never built or run, and C libraries (`cLibrary`) in a used
  library are an error until they are supported.
- **`kpp --test [names] [-- runner arguments]`** builds and runs the tests (design.md, "Native
  tests") of the executables and libraries named, all when none is: each one's roots, a
  library's test roots too, through `kppc --test` with its build's options. `kpp --test -- --filter
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
  expressions, `if constexpr` and `conditional(...)`, at declaration scope too (kppc's
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

#### Library scopes (decided 2026-11-01, design.md 16; the lookup is not in kppc yet)

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
- **`lib.unscoped = true`:** its names are reachable only bare, never as `NAME::x` (its name is for
  diagnostics only), and share the one global scope with the
  program, the other unscoped libraries and the C imports; a clash is a compile error. The
  standard library is unscoped. Meant for a project's own libraries and foundations; the registry
  shows it on the library's page.
- **Adding a scoped library never changes existing code's meaning.** There is no per-file
  `using`: a name means the same in every file.
- **What kpp gives kppc,** per root of each library a cell uses, in resolveUses' order:
  `--library-root NAME DIR` (scoped) or `--unscoped-library-root NAME DIR` (kppc's options, thread
  A's lookup); the program's own roots stay plain arguments, and the stdlib, which kppc adds itself,
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
  objects into `build\<configuration>\<name>.lib` with llvm-lib. A library's error is reported
  at its file and line in the same Error List.
- **Its configuration.** Version 1 compiles C libraries as the game's prebuilt ones are: /MT /O2
  in both configurations, so that one CRT is linked. A per-library debug setting comes with a
  real need.
- **The flags are a short list:** include, define, language, `warning(off)`. There is no
  free-form flag, so a library's build stays readable and the registry (library-registry.md, "C
  dependencies") can take the same list.
- **Factorio's 18 libraries** become 18 `cLibrary` entries whose sources and defines come from
  fbuild.bff's Library() sections. Converting those by hand once is C's or E's work, depending on
  kovarex's answer.

### Adding a library

**Writing the line adds the library.** `project.use("json")` (or `game.use(...)` for one target)
is the whole act. On the next build kppc does the following:
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
last `use` call. kppc parses Kpp, so it knows where that is.

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
    record, kppc can show the fork's diff and say when upstream has a newer release.
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
  build.lock         written by kppc when there are libraries; committed
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
- **One-file programs** stay one command: `kpp --run hello.kpp` needs no directory.

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
  Executable& tests = project.executable("factorio-tests", {"src", "tests"});
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
  one (made if new, with no base: not optimized, no debug info, `config::debug` true as kppc's
  default), `project.configuration(NAME, BASE)` makes one based on another: the base's
  settings come first, then its own, resolved after build() (a change to debug later in build()
  reaches fastdebug too). A configuration holds:
  - `config(NAME, VALUE)`: a `config::` constant (bool, integer or string);
  - `optimize`: clang -O2 and ThinLTO and a whole link (kppc `--optimize`), or -O0 and an
    incremental link;
  - `debugInfo`: line tables and a PDB;
  - `clangFlag(F)` (kppc `--clang-flags`) and `argument(OPTION)` (any kppc option).

  The built-in two (thread H's contents, adopted):

  | | debug | release |
  |---|---|---|
  | `config::debug`, so `assert` | true: asserts in | false: `assert` compiles to nothing, `release_assert` stays |
  | `optimize` | false: `-O0`, incremental link | true: `-O2` + ThinLTO, lld, a whole link |
  | `debugInfo` | true | false (question 1) |
  | kppc trivial-function inlining | on | on |
- **config constants** are set for the project (`project.config`), for a configuration, for a
  target (`tests.config`) and for a cell (configure()); each later one overrides: project,
  configuration, target, cell. The command line's `kpp --config NAME=VALUE` overrides them all.
  They reach the program as kppc's `--config NAME=VALUE`, so the program reads `config::NAME`.
- **configure(Cell& cell)** is optional. The interpreter runs it once per cell after build(), on
  a copy of the cell's target and configuration: `cell.target` and `cell.configuration` are
  readable and changeable for that cell only (`cell.target.linkFile(...)`,
  `cell.target.afterBuild(...)`, `cell.configuration.clangFlag(...)`), and `cell.config(NAME, VALUE)`
  sets a constant for the cell. It still cannot act. A build.kpp without one gets projectlib's,
  a template that does nothing (a function is chosen over a template). It runs for every cell, not
  lazily for the ones built: the description is computed once and kept (above), the Visual Studio
  files and the language server need every cell, and a cell costs little. Measured 2026-11-01 on
  factorio-in-kpp's build.kpp (`kppc --quiet --run-build matrix projectlib build.kpp
  build\kpp\settings.kpp`, 15 interleaved rounds, the machine shared with other builds): 272 ms at
  best with 4 cells (2 targets, debug and release), 299 ms with 6 (fastdebug added).
- **The command line.** `kpp TARGET CONFIGURATION`, in either order: `kpp factorio-tests release`.
  A name is a configuration's or a target's, never both (a description naming both is an error).
  No target: every target; no configuration: debug. Several names build every cell they make;
  `--debug` and `--release` still mean `debug` and `release`. `kpp --test [names]` runs the tests
  of those targets and libraries in those configurations. A name that is also an existing file or
  directory is still a plain build of those sources, so a target is best not named like a
  directory (`tests`).
- **Outputs** go to `build/<configuration>/<target>.exe`, with kppc's object cache beside it. A cell's
  kppc arguments differ from every other cell's (its configuration's options and constants, its
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

## Project files for an IDE

These build, run and debug. IntelliSense (errors as you type, hover, go to definition, completion)
comes from the language server, `kpp --lsp`: [editors.md](editors.md) has it for VS Code, Visual
Studio and other editors.

**kppc generates them** (decided by kovarex 2026-10-31: no hand-written project files):

```
kppc --generate-visual-studio-files --generate-vs-code-files <build options> --exe OUT.exe SOURCES...
```

- **Visual Studio:** `build\visual-studio\<name>.sln`, `.vcxproj` and `.vcxproj.filters`. Every
  source file is listed, in folders by source root and directory, the standard library as
  *Standard Library*.
- **VS Code:** `.vscode\tasks.json`, `launch.json` and `settings.json`. A `.vscode` file kppc
  did not write is never overwritten.

Both have debug (`--debug-info`, incremental link) and release (`--release`), each with its
executable in `build\<configuration>\`. They build through the compile server with
`--msvc-diagnostics`. Sources are passed as absolute paths, so errors carry absolute paths: the
IDE opens a relative path against the project file's directory, which is not the build's.
Options that build.kpp's targets and steps will give later:
- `--debug-command`, `--debug-arguments`, `--debug-directory`: what the debugger starts;
- `--before-build` and `--after-build`: commands around the build, with `{exe}` and
  `{configuration}` in them.

The code is `compiler/src/generate.cpp`; `tests/generate.ps1` (in run_tests.cmd) does the
following:
1. Generates both for tests/run/hello.kpp.
2. Builds debug and release with MSBuild and runs each program.
3. Checks that an error reaches MSBuild as `<absolute path>(line): error :`.

Usage: [tools/vs/README.md](../tools/vs/README.md). programs/factorio's are written by
`tools\vs\factorio\generate.cmd`. Measured on its generated solution, MSBuild from the command
line, warm server, the converted tree of 783d327 (whose 123 errors against today's stdlib are the
tree's age, and `--keep-going` links anyway): 10 s, including native\build.cmd's check and the
copy into the game's directory. VS Code is not installed on PC-PRACOVNA: its files are checked
as JSON only, not run.

**Why a Makefile `.vcxproj`.** It is Visual Studio's own "external build" project.
- Ctrl+Shift+B runs kppc through its server.
- F5 builds, then starts the program under the debugger.
- The Debug/Release dropdown maps to the configurations.
- `.kpp` files are in Solution Explorer, and no C++ IntelliSense runs on them.

The alternative, Open Folder with `.vs/tasks.vs.json` and `launch.vs.json`, keeps its files in the
hidden, per-user `.vs/` directory. It has no configuration dropdown, and nothing documents that
it fills the Error List from a task's output. It was not built or measured.

**The Error List.** MSBuild reads errors from a tool's output only in the MSVC form,
`file(line[,col]): error|warning [code]: text`. Verified with MSBuild 17 on a Makefile project:
kppc's `path:line: error:` gave only `MSB3073 exited with code 1`, and `path(line): error:` gave
an error with file and line. Hence `--msvc-diagnostics`. Every diagnostic's place comes from one
formatter, `sourceLocation` (source.hpp), which gives `path:line`, or `path(line)` with the
option. Every diagnostic carries its label: the code generator's gaps ("does not support ...
yet") now print as `error:` too. run_tests.cmd checks both forms on tests/diagnostics: a parse,
a checker, a codegen and a static_assert diagnostic. kppc's messages have no column, so the Error
List shows lines only. (The first version, 456eda12, rewrote kppc's output through a pipe. That
was replaced because it could misread a program's own output under `--run`.)

**Measurements** (hand-written projects, before generation;
2026-10-30, the machine shared with seven other threads, so noisy):

| | command | times |
|---|---|---|
| hello, edit→build | `kppc --quiet --exe hello.exe hello.kpp` | 130–420 ms (10 runs) |
| hello, edit→build | the same with `--server` | 170–400 ms |
| hello, edit→build | `MSBuild.exe hello.vcxproj` (hand-written then; standalone, so its process start included) | 1.2–2.5 s |
| factorio, cold (server start) | `MSBuild.exe tools\vs\factorio\factorio.vcxproj` | 38.4 s |
| factorio, warm, a comment edit | `tools\vs\factorio\build.cmd` | 6.5, 7.1, 7.9, 9.0 s |
| factorio, warm, a comment edit | `MSBuild.exe factorio.vcxproj` | 6.4, 6.5, 9.8, 21.9 s |

Of factorio's warm build, `native\build.cmd`'s up-to-date check is 0.9 s and the copy into the
game's directory 0.16 s; the rest is kppc (front end, codegen, link). Inside Visual Studio the
build runs in-process, with no MSBuild.exe start. That measurement is still to do:
`measure.ps1` opened Visual Studio, which stopped at a "Microsoft Visual Studio Account Settings"
dialog (a sign-in prompt) that only kovarex can answer. After he has signed in once:

```
powershell -File tools\vs\measure.ps1 -Project <a generated .vcxproj> -EditFile <one of its sources>
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

kovarex's goal (2026-10-31): clone a project such as factorio-in-kpp, install a Kpp release, run
`kpp` in the project's root, and build or generate Visual Studio files from there.
- **A release** is `kpp.exe` (kppc under the name users type), `stdlib\`, `projectlib\`, the
  bundled toolchain (below), the TextMate grammar, kpp.vsix and a README. kpp finds `stdlib\`,
  `projectlib\` and `bin\` next to itself. An installation needs nothing else: no Visual Studio,
  Build Tools or Windows SDK (design.md 17, stage 2; `--toolchain msvc` uses an installed Visual
  Studio's clang on request, as the game's build does).
- **The bundled toolchain in a release** is llvm-mingw's x86-64 part at its own relative paths, so
  clang finds the rest from its `bin\`: `bin\` (clang.exe, clang-19.exe, ld.lld.exe, llvm-ar.exe,
  llvm-rc.exe, llvm-dlltool.exe, libLLVM-19.dll, libclang-cpp.dll, libc++.dll, libunwind.dll and the
  two .cfg files), `include\`, `lib\clang\19\include\`,
  `lib\clang\19\lib\windows\libclang_rt.builtins-x86_64.a`, `x86_64-w64-mingw32\lib\` and
  `x86_64-w64-mingw32\share\mingw32\` (MinGW-w64's COPYING files). Also included:
  `LICENSE-llvm-mingw.txt` (the archive's LICENSE.TXT) and `NOTICE-toolchain.txt`, which gives each
  part's licence. The i686, armv7 and aarch64 trees, python, lldb, clangd and clang-tidy are left
  out.
- **`tools/release/package.ps1 -Version V`** packages a built kppc into `build\release\`. It takes
  the toolchain from `tools\sysroot\llvm-mingw.ps1` (pinned, SHA-256 checked, unpacked once into
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
  ISCC elsewhere).
- **`tools/release/kpp.iss`** is the installer. It installs into Program Files\Kpp and adds that
  directory to the system PATH (the uninstaller removes it). It checks for nothing: until stage 2,
  it warned when no Visual Studio with the Clang component was found.
- **`.github/workflows/release.yml`** is started by hand (Actions > release > Run workflow): it
  builds kppc on a windows-2022 runner, packages it, and publishes the zip and installer as the
  prerelease `build-<run number>`, version 0.1.<run number>. It ran on every master commit from
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
  checked) into build\llvm-mingw, for the tests and the package. Licences: LLVM's Apache-2.0 WITH
  LLVM-exception, MinGW-w64's headers and libraries public domain or ZPL (the DirectX headers from
  Wine LGPL-2.1+), llvm-mingw's own scripts ISC.
- **Windows' GNU dialect** (`--target windows-gnu`, the bundled toolchain's own): the same ABI, data
  layout and SEH exceptions as MSVC's, MinGW-w64's headers in the C import (their `__attribute__`,
  `__extension__` and `__restrict__` taken as MSVC's are), lld's GNU driver (`--gc-sections`,
  `-municode` for a wmain). A program imports only Windows' own DLLs (ucrtbase, the api-ms-win-crt
  set, kernel32...). `config::mingw` is true for it (CWindows.kpp's fpclassify returns MinGW-w64's
  FP_ codes). Two pieces kppc makes once into the import cache with the toolchain's tools: an import
  library of ucrtbase.dll's functions MinGW-w64's leave out (`__uncaught_exceptions`, strtold), and
  math.h's long double functions on doubles: Kpp's long double is a double, as MSVC's, and
  libmingwex's are x87's 80 bits.
- **Which toolchain a build uses:** `--toolchain bundled` or `--toolchain msvc` (Visual Studio's
  clang, found by vswhere, and its MSVC dialect, `--target windows`), and an error when that one is
  not there, never the other in its place. Without it: KPP_CLANG, else the installation's `bin\`
  when it has one, else Visual Studio's. The bundled toolchain with `--target windows` (it has no
  MSVC headers) and Visual Studio's clang with `--target windows-gnu` are errors. The build's report
  says which (`toolchain: ... (bundled, windows-gnu: links with lld)`). A compile server takes `bin\`
  next to its client's kpp.exe. A project's C libraries compile with clang's own flags under the
  bundled toolchain, with clang-cl's (`clang --driver-mode=cl`) under Visual Studio's.
- **The link:** the bundled toolchain links with lld, debug builds too. `--incremental-link`
  (link.exe `/INCREMENTAL`, the debug edit loop's link) is Visual Studio's: with the bundled
  toolchain it is an error, and a project's debug build leaves it out. Measured on the converted
  game (2026-11-02, the compile server's leaf and wide edits and each link alone, interleaved):
  lld-link's debug link with a PDB takes 5.7 to 9.7 s, 80% of it merging the 5.2 M type records of
  the Visual Studio-built libraries' debug information (the PDB 485 MB, link.exe's 77 MB), against
  0.4 to 1.5 s for link.exe's incremental relink; without a PDB lld-link takes 1.1 to 1.4 s. The
  game, which links Visual Studio-built C++ libraries, keeps `--toolchain msvc` and
  `--incremental-link`. The release link (ThinLTO) is the same with lld 19.1.5 and 19.1.7: 52.9 to
  66.6 against 53.6 to 74.9 s on a busy machine, the executable 44.0 MB both.
- **Checked:** tests\toolchain.ps1 (an installation made of llvm-mingw and this kppc: its default,
  `--toolchain msvc`, the errors, a project's debug link, and that its programs import Windows' DLLs
  alone), tests\windowsgnu.ps1 (the 45 end-to-end programs in the GNU dialect), and
  `run_spec.py --target windows-gnu` (KPP_CLANG naming llvm-mingw's clang; 2026-11-02: every
  snippet the default target passes).

## Continuous integration

`.github/workflows/ci.yml` (thread H, 2026-10-31) runs kpp's tests on a windows-2022 runner,
or on the runner named by the repository variable `KPP_CI_RUNNER`.
Triggers (kovarex, 2026-10-31, chosen from the costs below):
- **Every push to master and every pull request:** it builds kppc with `compiler\build.cmd`, then
  runs `compiler\tests\run_tests.cmd` and `compiler\tests\spec.cmd --no-doc` (debug).
- **Nightly at 03:17 UTC, and when started by hand** (Actions > ci > Run workflow): the same, plus
  `spec.cmd --no-doc --release` and `spec.cmd --no-doc --debug-info`. The time is outside
  06:00 to 09:00 Pacific.
- **Every pull request, besides:** the `signoff` job checks that each of its commits (merges
  aside) has a `Signed-off-by` line with the author's e-mail (`tools/ci/check-signoff.sh BASE HEAD`,
  [CONTRIBUTING.md](../CONTRIBUTING.md) "Sign-off"). It runs on an ubuntu-latest runner, whose
  minutes cost half a Windows runner's (`KPP_CI_LINUX_RUNNER` names a Forgejo runner's label).
- **What it leaves out:** `programs\factorio\convert.cmd` and everything else of the game. They need
  C:\Factorio, private source that is not on the runner; factorio-in-kpp's own CI is the game's check.
- **SDL3 headers:** the two interop snippets that import SDL3/SDL.h get upstream SDL 3.4.18's headers
  through `KPP_SDL_INCLUDE` (run_spec.py). That is the game's release; the game's copy adds only
  `SDL_GetDisplayInternalDriverData`. The zip comes from kpp.net's mirror,
  `https://kpp.net/registry/mirror/<sha256>/SDL3-devel-3.4.18-VC.zip`, or from SDL's release on
  github.com until it is there. Either copy must have the SHA-256 in ci.yml (`SDL_SHA256`), or the
  step fails. A master run that had to fall back mirrors the zip, when it has kpp.net's key.
- **Clang for the snippets' C sources:** clang comes from vswhere through `KPP_CLANG`, because the
  runner's Visual Studio is Enterprise. Node.js must be on the runner, for run_tests' LSP smoke test
  and the package's VS Code extension; the step fails without it.
- **Publishing:** every green run on master (push, nightly or by hand) installs Inno Setup
  (`choco install innosetup`, as release.yml does) and packages kpp with `tools/release/package.ps1`,
  including the package's own smoke test: the zip, the installer `kpp-<version>-setup.exe` and
  kpp.vsix. The version is `0.1.<run number>` (semver, which vsce requires, and it only rises). `tools/release/publish.ps1`
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
    green master run's, push, nightly or by hand (kovarex, 2026-11-02), titled "Kpp <version>",
    with the three files. A push or hand-started run's tag is `v0.1.<run>`; the nightly run's is
    `v0.1.<date>` (UTC, as `v0.1.20261103`), titled "Kpp <version> (nightly <date>)", one durable
    release per day (a second nightly run on the same day replaces it). On GitHub a release that
    exists is updated in place (its tag moved, the new files uploaded beside the old ones, then the
    old ones removed), so a failed step leaves the old release whole. GitHub refuses the job's
    token a tag on a commit whose `.github/workflows` differ from master's at that moment (403
    "Resource not accessible by integration"): a run whose commit predates a workflow change pushed
    while it ran cannot publish, and says so; the next run publishes.

  `tools/release/settle.ps1` then makes the release of the highest run number GitHub's "Latest",
  which https://github.com/kovarex/kpp/releases/latest and the repository's sidebar show, and
  factorio-in-kpp's CI downloads. It goes by the run number in the release's notes ("ci run <N>"),
  not by which run finished last: push, nightly and hand-started runs have concurrency groups of
  their own, so an older run can finish after a newer one, and it then leaves the newer one Latest.
  It also deletes the push releases (`v0.1.<run>`) beyond the newest 20, each with its tag; the
  nightly ones are never deleted. From 2026-11-02 until this, a push run published only the rolling
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
- **No caches:** checkout gives every source a new time, so Ninja would rebuild a cached CMake build
  directory anyway, and a clean build takes about a minute. kppc's import cache (%TEMP%\kpp-imports)
  is not worth a cache action.
- **Concurrency:** one run at a time per trigger and branch. A push run on master that has started
  finishes, so a green one publishes; of the pushes meanwhile, only the newest waits (GitHub
  cancels an older waiting run), so at most one run waits. Until 2026-11-02 a newer push cancelled
  the running one, and while pushes came every few minutes nearly every push run was cancelled
  before it could publish. A pull request's newer push still cancels its older run. The nightly and
  hand-started runs have groups of their own, so a push does not cancel them.

### GitHub Actions and Forgejo Actions

The repositories' home is to be a Forgejo forge on kpp.net, with GitHub as a mirror (kovarex,
2026-10-31; thread I designs it). So ci.yml runs on both:
- every step is a script (Windows PowerShell 5.1 or cmd) except `actions/checkout@v4`;
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
  - **`runs-on`:** a Forgejo runner is self-hosted, with labels of its own, so the repository
    variable `KPP_CI_RUNNER` must name its label. Whether Forgejo expands `vars` in `runs-on` has not
    been checked; if it does not, the label is written in place of the expression.
  - **The runner itself:** Forgejo hosts no runners. A Windows machine with Visual Studio 2022 and
    its Clang component, Node.js and Python runs `forgejo-runner` in host mode.
  - **`concurrency`:** GitHub's one run at a time, with one waiting (and a pull request's older run
    cancelled). Forgejo Actions may not honour it; there, every push runs, side by side.
  - **The SDL3 headers** come from kpp.net's mirror, with libsdl-org's release on github.com
    (`SDL_FALLBACK`) as the fallback until the zip is mirrored.
  - **The job's token:** `github.token` on both. On Forgejo it must be allowed to write releases.

Cost, estimated before any runner run. These are measured on PC-PRACOVNA pinned to 2 logical CPUs
(`start /affinity 3`), a stand-in for the 2-vCPU Windows runner a private repository gets, and on
the full machine:

| Step | 2 CPUs | 32 CPUs |
|---|---|---|
| clean kppc build | 68 s | |
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
run but bill each runner's checkout and 2-minute kppc build again.

## Questions for kovarex

Answered 2026-10-31: the build description is `build.kpp` in Kpp, with `src/` and `libraries/`
(above); project files for an IDE are generated by kppc.

1. **Release builds with debug info** (a PDB for profiling and crash stacks), or without?
2. **The command's name**: a release installs it as `kpp.exe` (kovarex writes `kpp`), and this
   repository still builds `compiler\build\kppc.exe`, which every script calls. Rename it in the
   repository too, or keep the two names?
3. **The C libraries the game links** are FASTBuild's April build; `libraries/` changed 54 times
   since, and `C:\Factorio` is read-only. Options: build them with FASTBuild into a directory
   outside `C:\Factorio` (if fbuild.bff allows an output root), build them with kppc's clang
   as libraries with a build.kpp listing their C sources, or accept prebuilt libraries and
   rebuild them by hand when `libraries/` changes. What is at stake (thread C's comparison of
   the headers at 4c178a5b915 with the .libs, built at d0495ebfd8a): SDL3 3.3.4 → 3.4.18
   (sdl.lib), SDL_Mixer 3.1.2-pre → 3.2.4 (sdl-mixer.lib), zlib 1.2.7 → 1.3.2 (zlib.lib). Once
   the new source is converted, the game compiles against SDL 3.4 headers and links SDL 3.3
   code. Not mismatches: crc32fast (code only), curl (its osx configuration), and Lua 5.2.1 (6
   files; the game's Lua is converted from source, and lua.lib is used only by the lua_compat
   check, which therefore compares against the old Lua).
