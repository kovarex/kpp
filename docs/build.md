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
| `--lib F` | passed to the link: a `.lib`, `-lname`, `-Wl,/OPT`, a `.res` |
| `--cinclude D` | a directory searched by the C headers the code imports |
| `--clang-flags F` | more flags for clang's compile of the modules (`-fsanitize=address`) |
| `--incremental-link` | debug builds only: link.exe `/INCREMENTAL`, so the next link redoes only changed objects |
| `--no-inline` | kppc emits no trivial function into its callers (a diagnostic tool; inlining is on by default) |
| `--keep-going`, `--stub-missing` | bring-up tools: link what was emitted despite errors, stub what is missing |
| `--server` | build through a persistent compile server ([compile-speed.md](compile-speed.md)) |
| `--msvc-diagnostics` | errors as `path(line): error: ...`, which Visual Studio's Error List reads |

**The only configuration switch is `config::debug`.** Its default is true. It decides:

| | `config::debug` true (default) | false (`--release`) |
|---|---|---|
| `assert` | compiled in | compiles to nothing (`release_assert` stays) |
| clang | `-O0` | `-O2`, ThinLTO when there are several modules (lld runs the back ends) |
| link | link.exe, `--incremental-link` allowed | lld, whole program; `--incremental-link` is an error |
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
  interpreter, once per configuration. The function can compute (loops, functions, the standard
  library), but it can only describe: it fills in a `Project`, and kppc builds what that says.
- **It cannot act.** It cannot read a file, start a process or call C. That is the interpreter's
  existing limit, so no separate sandbox is needed.
- **Its errors are Kpp errors**, with the same Error List, colouring and IDE navigation.
- **The program does not see build.kpp's declarations.** What crosses over are the `config::`
  constants the build sets.
- **Inputs from outside come only from the command line.** `kpp --set FACTORIO=D:\snapshot` is
  read as `project.setting("FACTORIO", "C:/Factorio")`, so a build depends only on its files and
  its arguments. The arguments are part of the server's key.
- **Its cost:** the server evaluates it once per configuration and keeps the result until the file
  changes. To be measured.

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
    project.debugInfo = project.configuration == "debug";

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
  kpp --release           every target, release
  kpp factorio --debug    one target
  kpp --run factorio      build it, then run it
  kpp --test              build and run the tests
  ```

  `--debug` is the default and may be written out. `kpp` is the name kovarex used; it would
  replace `kppc` (question 2). A file or directory given on the command line still means what it
  means today: build these sources, with no build.kpp.
- **Targets.** `project.executable(name, roots)` is one program. It gets the project's settings and
  libraries, plus its own. Each target is a whole program: two executables from one `src/` share
  the front end through the server, but each generates its own code (a cold build of both is
  about twice one). `exclude` leaves files out, so each program has exactly one `main`.
- **Configurations.** `debug` and `release` are built in. `project.configurations.add("asan", "debug")`
  adds one based on another.
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
  - `kpp [names] [--debug|--release]` builds them, all of them when none is named;
  - `kpp NAME --run ARGS` builds and runs one;
  - `--generate-visual-studio-files` and `--generate-vs-code-files` write one solution for all
    executables, whose builds run `kpp NAME --debug --msvc-diagnostics`, so an edit of build.kpp
    needs no new project files;
  - `--set NAME=VALUE` gives a setting; `--no-server` builds without the compile server, and
    `--stop-server` stops the project's servers;
  - `kpp` alone in a console shows a menu of these, each with its command; `kpp new [DIR]` writes
    build.kpp, `src\main.kpp` and a `.gitignore`.
  An argument that is an existing file or directory makes it a plain build of those sources, as
  before.
- **How build.kpp runs.** kpp runs `kppc --run-build debug,release` on build.kpp, projectlib and a
  generated `build\kpp\settings.kpp` (the `--set` values and the project's name), through the
  compile server. The interpreter (GlobalScope::runBuild) default-constructs a Project, sets its
  configuration, runs build(project) and then projectlib's `String describe(const Project&)`, and
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
  - `use()` and the registry do not exist yet; there is no `configurations()` either.

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

### Named configurations

Their contents are thread H's, adopted:

| | debug | release |
|---|---|---|
| `config::debug`, so `assert` | true: asserts in | false: `assert` compiles to nothing, `release_assert` stays |
| kppc trivial-function inlining | on | on |
| clang | `-O0` | `-O2` + ThinLTO, lld, its LTO cache |
| link | incremental allowed | whole |

Debug info is a field (`project.debugInfo`), not a configuration. Question 1 asks whether release
should have it by default.

The converted game has a second axis apart from these: the Factorio defines the converter
preprocesses with. `game-fastdebug` (_DEBUG, DEBUG, LUA_USE_APICHECK) is the only one today; a
`game-release` would need a second conversion. It belongs to the conversion, not to kppc's
configurations, and the two axes never share names.

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
  TextMate grammar and a README. kpp finds `stdlib\` and `projectlib\` next to itself, and clang in
  the newest Visual Studio with the Clang component, any edition (vswhere). So an installation
  needs Visual Studio 2022 with "C++ Clang Compiler for Windows", which also supplies the linker
  and the Windows SDK.
- **`tools/release/package.ps1 -Version V`** packages a built kppc into `build\release\`. Its
  smoke test runs `kpp new` and `kpp --no-server --run` with the packaged kpp outside the
  repository. It then writes the zip, and the installer when Inno Setup 6 is installed.
- **`tools/release/kpp.iss`** is the installer:
  - it installs into Program Files\Kpp and adds that directory to the system PATH (the
    uninstaller removes it);
  - it warns when vswhere finds no Visual Studio with the Clang component.
  Not yet built or run on PC-PRACOVNA, which has no Inno Setup.
- **`.github/workflows/release.yml`** is started by hand (Actions > release > Run workflow): it
  builds kppc on a windows-2022 runner, packages it, and publishes the zip and installer as the
  prerelease `build-<run number>`, version 0.1.<run number>. It ran on every master commit from
  e0ca8975 until the coordinator made it manual (2026-10-31): master gets dozens of commits a day,
  and no run had been checked. An automatic trigger (per tag, or nightly) comes once kovarex has
  seen a manual run succeed. The repository is private, so its runs use the account's Actions minutes (Windows
  minutes count double) and its releases are visible only to its collaborators.

## Continuous integration

`.github/workflows/ci.yml` (thread H, 2026-10-31) runs kpp's tests on a windows-2022 runner,
or on the runner named by the repository variable `KPP_CI_RUNNER`.
Triggers (kovarex, 2026-10-31, chosen from the costs below):
- **Every push to master and every pull request:** it builds kppc with `compiler\build.cmd`, then
  runs `compiler\tests\run_tests.cmd` and `compiler\tests\spec.cmd --no-doc` (debug).
- **Nightly at 03:17 UTC, and when started by hand** (Actions > ci > Run workflow): the same, plus
  `spec.cmd --no-doc --release` and `spec.cmd --no-doc --debug-info`. The time is outside
  06:00 to 09:00 Pacific.
- **What it leaves out:** `programs\factorio\convert.cmd` and everything else of the game. They need
  C:\Factorio, private source that is not on the runner; factorio-in-kpp's own CI is the game's check.
- **SDL3 headers:** the two interop snippets that import SDL3/SDL.h get upstream SDL 3.4.18's headers
  through `KPP_SDL_INCLUDE` (run_spec.py). That is the game's release; the game's copy adds only
  `SDL_GetDisplayInternalDriverData`.
- **Clang for the snippets' C sources:** clang comes from vswhere through `KPP_CLANG`, because the
  runner's Visual Studio is Enterprise. Node.js must be on the runner, for run_tests' LSP smoke test
  and the package's VS Code extension; the step fails without it.
- **The "latest" prerelease:** every green run on master (push, nightly or by hand) packages kpp with
  `tools/release/package.ps1 -Version 0.1.0-ci.<run>` (semver, which vsce requires), including the package's own smoke test. It
  replaces the prerelease `latest` with that zip, which is what factorio-in-kpp's CI downloads.
  `tools/release/publish.ps1` does the replacing through the forge's REST API, with the job's token.
  Numbered releases stay with release.yml.
- **No caches:** checkout gives every source a new time, so Ninja would rebuild a cached CMake build
  directory anyway, and a clean build takes about a minute. kppc's import cache (%TEMP%\kpp-imports)
  is not worth a cache action.
- **Concurrency:** one run per trigger and branch. A newer push cancels the older run still going,
  so runs never queue back to back. The nightly and hand-started runs have groups of their own, so
  a push does not cancel them. While pushes come faster than a run takes, push runs keep being
  cancelled; then the nightly run is the one that moves "latest".

### GitHub Actions and Forgejo Actions

The repositories' home is to be a Forgejo forge on kpp.net, with GitHub as a mirror (kovarex,
2026-10-31; thread I designs it). So ci.yml runs on both:
- every step is a script (Windows PowerShell 5.1 or cmd) except `actions/checkout@v4`;
- it names no repository URL: the forge's API comes from `github.api_url` and the repository from
  `github.repository`;
- publishing is `tools/release/publish.ps1 -ApiUrl URL -Repository owner/name -Tag T -Commit SHA
  -Title T -Files F...`, with the token in `KPP_FORGE_TOKEN`. It speaks GitHub's API, or
  Forgejo's (Gitea's) when the API URL ends in `/api/v1` (`-Forge github|forgejo` overrides). Both
  paths were checked against a mock of the two APIs (create, then replace).
- What is not portable, or not checked on Forgejo:
  - **`actions/checkout@v4`:** Forgejo fetches it from its own mirror of the actions (its runner's
    default actions URL), not from github.com.
  - **`runs-on`:** a Forgejo runner is self-hosted, with labels of its own, so the repository
    variable `KPP_CI_RUNNER` must name its label. Whether Forgejo expands `vars` in `runs-on` has not
    been checked; if it does not, the label is written in place of the expression.
  - **The runner itself:** Forgejo hosts no runners. A Windows machine with Visual Studio 2022 and
    its Clang component, Node.js and Python runs `forgejo-runner` in host mode.
  - **`concurrency`:** GitHub's cancelling of an older push run. Forgejo Actions may not honour it;
    there, each push runs to the end.
  - **The SDL3 headers** come from libsdl-org's release on github.com (`SDL_ZIP`), an external
    download, not the repository's host.
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
minutes) is the other option. The numbers get replaced by a real runner's once runs have been
timed.

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
