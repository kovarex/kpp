# Building Kpp programs

How a Kpp program is built today, where FASTBuild fits in, a proposal for describing a build
(project file, named configurations), and the Visual Studio integration (`tools/vs/`). The
proposal waits on kovarex's answers to the questions at the end. Nothing in it is implemented
until he answers the format question.

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
| debug info | none (the modules carry none; B is adding opt-in `--debug-info`, CodeView) | none |
| kppc inlining of trivial functions | on | on |

**The compile server.** `--server` anywhere on the line makes kppc a client. The server is keyed by
kppc's path, the working directory and the argument list, so every distinct argument list has its
own server. A server keeps the last build's program in memory, rebuilds only what an edit touched,
and ends after an hour idle. `--stop-server` with the same arguments stops it.
`--msvc-diagnostics` is removed from the arguments before the key is made, so the Visual Studio
build and a command-line build of the same program share one server.

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
them with FASTBuild writes into `C:\Factorio`, which is read-only. See question 3.

## Proposal: the build description

### A project file

One file per program names everything its build needs: sources, C include directories,
libraries, the output, and its configurations. kppc reads it with
`kppc --project FILE [--configuration NAME]`, and command-line options still add to it.
Two candidate formats follow, using programs/factorio as the example.

**(A) A list of kppc's own options** (`factorio.kppproj`): one option per line, as on the command
line; `[name]` starts a configuration's section; `NAME = value` sets a variable that the
environment overrides; `%NAME%` expands one; paths are relative to the file.

```
# The converted game (programs/factorio).
FACTORIO = C:\Factorio
FIK = C:\factorio-in-kpp\kpp
%FIK%\CommonUtil  %FIK%\Agui  %FIK%\cxxopts  %FIK%\StackWalker  %FIK%\Lua  %FIK%\src
--exe build\%CONFIGURATION%\factorio.exe
--cinclude %FACTORIO%\libraries\SDL3\include
--cinclude native\include
--lib native\build\factorio-native.lib
--lib -lgdi32
...
[debug]
--incremental-link
--debug-info
[release]
--release
```

kppc needs nothing new except reading the file: every line is an option it already has. A reader
needs nothing new either, and the server key is the expanded list (an edit of the file gets a new
server, as a changed command line does now).

**(B) A Kpp declaration** (`project.kpp`), in the form `package.kpp` already proposes
(design.md, "Code packages"):

```cpp
project "Factorio"
{
    sources = { "%FIK%/CommonUtil", "%FIK%/Agui", "%FIK%/cxxopts", "%FIK%/StackWalker", "%FIK%/Lua", "%FIK%/src" };
    cinclude = { "%FACTORIO%/libraries/SDL3/include", "native/include" };
    libs = { "native/build/factorio-native.lib", "-lgdi32" };
    output = "factorio.exe";
}
configuration "release" { config::debug = false; }
```

It is typed and checked by the compiler, and `config::` values sit next to the rest. The cost:
kppc needs a small evaluator for it, tools must run kppc to read it, and each kppc option needs a
field.

**Recommendation: (A).** It is the leanest, and it is exactly what the .cmd scripts hold today.

### Named configurations

`debug` and `release` are built in, so a project that defines neither still has both; a project's
`[debug]` or `[release]` section adds options to the built-in set. Proposed contents:

| | debug | release |
|---|---|---|
| `config::debug`, so `assert` | true: asserts in | false: asserts out |
| kppc trivial-function inlining | on | on |
| clang | `-O0` | `-O2` + ThinLTO |
| debug info (B's `--debug-info`, CodeView PDB) | on | on (question 2) |
| link | incremental | whole (lld) |

Any further configuration (a profiling build, an AddressSanitizer build) is a `[name]` section with
its options, such as `--clang-flags -fsanitize=address --lib -fsanitize=address`. `--release`
stays as shorthand.

### Libraries, C include directories, outputs

These are the `--lib`, `--cinclude` and `--exe` lines above, nothing more. The output defaults to
`build\<configuration>\<project name>.exe` next to the project file, which keeps configurations
from sharing an object cache. C sources the program compiles itself (programs/factorio/native)
stay a script until question 3 is answered.

### What changes for Visual Studio

The `.vcxproj` (below) stops naming sources and options. It runs
`kppc --server --msvc-diagnostics --project F --configuration $(Configuration)`, and the
configurations dropdown becomes the project's configurations. `tools/vs/factorio/build.cmd`
shrinks to the copy into the game's directory.

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
an error with file and line. Hence `--msvc-diagnostics` (commit 456eda12; tested in
run_tests.cmd). It rewrites every `path:N: ` line on stdout and stderr, and calls a line with no
`warning:` or `note:` an error, since codegen's "does not support" lines carry no label. kppc's
messages have no column, so the Error List shows lines only.

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

**Debugging.** F5 starts the executable under Visual Studio's debugger. Today that gives call
stacks with function names (programs/factorio links with `/DEBUG`), but no source lines or
breakpoints in `.kpp` files, which need B's `--debug-info`. When it lands, it goes into the
debug configuration in `Kpp.props`.

**Colouring.** `tools/vs/kpp/Syntaxes/kpp.tmLanguage.json`, a TextMate grammar: Visual Studio
reads these from `%USERPROFILE%\.vs\Extensions\` when it starts, with no extension to build or
install. `install.cmd` links it there.

## Questions for kovarex

1. **Project file format:** (A) a list of kppc options with `[configuration]` sections
   (recommended), or (B) a Kpp `project "..." { }` declaration?
2. **Release builds with debug info** (a PDB for profiling and crash stacks), or without?
3. **The C libraries the game links** are FASTBuild's April build; `libraries/` changed 54 times
   since, and `C:\Factorio` is read-only. Options: build them with FASTBuild into a directory
   outside `C:\Factorio` (if fbuild.bff allows an output root), build them with kppc's clang
   from a list of C sources in the project file, or accept prebuilt libraries and rebuild them by
   hand when `libraries/` changes.
4. **One `.vcxproj` per program, written by hand** (25 lines, recommended), or generated by
   `kppc --vcxproj` from the project file?
