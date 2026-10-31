# Building Kpp programs

How a Kpp program is built today, where FASTBuild fits in, a proposal for describing a build
(a `build.kpp` in Kpp, the project layout, named configurations), and the Visual Studio
integration (`tools/vs/`). The proposal waits on kovarex's answers to the questions at the end.
Nothing in it is implemented until he answers questions 1 and 2.

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
write their own kppc line: 6 source roots and 4 (link) or 9 (run) more options. `tools/vs/factorio/build.cmd`
is a third copy of run.cmd's line. Three copies of one build is what a project file removes.

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
them with FASTBuild writes into `C:\Factorio`, which is read-only. See question 4.

## Proposal: the build description

What kovarex asked for (2026-10-31): one description of a project's builds (targets, steps,
libraries) for every tool, not only Visual Studio; no special build language such as CMake's or a
script's, so research writing it in Kpp, sandboxed; starting must take nothing (no file, or a tiny
generated one, gives debug and release); and one layout for the program's own code, its own
libraries and forks, and libraries taken unchanged from the registry
([library-registry.md](library-registry.md)).

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
1. **The description computes and declares; it does not act.** Running a tool is a step that
   names its inputs and outputs, and the build tool runs it and caches its result (Bazel, Swift's
   plugins, Zig's step graph). Where the description language can act (Cargo's `build.rs`, Zig,
   Jai, Nim), every dependency runs code on the developer's machine, and the build depends on the
   machine it ran on.
2. **Defaults carry most programs.** Go needs no file; Cargo's is 6 lines; Jai and Zig compile a
   lone file.
3. **Tools must edit the dependency list.** Zig put it in a data file (`build.zig.zon`) next to the
   code; Swift's `Package.swift` is code, and editing it from a tool came years late, through a
   Swift parser.

Kpp is placed unusually well for point 1: its compile-time interpreter (design.md, "Compile-time
evaluation") already runs ordinary Kpp with no I/O and no C calls, under step and memory limits.
That is Starlark's sandbox without a second language.

### build.kpp, run by the interpreter (recommended)

- **The build file is Kpp**: `build.kpp` in the project's root directory. kppc compiles it on its
  own, with the standard library and a small `build` package (`Project`, `Step`, `Dependencies`),
  and runs it in the interpreter. It may compute anything: loops, functions, the standard
  library. It can only describe: it fills in a `Project` value, and kppc builds what that value
  says. It cannot read a file, start a process or call C. This is the interpreter's existing
  limit, so it needs no separate sandbox.
- **Its errors are Kpp errors.** They come with the same diagnostics and go to the same Error
  List, colouring and IDE navigation as the program's own code. There is no second syntax.
- **The program does not see build.kpp's declarations.** The `config::` constants are what
  crosses over: the build sets them per configuration, and the program reads them as today.
- **Inputs from outside come only from the command line.** `kppc --set FACTORIO=D:\snapshot`
  is read as `project.setting("FACTORIO", "C:/Factorio")`, so a build depends only on its files
  and its arguments. The arguments are part of the server's key.
- **Cost:** the server evaluates the file once per configuration and keeps the result until the
  file changes. To be measured once it exists.

The functions kppc calls, all of them optional:

```cpp
// build.kpp: the libraries this project uses. One call per line and nothing else,
// so that `kppc add` and `kppc remove` can edit it without a Kpp parser of their own.
void dependencies(Dependencies& d)
{
    d.use("sdl3", "3.4");        // registry name, minimum version
    d.use("lua", "5.4");
}

// More configurations than the built-in debug and release, each based on one of them.
void configurations(Configurations& c)
{
    c.add("asan", "debug");
}

// Called once per configuration. Before the call, project holds the defaults for that
// configuration: the name, sources and output from the layout below, and H's settings.
void build(Project& project)
{
    if (project.configuration == "debug")
        project.debugInfo = true;
    if (project.configuration == "asan")
        project.clangFlags.add("-fsanitize=address");
}
```

**With no build.kpp** the project is the defaults: the directory's name, `src/` as the sources,
an executable, debug and release, no libraries. `kppc new hello` writes `src/main.kpp`, a
`.gitignore` with `build/`, and no build.kpp at all.

**programs/factorio** today needs three .cmd files (link-settings, link, run). As one file:

```cpp
// programs/factorio/build.kpp
void build(Project& project)
{
    StringView factorio = project.setting("FACTORIO", "C:/Factorio");
    StringView converted = project.setting("FIK", "C:/factorio-in-kpp/kpp");
    project.sources = {};
    for (StringView root : {"CommonUtil", "Agui", "cxxopts", "StackWalker", "Lua", "src"})
        project.sources.add(path(converted, root));
    for (StringView lib : {"SDL3/include", "SDL_Mixer/include", "Lua", "", "curl/include", "glad/include", "trio"})
        project.cIncludes.add(path(factorio, "libraries", lib));
    for (StringView system : {"gdi32", "user32", "opengl32", "winmm" /* ... */})
        project.systemLibraries.add(system);
    project.debugInfo = project.configuration == "debug";
    project.steps.add(Step::copy(project.output, path(project.setting("GAME", "C:/factorio-in-kpp"), "bin/x64")));
}
```

The C shims (`native/`, built by a .cmd today) become a library, `libraries/factorio-native`, whose
build.kpp lists its C and C++ files. The C:\Factorio libraries become registry libraries or
libraries in `libraries/`, depending on question 4.

**Steps** are for what is not compiling Kpp or C, such as generating code or copying into a game
directory. A step is either a built-in (`Step::copy`) or a Kpp program in the project
(`Step::run("tools/generate_imgui.kpp", inputs, outputs)`). kppc compiles and runs it with the
full standard library, and runs it again only when its inputs change. That covers the remaining
scripts, written in Kpp too. Only the root project may declare steps. A library's build.kpp may
not, so adding a library still runs none of its code on your machine (library-registry.md,
decision 5).

**What it needs before it works:**
- **Allocation in the interpreter.** `new` and `delete` are not evaluated today (C++20's
  transient allocation), so a build file could not use `Vector` or `String`. This is thread D's
  work. Until then build data must be arrays and string literals, which is too tight for the
  factorio example.
- **A way for kppc to read the resulting `Project` value back** into the driver. This is B's
  driver and E's.
- **The `build` package** in the standard library (C's).
- **The commands** `kppc build [configuration]`, `kppc run` and `kppc new`, which find build.kpp
  upward from the current directory.

**The alternative**, a list of kppc's options with `[configuration]` sections (this document's
first proposal), needs none of that and could be done in a day. But it is exactly the "special
language" that grows conditionals and variables until it is CMake.

### One layout

```
hello/
  build.kpp          optional
  build.lock         written by kppc when there are dependencies; committed
  src/               the program's code: the top-level package, named after the project
  libraries/         libraries kept in this repository: your own, and forks; committed
    json/            a fork: replaces the registry's json everywhere in the program
      build.kpp      a library is a project with no executable
      src/
  build/             outputs, never committed
    debug/hello.exe
    release/hello.exe
```

- **Registry libraries are not in the tree.** They sit in a read-only shared cache
  (`%LOCALAPPDATA%\kpp\libraries\<name>-<version>-<hash>\`), named in `dependencies` and pinned with
  their hashes in build.lock, so they are 1:1 with what the registry serves.
- **A directory in `libraries/` named like a registry library replaces that library** for the
  whole program, its other libraries included. A program has one version of each library anyway
  (the one global scope).
  - `kppc fork json` copies the locked version there and records what it was forked from.
  - From that record, kppc can show the fork's diff to upstream and say when upstream has a
    newer release.
  - Deleting the directory goes back to the registry's version.
- **A directory in `libraries/` with a new name** is your own library, used by the program
  without a registry.
- **The IDE's package tree** shows *Program*, *Libraries* (own, and forks marked as such),
  *Registry* (read-only) and *Standard Library*.
- **One-file programs** stay one command: `kppc --run hello.kpp` needs no directory, as today.

### Named configurations

`debug` and `release` are built in, so a project that defines neither still has both. Their
contents are thread H's, adopted:

| | debug | release |
|---|---|---|
| `config::debug`, so `assert` | true: asserts in | false: `assert` compiles to nothing, `release_assert` stays |
| kppc trivial-function inlining | on | on |
| clang | `-O0` | `-O2` + ThinLTO, lld, its LTO cache |
| link | incremental allowed | whole |

Debug info is a field (`project.debugInfo`), not a configuration. Question 3 asks whether release
should have it by default. Each configuration's output goes to `build\<configuration>\`, so
configurations never share an object cache.

The converted game has a second axis apart from these: the Factorio defines the converter
preprocesses with. `game-fastdebug` (_DEBUG, DEBUG, LUA_USE_APICHECK) is the only one today; a
`game-release` would need a second conversion. It belongs to the conversion, not to kppc's
configurations, and the two axes never share names.

### What changes for Visual Studio

The `.vcxproj` (below) stops naming sources and options. It runs
`kppc build $(Configuration) --server --msvc-diagnostics` in the project's directory, and the
configurations dropdown lists the project's configurations. `tools/vs/factorio/build.cmd`
disappears into factorio's build.kpp.

## Visual Studio

Files are in `tools/vs/` ([README](../tools/vs/README.md)): `Kpp.props`, which a program's 25-line
`.vcxproj` imports; `hello/` and `factorio/` projects; a TextMate grammar for colouring
(`install.cmd`); and `measure.ps1`, which drives a fresh Visual Studio through DTE to time
edit→build and check the Error List.

**Chosen: a Makefile `.vcxproj`.** It is Visual Studio's own "external build" project.
- Ctrl+Shift+B runs kppc through its server.
- F5 builds, then starts the program under the debugger.
- The Debug/Release dropdown maps to the configurations.
- `.kpp` files are in Solution Explorer, and no C++ IntelliSense runs on them.

The alternative, Open Folder with `.vs/tasks.vs.json` and `launch.vs.json`, keeps its files in the
hidden, per-user `.vs/` directory. It has no configuration dropdown, and nothing documents that
it fills the Error List from a task's output. It was not built or measured, because the Makefile
project needs no more files (one 25-line `.vcxproj` per program).

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

**Measurements** (2026-10-30, the machine shared with seven other threads, so noisy):

| | command | times |
|---|---|---|
| hello, edit→build | `kppc --quiet --exe hello.exe hello.kpp` | 130–420 ms (10 runs) |
| hello, edit→build | the same with `--server` | 170–400 ms |
| hello, edit→build | `MSBuild.exe hello.vcxproj` (standalone, so its process start included) | 1.2–2.5 s |
| factorio, cold (server start) | `MSBuild.exe tools\vs\factorio\factorio.vcxproj` | 38.4 s |
| factorio, warm, a comment edit | `tools\vs\factorio\build.cmd` | 6.5, 7.1, 7.9, 9.0 s |
| factorio, warm, a comment edit | `MSBuild.exe factorio.vcxproj` | 6.4, 6.5, 9.8, 21.9 s |

Of factorio's warm build, `native\build.cmd`'s up-to-date check is 0.9 s and the copy into the
game's directory 0.16 s; the rest is kppc (front end, codegen, link). Inside Visual Studio the
build runs in-process, with no MSBuild.exe start. That measurement is still to do:
`measure.ps1` opened Visual Studio, which stopped at a "Microsoft Visual Studio Account Settings"
dialog (a sign-in prompt) that only kovarex can answer. After he has signed in once:

```
powershell -File tools\vs\measure.ps1 -Project tools\vs\hello\hello.vcxproj -EditFile tools\vs\hello\hello.kpp
```

**Debugging.** F5 starts the executable under Visual Studio's debugger. The Debug configuration
builds with `--debug-info` (ee17f9a8, [debugging.md](debugging.md)): hello's PDB names
`hello.kpp`. Stepping and breakpoints in Visual Studio have not been tried yet, for the same
sign-in reason as the timings. programs/factorio builds Debug with it too:
all 64 modules, 29.1 s through MSBuild on a warm server, a 268 MB PDB naming the converted
`.kpp` files, and the game runs. Before B's fix edb182a4 (an exception leaving a catch handler
unwound to the caller, a miscompile in every build), clang rejected most modules with debug
info. That failure was hidden by `--quiet`, so factorio's `build.cmd` prints an error line
whenever kppc exits non-zero.

**Colouring.** `tools/vs/kpp/Syntaxes/kpp.tmLanguage.json`, a TextMate grammar: Visual Studio
reads these from `%USERPROFILE%\.vs\Extensions\` when it starts, with no extension to build or
install. `install.cmd` links it there.

## Questions for kovarex

1. **The build file in Kpp**: `build.kpp`, run by the compile-time interpreter, which only
   describes (recommended); or a list of kppc options with `[configuration]` sections?
2. **The layout**: `src/`, `libraries/` (your own and forks, a fork replacing the registry's
   library of its name), `build/`, registry libraries in a shared cache outside the tree. Are the
   names and the replacement rule right?
3. **Release builds with debug info** (a PDB for profiling and crash stacks), or without?
4. **The C libraries the game links** are FASTBuild's April build; `libraries/` changed 54 times
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
5. **One `.vcxproj` per program, written by hand** (25 lines, recommended), or generated by
   `kppc --vcxproj` from build.kpp?
