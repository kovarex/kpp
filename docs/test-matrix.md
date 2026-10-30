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

## Runtime

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
