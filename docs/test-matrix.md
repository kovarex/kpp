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

### Unit tests

Plain runs (no `--fork`, no CRC mode), one after another: `factorio-test.exe --config CFG
--write-data DIR --no-colors` as `tests.cmd --baseline` runs it, each with its own write directory;
kppc's is `tests.cmd DIR` on the 40ce3c8d tree.

| Build | Result | Test time |
|---|---|---:|
| C++ Release | 9,501/9,501 | 229.2 s |
| C++ Debug | 9,501/9,501 | 829.5 s |
| kppc debug | 9,500/9,500 (one test fewer in the conversion) | 445.4 s (0.54× C++ Debug) |
| kppc release | not run: the pending adjacent-invokes crash (below) is not fixed yet | |

A second round, the four builds interleaved (`unit-pair.ps1`: C++ Debug, kppc debug, C++ Release,
kppc release), with kppc at 9b4552aa (B's fix of the adjacent-invokes crash, 76e81e04, included) on a
fresh conversion of 4c178a5b915 (headline 0, every smoke test passes). CPU load before each run 67 to
91%:

| Build | Result | Test time |
|---|---|---:|
| C++ Debug | 9,501/9,501 | 1,121.0 s |
| kppc debug | 9,499/9,500 | 705.0 s (0.63× C++ Debug) |
| C++ Release | 9,501/9,501 | 240.2 s |
| kppc release | **9,500/9,500** | 302.4 s (1.26× C++ Release) |

kppc debug's one failure, Multiplayer/MultiplayerRandomConnections ("not enough reconnections" after
182 s, TestMultiplayer.cpp:502), passes alone (kppc debug 4.7 and 27.4 s, C++ Debug 23.6 s): its
length varies from run to run, and it failed only under load. kppc's test build of this conversion
has 1 error and 1 stub: `entries.extract(entries.begin())` on a Set (tests/ForkLogic.kpp:449,
used only by `--fork`), broken by stdlib 6247e579 and fixed by C in 9ad6dd98.

**kppc release against C++ Release: 0.84×, and the 1.26× was the machine.** Two more rounds of the two
release builds alone (`release-pair.ps1`, the order swapped each round) gave test times of C++ Release
240.2 / 484.4 / 623.2 s and kppc release 302.4 / 375.7 / 198.6 s over the three rounds (the 623 s run
started at 1% load): one full run on this shared machine says little. Per test, each build's fastest
of the three rounds, summed over the 9,500 tests both have: C++ Release 209.5 s, kppc release
**175.6 s, 0.84×** (the median per-test ratio of tests over 5 ms 0.88, the 10th and 90th percentiles
0.48 and 1.47), in line with the menu maps' 0.81×. The first round's 18 outliers (from tens of
milliseconds to 1 to 3 s, 20.7 s of its 62 s gap, Simulations/MainMenuSimulations_gleba_grotto 0.20
against 2.91 s) do not reproduce: each alone, the builds alternating, 5 times, kppc release is as fast
or faster on every one (gleba_grotto 0.116 to 0.126 s against 0.122 to 0.184 s). By suite (per-test
minima), kppc release is slower by less than a second in AudioMusic (+0.91 s), TransitionalRequests
(+0.91 s) and SpacePlatform (+0.83 s), and faster in Simulations (−6.5 s), Saves (−4.1 s), Migrations
(−3.0 s), LatencyInput (−2.7 s) and Campaigns (−2.4 s). `suite-compare.py` and the per-test-minimum
comparison are in thread H's scratch.

C's CRC runs on the same builds (C++ Debug `--crc-write` 9,501/9,501 in 2,264 s; C++ Release checked
against it, 9,501/9,501 in 843 s, 0 mismatches) are slower because they write or check the CRCs.

### Unit tests against C++ Debug's CRCs

The runner's cross-platform CRC mode (tests\Main.cpp's `--crc-mode`): a run with `write` records
every CRC and double the tests check (CHECK_CRC, CHECK_DOUBLE, and TestScenario's map CRC before
each tick and at a test's end) per test; a run with `check` compares its own against them, each
difference a failed check. C++ Debug writes: `tests.cmd --baseline-debug --crc-write F` (C:\Factorio's
Debug factorio-test.exe of 4c178a5b915, 9,501/9,501, 1.87 MB of CRC data). The others check against
F: `tests.cmd --baseline --crc-against F`, `tests.cmd DIR --crc-against F`, `tests.cmd --release DIR
--crc-against F`. kppc 975d7584 (2026-10-31), a fresh conversion of 4c178a5b915 (headline 0, tests
errors 0).

| Build checked | Tests | Failed checks (CRC mismatches) | Test time |
|---|---|---:|---:|
| C++ Release (the control) | 9,501/9,501 | 0 | 843 s |
| kppc debug | 9,500/9,500 | 0 | 1,105.5 s |
| kppc release | 9,500/9,500 | 0 | 458.6 s |

The way there, the first check (kppc 92a7fb5f's conversion): kppc release 17 tests with mismatches
(31 failed checks), kppc debug 24 (43). Each cause was found by diffing the two builds' tagged map
dumps of a failing test (`--dump-mode` with a CRC mode; the first differing tag):
- Lua formatted numbers with the C runtime's sprintf, not trio's as its C++ build does, and the
  game's files that include a Lua header called trio where C++ calls the C runtime (d9006ed7).
- ssprintf formatted with the C runtime's vsnprintf into one static buffer; C++ formats with trio
  into a thread_local one (9b4552aa). LuaPrintNumbers and 11 prototype tests.
- move() of a const object emptied it (89e0adb0, spec language/move_of_const_copies): the player's
  translation requests in flight lost their keys (Campaigns/LoadDemoLevel05).
- A string literal was a private copy per module, so `==` of two pointers to it failed in kppc
  debug (39d5785c): the latency-car tests' tank with a grid got no equipment (7 tests, debug only).
- A line splice in a string literal kept its newline (17a6d854, spec
  language/string_literal_line_splice): the tests' multi-line Lua commands echoed into the console
  otherwise (ChunkBounds, EmptyMapGenSettingsReadWriteStability,
  DestroyRoboportWithTriggerWhenSpawningRobotsForLogistics).

MapDeserialiser/SaveLoadStdVariant, the one test the conversion leaves out
(overrides\left-out-tests.txt), is the 9,501st of C++'s.

### Build time: the game, edit to linked executable

**Method.** Each step runs the four builds one after another (paired, interleaved), two rounds;
`pair-edit.ps1` in thread H's scratch. The CPU load before each run is recorded: 0 to 100% (other
threads' builds), so every number is one sample on a shared machine.
- C++: `cd C:\Factorio\fastbuild`, `FBuild.exe -summary [-clean] vs2022-<Debug|Release>x64-factorio-run`
  (the game only, as kppc builds it; the time is the summary's `Real`). An edit is a timestamp-only
  touch of a header (`(Get-Item F).LastWriteTime = Get-Date`): FASTBuild rebuilds what includes it as
  for a one-line change, and C:\Factorio's sources and git status stay unchanged.
- kppc: run.cmd's own kppc line with `--server` added, into a directory of its own; the time is the
  client's wall time from start to exit, including the link.
- The leaf edit: C++ touches `src/AnimationBank.hpp` (an ordinary header, 5 objects); kppc changes a
  string in `AnimationBank::getAnimation` in the converted AnimationBank.kpp.
- The wide edit: C++ touches `src/ActionResult.hpp` (included by the PCH, 102 objects of the game);
  kppc swaps the operands of `MapPosition::operator+(const MapPosition&)`, an inline function used
  across the game. Each kppc edit is undone by the next round's, so the tree ends as converted.

| Step | C++ Debug | kppc debug | C++ Release | kppc release |
|---|---|---|---|---|
| clean | 56.7 / 61.0 s (862 objects) | 56.4 / 44.3 s (fresh server; see below) | 4 min 19.6 s with factorio-test (once: a Release clean rewrites the libraries kppc links) | 133.0 / 142.7 s |
| no change | 0.2 / 0.2 s | 0.2 / 0.2 s | 0.2 s | 0.1 / 0.2 s |
| leaf edit | 22.0 / 28.7 s | **2.4 / 3.2 s** | 23.2 / 26.5 s | 67.2 / 88.2 s |
| wide edit | 58.8 / 51.1 s | **10.7 / 14.8 s** | 111.4 / 105.9 s | 103.4 / 92.0 s |

An earlier pass of the kppc builds alone (no C++ in between) gave clean 14.4 / 56.2 s (debug) and
96.1 / 99.8 s (release), leaf 2.0 / 2.0 s and 64.5 / 64.3 s, wide 7.5 / 8.7 s and 87.1 / 67.8 s.
C++ Release's first no-change build compiled 102 objects for a reason not found; the second built
nothing (0.1 s).

- **kppc release's edit is its ThinLTO link**: 60 to 85 s of the leaf edit's 64 to 88 s is the lld
  link with 1 of 64 modules changed (clang itself 0.8 to 1.4 s), against 0.4 to 0.7 s for the debug
  link. That makes kppc release's leaf edit slower than C++ Release's (64 to 88 s against 23 to 27 s)
and its wide edit about level (68 to 103 s against 106 to 111 s).
- **The server's first build is slower than a build in its own process.** Clean kppc debug builds
  alternating the two (`clean-kppc.ps1`, 3 rounds): in its own process 12.8 / 12.4 / 15.0 s, through a
  fresh server 12.4 / 35.2 / 38.4 s. In the slow ones, instantiation takes 55 to 62 s of CPU instead
  of 18 to 26 s, and resolve 2.0 to 2.2 s instead of 0.9 to 1.3 s. A clean kppc debug build is 12 to
  15 s against C++ Debug's 57 to 61 s.

C++ with its tests (`factorio-run` and `factorio-test`, the 2026-10-23 baseline's targets),
`cpp-runtest.ps1`, Debug and Release interleaved, plus the first Debug rounds:

| C++, run + test | Debug | Release |
|---|---|---|
| clean | 77.1 s (920 objects, 24 min 58 s CPU, 19.4×) | 4 min 19.6 s (1 h 57 min CPU, 27.1×) |
| no change | 0.09 to 0.21 s | 0.09 to 0.11 s |
| touch `src/AnimationBank.hpp` (5 objects) | 37.1 / 25.6 / 35.3 / 43.1 s | 29.0 / 38.9 s |
| touch `src/ActionResult.hpp` (143 objects) | 70.3 / 71.8 / 67.2 / 65.1 s | 4 min 46.5 s / 4 min 43.3 s |

On 2026-10-23 (d0495ebfd8a) the same Debug numbers were 62 s clean, 16 s and 55 s. The ordinary
header's critical path now: its slowest unity object 11.4 s, factorio.lib 4.2 s, the factorio-test.exe
link 9.9 s (factorio-run.exe 5.9 s).

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
