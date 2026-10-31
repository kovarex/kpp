# Test matrix

Every check the repository has, in each build configuration, at one kppc commit on one Factorio
source. Thread H keeps it current; each row names the command that produces it.

## The configurations

| Name | kppc flags | What differs |
|---|---|---|
| debug | (none) | `assert` checked, clang -O0, link.exe /OPT:REF /OPT:NOICF (`--incremental-link`: /INCREMENTAL, run.cmd's default) |
| release | `--release` (= `--config debug=false`) | `assert` compiles to nothing (`release_assert` stays), clang -O2 (-flto=thin with several modules), lld ThinLTO with a cache and /OPT:REF; refuses `--incremental-link` |
| debug-info | `--debug-info` | CodeView line tables and a PDB; orthogonal to the two above |

kppc's trivial inlining is on in all of them (`--no-inline` turns it off). The converted game is
always Factorio's FastDebug preprocessing (`_DEBUG`, `DEBUG`, `LUA_USE_APICHECK`: convert.cpp's
`buildDefines`), so the kppc release game is FastDebug code without asserts at -O2, not Factorio's
Release. [build.md](build.md) has the configuration names and the build description.

The commands: `compiler\tests\run_tests.cmd`; `compiler\tests\spec.cmd [--release | --debug-info]`;
`programs\factorio\convert.cmd DIR` (headline and smoke tests); `programs\factorio\run.cmd
[--release] --version` (the game builds and starts); `python -I programs\factorio\menu-checksums.py
GAME_DIR` (the 45 menu maps' checksums, with test_mod removed from the game's mods);
`programs\factorio\tests.cmd [--release] DIR` (the unit tests).

## kppc b96ea59a, Factorio 4c178a5b915 (2.1.25, 2026-10-31)

C's conversion factorio-in-kpp 40ce3c8d (a copy of it in thread H's scratch), linked against the
Releasex64 libraries of the C++ Release build below (so SDL 3.4, SDL_Mixer and zlib match the
source again). The C++ builds are C:\Factorio's own FASTBuild of the same commit, built the same day.

| Check | kppc debug | kppc release | C++ Debug | C++ Release |
|---|---|---|---|---|
| game builds | yes, 0 stubs, 21 s | yes, 0 stubs, 78 s (ThinLTO link 64.6 s) | clean 77.1 s (with factorio-test) | clean 4 min 19.6 s (with factorio-test) |
| menu maps vs the 2.1.25 baseline | 45/45 same | 45/45 same | the reference | 45/45 same as C++ Debug |

The baseline is `menu-checksums.py --baseline` run on this C++ Debug build (2.1.25, build 87847);
every checksum differs from the 2.1.0 menu-checksums.txt, which C replaces with the 2.1.25 one.

### Runtime: the 45 menu maps

`python -I bench-menu.py 3 kppc-release=GAME cpp-release=cpp:C:\Factorio\bin\Releasex64vs2022\factorio-run.exe
kppc-debug=GAME cpp-debug=cpp:C:\Factorio\bin\Debugx64vs2022\factorio-run.exe` (thread H's scratch
script): every map 600 ticks with `--benchmark`, the four builds one after another on each map, three
rounds; the time is the game's own "Performed 600 updates in X ms". The total is the sum over the 45
maps of each map's median. The machine was shared with other threads, equally for all four builds.

| Build | 45 maps | per round | vs C++ of the same configuration |
|---|---:|---|---:|
| kppc release | **8,703 ms** | 8,699 / 9,606 / 8,920 | 0.81× |
| C++ Release | 10,681 ms | 12,024 / 10,765 / 10,794 | |
| kppc debug | **22,254 ms** | 22,563 / 23,668 / 23,157 | 0.60× |
| C++ Debug | 36,911 ms | 38,677 / 38,469 / 37,177 | |

No map's checksum differs between the four. kppc release is faster than C++ Release on 43 maps
(slower on nuclear-power, 174.5 against 160.4 ms, and on oil-pumpjacks, 170.8 against 170.3 ms);
kppc debug is faster than C++ Debug on 44 (not on brutal-defeat, 2,783 against 2,708 ms). The
configurations are not the same code: kppc release is Factorio's FastDebug preprocessing (`_DEBUG`,
`DEBUG`, `LUA_USE_APICHECK`) at clang -O2 with ThinLTO and kppc's asserts compiled out, against
Factorio's own Release configuration in MSVC; kppc debug is the same preprocessing at clang -O0,
against Factorio's Debug configuration.

### Build time: C++ (FASTBuild)

`cd C:\Factorio\fastbuild` then `FBuild.exe -summary [-clean] vs2022-Debugx64-factorio-run
vs2022-Debugx64-factorio-test` (the time is the summary's `Real`). An edit is a timestamp-only touch
of the header (`(Get-Item F).LastWriteTime = Get-Date`): FASTBuild rebuilds what includes it exactly
as for a one-line change, and C:\Factorio's sources and git status stay unchanged. Two rounds, the
CPU load before each run recorded (from 1 to 100%: other threads' builds).

| C++ Debug | run + test | run only | objects |
|---|---|---|---|
| clean | 77.1 s (19.4× parallel, 24 min 58 s CPU) | | 920 |
| no change | 0.09 s | | 0 |
| touch `src/AnimationBank.hpp` (an ordinary header) | 37.1 / 25.6 s | 27.4 / 19.7 s | 5 |
| touch `src/ActionResult.hpp` (in the PCH) | 70.3 / 71.8 s | 65.6 / 50.3 s | 143 (run only: 102) |

The ordinary header's critical path: its slowest unity object 11.4 s, factorio.lib 4.2 s, the
factorio-test.exe link 9.9 s (factorio-run.exe 5.9 s). C++ Release clean: 4 min 19.6 s, 920 objects,
1 h 57 min CPU (27.1×). The paired rounds with kppc's server (a body edit, an inline function's
edit) and C++ Release's edits are still to come.

## kppc 9ab96a0e, old source d0495ebfd8a (2026-10-30)

A fresh conversion of C:\kpp-sandbox\scratch\convert\d0495 (FACTORIO), so TCPSocket uses a free port.

| Check | debug | release | debug-info |
|---|---|---|---|
| run_tests | 45/45 | | |
| spec | 791 pass, 9 pending, 0 failing | 791 pass, 9 pending, 0 failing | 791 pass, 9 pending, 0 failing |
| convert: headline | 0 | | |
| convert: smoke tests | 9/9 (CommonUtil, Agui, Util, imgui, glm, Lua internals, GDI+, regex, Lua compatibility) | | |
| game builds and runs `--version` | yes, 0 stubs | yes, 0 stubs | |
| menu maps | 45/45 same | 45/45 same | |
| unit tests | 9,005/9,005, test time 333 s | crash in LuaGame/LuaGameScript_PrintRecursiveLocalisedString; one suite per process (suites.py): 555/556 suites, 8,988/8,988 | |

The C++ build passes 9,006 (one test more than the conversion has).

The release crash is a double free in LuaHelper::parseLocalisedString, and its cause is general:
when -O2's branch folding lays two invokes back to back, the first one's `__C_specific_handler`
scope entry ends at its return address + 1 and the next one's begins at that address, so a throw
from the first runs both entries' cleanups (here the push_back argument temporary of the previous
loop iteration is destroyed again). Pending snippet
`compiler/tests/spec/language/adjacent_invokes_cleanup_once_release.kpp` (codegen, B). Found by
compiling only that module at -O2 into the debug build and bisecting with `-mllvm
-opt-bisect-limit`: the first bad pass is "Control Flow Optimizer" on that function.

### Runtime on the old source

Menu maps: `python -I bench-menu.py 3 debug=GAME release=GAME cpp-debug=cpp:EXE` (thread H's
scratch script: every map, each build in turn, three rounds; the benchmark's own "Performed 600
updates in X ms"), the sum over the 45 maps of each map's median. Executables of kppc 7f3e55cb on the
old source; the C++ Debug factorio-run.exe of the same source.

| Build | 45 maps, 600 ticks each |
|---|---|
| kppc debug | 29,314 ms |
| kppc release | 11,443 ms |
| C++ Debug | 48,293 ms |

No map's checksum differs between the three. A C++ Release of the same source runs only the unit
tests (its factorio-run.exe is 2.0.67, older than the data).

Game build at b80f9205, one run each (programs\factorio\kpp\run*\build.txt), every module changed:
debug codegen 7.4 s, clang -O0 4.9 s, link 4.6 s; release codegen 6.5 s, clang -O2 6.6 s, ThinLTO
link 103 s (12.8 s when the LTO cache has every module). The paired clean and one-line-edit measurements are still to come.
