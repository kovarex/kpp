# Test matrix

Every check the repository has, in each build configuration, at one kppc commit on one Factorio
source. Thread H keeps it current; each row names the command that produces it.

## The configurations

| Name | kppc flags | What differs |
|---|---|---|
| debug | (none) | `assert` checked, clang -O0, link.exe /OPT:REF /OPT:NOICF (`--incremental-link`: /INCREMENTAL, run.cmd's default) |
| release | `--release` (= `--config debug=false`) | `assert` compiles to nothing (`release_assert` stays), clang -O2 (-flto=thin with several modules), lld ThinLTO with a cache and /OPT:REF; refuses `--incremental-link` |
| debug-info | `--debug-info` | CodeView line tables and a PDB; orthogonal to the two above |

A third, final, is planned: the game as Factorio's official FinalRelease build makes it (whole-program
optimization, `FINAL_RELEASE`). [final-release.md](final-release.md) has what that needs and its
benchmark against C++ FinalRelease.

**Game speed is compared against C++ FinalRelease** (kovarex, 2026-11-04). kpp's release and final
builds are measured against the C++ FinalRelease build of the same Factorio commit, the build the
official releases come from. C++ Release is at most an extra column. The latest numbers: kpp release
**0.97x C++ FinalRelease** on performance.zip and **0.98x** on modern_playthrough_155 (C++ Release:
1.10x on both; [final-release.md](final-release.md), "The benchmark"). Thread H rebuilds C++
FinalRelease whenever the converted Factorio commit moves, and keeps its benchmark root
(`scratch\configs\bench\cppfinal`) for every thread's runs.

kppc's trivial inlining is on in all of them (`--no-inline` turns it off). The converted game is
always Factorio's FastDebug preprocessing (`_DEBUG`, `DEBUG`, `LUA_USE_APICHECK`: convert.cpp's
`buildDefines`), so the kppc release game is FastDebug code without asserts at -O2, not Factorio's
Release. [build.md](build.md) has the configuration names and the build description.

The commands: `compiler\tests\run_tests.cmd`; `compiler\tests\spec.cmd [--release | --debug-info]`;
`programs\factorio\convert.cmd DIR` (headline and smoke tests); `programs\factorio\run.cmd
[--release] --version` (the game builds and starts); `python -I programs\factorio\menu-checksums.py
GAME_DIR` (the 45 menu maps' checksums, with test_mod removed from the game's mods);
`programs\factorio\tests.cmd [--release] DIR` (the unit tests).

## kppc b96ea59a, Factorio 4c178a5b915 (2.1.25, 2026-10-31): current

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

This measurement predates the rule that game speed is compared against C++ FinalRelease: it compares
against C++ Release and C++ Debug. The next run of the 45 maps adds C++ FinalRelease as the
reference. Against FinalRelease, see [final-release.md](final-release.md) (the two saves).

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
--crc-against F`. kppc master with the typed unions' construction from a tag (D's 4e7aa643 and
5342a7fd) and the converter's mapping of std::variant's remaining constructions to it (2026-10-31),
a fresh conversion of 4c178a5b915 with every test of C++'s (headline 0, tests errors 0, no
printf-format error, Vector growth by move_if_noexcept).

| Build checked | Tests | Failed checks (CRC mismatches) | Test time |
|---|---|---:|---:|
| C++ Release (the control) | 9,501/9,501 | 0 | 843 s |
| kppc debug | 9,501/9,501 | 0 | 1,567.7 s |
| kppc release | 9,501/9,501 | 0 | 538.9 s |

Before, the conversion left MapDeserialiser/SaveLoadStdVariant out (its variants needed construction
in a given alternative): kppc 8ac5d25f debug 9,500/9,500 (1,183.3 s), release 9,500/9,500 (466.2 s);
kppc 975d7584 debug 9,500/9,500 (1,105.5 s), release 9,500/9,500 (458.6 s); 0 mismatches each.

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

## Container sizes: u32 against u64 (kppc fe933d5b, Factorio 4c178a5b915, 2026-11-01)

Every stdlib size, count, index and capacity is `ContainerSize` (stdlib/core/Sizes.kpp): u32, or u64
with `kppc --config sizeBits=64`, when this was measured. kovarex asked whether the width matters on the game, so that one
String, Vector and so on is enough if it does not. The verdict is that it does not, and ContainerSize
is now u64 alone, with the switch removed (design.md, "Container sizes are u64").
- u64 is as fast as u32, or slightly faster in release: −1.7% on the menu maps and −0.6% in the unit
  tests. In debug it is level: +0.1% and +0.5%.
- u64 uses about 1 to 2% more peak commit.
- The game builds equally fast with either width, and its exe is 1% larger at most.
- Both widths give the same results as C++: every menu map's checksum matches, and the unit tests'
  CRCs have 0 mismatches.

**Builds.** One conversion of 4c178a5b915 by kppc fe933d5b (convert.cmd into thread H's scratch:
headline 0, tests errors 0, every smoke test passes). It was built eight ways: the game and the
unit tests, debug and release, each at sizeBits 32 and 64. The command is run.cmd's and tests.cmd's
own kppc line with `--config sizeBits=W` added and a clean output directory (`size\build.cmd W
release|debug game|tests` in thread H's scratch). Every build has 0 errors and 0 stubs.

**The containers' sizes** (`sizeof`, a small program built at both widths):

| Type | u32 | u64 |
|---|---:|---:|
| ContainerSize | 4 | 8 |
| String | 24 | 32 |
| WString | 16 | 24 |
| Vector<int> | 16 | 24 |
| Deque<int> | 24 | 32 |
| Map<int, int>, Set<int> | 40 | 48 |
| HashMap<int, int>, HashSet<int> | 32 | 48 |
| StringView, List<int>, FastMap<int, int> | 16, 24, 24 | the same |

The elements are the same at both widths, and so are the capacities and allocations: only each
container's own header grows. Allocation counts were not measured, because nothing in the growth
policy depends on the width.

### Runtime: the 45 menu maps

`python -I bench-mem.py 3 menu.json u32-release=DIR u64-release=DIR u32-debug=DIR u64-debug=DIR`
(thread H's scratch). Each map runs for 600 ticks with `--benchmark`, as in the section above. Each
round runs the four builds one after another on each map, with the order reversed on every other
round, for three rounds. The total is the sum over the 45 maps of each map's median.

| Build | 45 maps | per round | u64/u32 | per-map u64/u32: median (min to max) | u64 faster on |
|---|---:|---|---:|---|---:|
| u32 release | 8,352 ms | 8,375 / 8,301 / 8,377 | | | |
| u64 release | 8,211 ms | 8,212 / 8,247 / 8,180 | **0.983** | 0.985 (0.943 to 1.034) | 37 of 45 maps |
| u32 debug | 18,360 ms | 18,371 / 18,434 / 18,246 | | | |
| u64 debug | 18,376 ms | 18,418 / 18,381 / 18,361 | **1.001** | 1.003 (0.952 to 1.045) | 17 of 45 maps |

All four builds give every map the checksum of the C++ 2.1.25 baseline (menu-checksums.txt), on
45 of 45 maps.

### Unit tests

Full plain runs, interleaved, with the order swapped every round (`size\unit-rounds.ps1`): three
rounds in release and two in debug. Every run passes 9,501 of 9,501. The time is the per-test minimum
over the rounds, summed over the 9,501 tests (`size\minima.py`).

| Build | runs' test times | per-test minima summed | u64/u32 | per-test ratio over 5 ms: median (10th to 90th percentile) |
|---|---|---:|---:|---|
| u32 release | 156.3 / 154.6 / 154.9 s | 150.7 s | | |
| u64 release | 155.0 / 154.9 / 153.6 s | 149.9 s | **0.994** | 0.997 (0.938 to 1.041) |
| u32 debug | 330.6 / 332.8 s | 326.4 s | | |
| u64 debug | 332.6 / 333.7 s | 327.9 s | **1.005** | 1.005 (0.939 to 1.069) |

No suite moves by more than 0.34 s. The largest change in release is Multiplayer, 15.85 s at u32
against 15.52 s at u64. The largest in debug is Simulations, 46.86 s against 47.19 s.

**CRCs.** The tests' cross-build CRC mode compared these builds against C++ Release, whose CRCs
equal C++ Debug's (above). C++ Release wrote the CRCs (`factorio-test.exe --crc-mode write`,
9,501 of 9,501 tests, 1.87 MB). Each kppc build then checked its own against them with `--crc-mode
check`:

| Build checked | Result | Failed checks (CRC mismatches) |
|---|---|---:|
| u64 release | 9,501/9,501 | 0 |
| u64 debug | 9,501/9,501 | 0 |
| u32 release (the control) | 9,501/9,501 | 0 |

### Memory

Peak working set and peak commit come from GetProcessMemoryInfo on each benchmark process
(bench-mem.py, the runs above). The peak is about 440 MB on every map at both widths, so it is the
game's prototypes and sprites, which are loaded before any map, and not the map itself. These menu
maps are small. The per-map difference compares each map's median over its three rounds:

| | median commit difference, u64 − u32 (min to max over the maps) | median ratio | working set difference |
|---|---|---:|---:|
| release | +5.8 MB (−12.3 to +29.4) | 1.013 | +5.5 MB |
| debug | +8.4 MB (−13.7 to +29.3) | 1.020 | +8.3 MB |

The largest peak in any run is 448 MB at u32 against 456 MB at u64 in release (commit 437 against
446 MB), and 462 MB against 472 MB in debug. No large save was measured. A map with millions of
small containers would grow by the header sizes in the table above, and by nothing else.

### Build time and size

Two clean builds of each, interleaved: u32 first in the first round, u64 first in the second. Times
are the wall time of kppc, including the link.

| Build | u32 | u64 | exe u32 | exe u64 | IR (.ll) u32 | IR u64 |
|---|---|---|---:|---:|---:|---:|
| game release | 69.9 / 68.8 s | 67.6 / 68.8 s | 65,590,784 | 66,435,072 (+1.3%) | 758.9 MB | 758.7 MB |
| game debug | 16.2 / 16.6 s | 16.4 / 21.6 s | 84,110,848 | 84,441,600 (+0.4%) | 877.2 MB | 877.2 MB |
| tests release | 138.8 / 137.5 s | 138.2 / 137.8 s | 206,733,824 | 207,378,432 (+0.3%) | 2,281.0 MB | 2,281.0 MB |
| tests debug | 34.4 / 36.2 s | 34.3 / 36.7 s | 239,804,928 | 239,092,736 (−0.3%) | 2,905.6 MB | 2,905.8 MB |

### Where the difference comes from

The width changes which integer conversions are written, not how many. In the game's release IR:
- `zext i32 … to i64` falls from 54,274 at u32 to 7,401 at u64;
- `sext i32 … to i64` rises from 52,206 to 102,416;
- `trunc i64 … to i32` falls from 7,852 to 2,844.

The converted game indexes mostly with `int` (`for (int i = 0; i < v.size(); ++i) v[i]`):
- At u32, the int becomes a u32 for free and then is zero-extended for the address. LLVM cannot
  widen a u32 induction variable to 64 bits, because its wraparound is defined.
- At u64, the int is sign-extended. An `int` loop counter's increment has no signed wrap (nsw), so
  LLVM's induction-variable widening removes the extension from the loop.

This is the likely source of release's 1.7%, which -O0 does not get; it is not profiled. Neither
width is slower anywhere that matters: the spread of per-map and per-test ratios is the same on both
sides of 1.

## Historical: kppc 9ab96a0e, old source d0495ebfd8a (2026-10-30)

Kept as a record only; 4c178a5b915 above is the current matrix. d0495 is no longer a gate: with the
printf-format check always on (21441057), its conversion has 3 errors from Factorio's own undefined
behaviour, which upstream has since fixed.

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
