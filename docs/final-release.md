# kpp's best release against C++ FinalRelease

The official Factorio releases are built in fbuild.bff's FinalRelease configuration, of its
factorio-all-in-one target: all of `src` in one unity file (kovarex, 2026-11-04). A fair runtime
comparison puts kpp's best release build against that, not against C++ Release (kovarex,
2026-11-03). This page records what FinalRelease is, what the converted game and kppc need to build
the same program, and the measurements. Thread H leads it. The converter's part belongs to C, the
build cell's to B.

**The rule** (kovarex, 2026-11-04, CLAUDE.md): the game speed of kpp's release and final builds is
always given against **C++ FinalRelease all-in-one** of the same Factorio commit. C++ Release, or
FinalRelease's factorio-run, appears only as an extra column. Thread H rebuilds it whenever the
converted commit moves (FASTBuild through the coordinator) and keeps its benchmark root,
`scratch\configs\bench\cppaio`, for every thread's runs.

**Latest** (2026-11-04, Factorio 4c178a5b915), against C++ FinalRelease all-in-one:

| build | performance.zip | modern_playthrough_155 |
|---|---:|---:|
| kpp final, full LTO (B's final cell) | **0.99x** | **0.97x** |
| kpp final at -O3 | 0.98x | 0.97x |
| kpp release (ThinLTO) | 0.98x | 0.99x |
| C++ FinalRelease, factorio-run | 1.01x | 1.00x |

The details are in "The benchmark against the all-in-one" below. On the 45 menu maps kpp release is
**0.87x** C++ FinalRelease's factorio-run, which is within 1% of the all-in-one on the two saves
(C++ Release: 1.01x; [test-matrix.md](test-matrix.md)).

Source: C:\Factorio 4c178a5b915 (2.1.25), `fastbuild/fbuild.bff`, `.BuildVanillaRelease` (line 1303)
and `.BuildVanillaFinalRelease` (line 1373). `.BuildRelease` and `.BuildFinalRelease` add `SPACE_AGE` to
them; the vanilla builds are the same without it.

## What FinalRelease changes against Release (Windows, vs2022 x64)

Both are unity builds in the same way: unity is per target, not per configuration. `src` goes into
100 unity files, Agui into 20 and the tests into 40, with the same precompiled headers. Everything
below is what differs.

| | Release | FinalRelease |
|---|---|---|
| Optimization | `/O2 /Ob2 /Oi /Ot` | the same |
| Runtime, security checks | `/MT /GS-` | the same |
| Function-level linking | `/Gy-` (none) | `/Gy` |
| Whole-program optimization | none | `/GL` when compiling, `/LTCG` when linking (full link-time code generation) |
| Global data in COMDATs | no | `/Gw` (unused or duplicate globals dropped) |
| Link | `/DEBUG:full /INCREMENTAL:NO /LTCG:incremental` (no effect: no object has /GL) | `/DEBUG /LTCG /OPT:REF /CGTHREADS:20` |
| Unreferenced code | kept (no `/OPT:REF`) | removed (`/OPT:REF`) |
| Defines | `NDEBUG USE_COMMON_HPP LUA_USE_APICHECK SPACE_AGE` | the same plus **`FINAL_RELEASE PACKAGED_BUILD`** |

`/Zi` (a PDB) and `/Zc:inline` are in both. `LUA_USE_APICHECK` stays on in FinalRelease, and so does
`release_assert`, which no build type turns off: `src/Util/ReleaseAssert.hpp` defines it
unconditionally. FinalRelease drops only the asserts that are guarded by `#ifndef FINAL_RELEASE`.

## What the two defines control

### FINAL_RELEASE

It appears in 154 lines in 92 files (`#ifdef`/`#ifndef FINAL_RELEASE`). There are also 13 uses of
`BuildConstants::IsFinalRelease`, a constexpr bool set from it (src/BuildConstants.hpp:26). Grouped
by what they do:

- **Checks on the simulation's hot paths, gone in FinalRelease:**
  - `release_assert(!LatencyState::runningLatencyUpdate)` in `Surface::getAdvancedTile` and the
    tile lookup at src/Surface/Surface.cpp:400 and :1482;
  - the four blocks of `release_assert`s on activation and mode changes in
    src/Entity/UpdatableEntity.cpp:184, 212, 229, 244;
  - the list consistency checks in src/Util/Targetable.cpp:140, 148, 157;
  - "Already linked" in IntrusiveList's link (src/Util/Container/IntrusiveList.hpp:438);
  - `checkConsistency()` in WireEnd (src/Wires/WireEnd.cpp:16, 26);
  - `release_assert(!this->isToBeDeleted())` in Entity's setup (src/Entity/Entity.cpp:375, 381);
  - the rocket silo clearer (src/Space/TransitionalRequestLogic.cpp:11, 24, 36).
- **Counters, gone in FinalRelease:**
  - `Entity::count`, a `std::atomic_size_t` changed in every entity's constructor and destructor
    (src/Entity/Entity.hpp:1318, src/Entity/Entity.cpp:108, 144, 277);
  - `Map::entitiesInvalidationCounter`, a member of Map (src/Map/Map.hpp:528, so Map's layout
    differs), incremented by entity creation and by every trigger (src/Entity/Entity.cpp:482,
    src/Trigger/Trigger.cpp:25).
- **Checks when saving, gone in FinalRelease.** These run when saving and in the heuristic CRC:
  - Inventory, ItemStack, Item, ID, MapPosition (src/Item/Inventory.cpp:85,
    src/Item/ItemStack.cpp:104, src/Item/Item.cpp:33, src/ID/ID.cpp:69,
    src/Util/MapPosition.cpp:110);
  - ActiveEntitiesList (src/Surface/ActiveEntitiesList.cpp:29);
  - src/Util/DeserialiserHelper.hpp:249.
- **Development features, gone in FinalRelease:**
  - debug and dev-tools GUIs (DevToolsSettings, ImguiSDL, DebugSettings, the prototype explorer);
  - Lua debug members and functions (LuaEntity, LuaPlayer, LuaObjectsData, LuaGameScript,
    LuaDefines);
  - developer console commands (CommandProcessor);
  - command line options (src/Main.cpp, 8 sites);
  - the lockstep desync write stream (src/MainLoop.cpp:1615);
  - mod and locale development helpers (ModManager, Mod, LocaleProvider).
- **Behaviour only FinalRelease has:**
  - forced graphics settings (src/Graphics/GraphicsSettings.cpp:438, 460);
  - settings defaults through `IsFinalRelease`: fullscreen, runtime sprite reload, the version
    filters, the main menu simulation selector (src/InterfaceSettings.cpp:46–89,
    src/Graphics/GraphicsSettings.cpp:30–119);
  - the base mod's campaign check (src/Campaign/Campaign.cpp:66);
  - the simulation map setup (src/Map/MapSetupHelper.cpp:220);
  - the crash handler (src/Util/CrashHandler.cpp:258);
  - `static_assert(currentBranchVersion == 0)` (src/Map/Map.cpp:132);
  - EntityBuildAnimations.hpp:34, AssertRenderThread.hpp:24, Scenario.cpp:415,
    TechnologyPrototype.cpp:544, LuaSurface.cpp:4418, LuaPlayer.cpp:769, 780.
- **Combined with names the Windows build leaves undefined.** No change on Windows:
  - `DEMO && FINAL_RELEASE` (src/CommandProcessor.cpp:704, 787, 1586);
  - `NINTENDO_SWITCH && !FINAL_RELEASE` (src/Main.cpp:359, 442);
  - `LINUX && !FINAL_RELEASE` (src/Graphics/SDLWindow.cpp:233);
  - `DEV_ONLY_DEBUG_TOOL && ...` (src/Debug/DebugTools.cpp:2501).
  - Two do need the define's value on Windows: `FINAL_RELEASE && !NINTENDO_SWITCH`
    (src/Util/CrashHandler.cpp:258) and `!NINTENDO_SWITCH || !FINAL_RELEASE`
    (src/Gui/DebugSettingsGui.cpp:46).

### PACKAGED_BUILD

It appears in 5 lines in 4 files. On Windows it changes only two things:
`BuildConstants::IsPackagedBuild`, which nothing else reads, and `SpriteLoaders.cpp:67`, where every
16-bit sprite is refused, not only those from the game's internal mods. The data paths depend on it
only on macOS and Linux (src/Paths.cpp:203, 251). A FinalRelease build therefore finds `data\` two
levels up, as Release does.

## For C: the converter

As built (thread C, 2026-11-04): a third configuration, `kppc --convert-final` (Release's defines with
FINAL_RELEASE, PACKAGED_BUILD and ALL_IN_ONE=1), merged with the other two under
`config::finalRelease`, which factorio-in-kpp's build.kpp sets in its final configuration. 197 #if
groups differ in final alone, and 2 differ in all three. Details, and the five groups taken from
Release, are in design.md 12 ("The game's FinalRelease as config::finalRelease"). The plan as it was
written:

The converted game has two configurations, FastDebug and Release, merged under `config::debug`
(design.md 12; convert.cpp's `buildDefines`). Neither defines `FINAL_RELEASE` or `PACKAGED_BUILD`, so
today's converted game is Release in both respects: every `#ifndef FINAL_RELEASE` block is in.

A final build needs a third configuration: Release's defines plus `FINAL_RELEASE` and
`PACKAGED_BUILD`, merged as a config constant (`config::finalRelease`, for example; the name is C's
or kovarex's decision), as plan B did for `config::debug`.

- **In function bodies** (most of the 154 sites): `if constexpr`.
- **Declarations**, which need `when` (design.md 19) or the equivalent: the header sites
  `src/Map/Map.hpp:528` (a member, so the layout is per configuration), `src/Entity/Entity.hpp:36`
  (an include) and `:1318` (a static member), `src/Chart/ForceChartedArea.hpp:28`,
  `src/CommandProcessor.hpp:208`, `src/Debug/DevToolsSettings.hpp:3`, `src/Debug/ImguiSDL.hpp:15`,
  `src/Entity/EntityBuildAnimations.hpp:34`, `src/Entity/HeuristicEntitySearch.hpp:64`,
  `src/GlobalContext.hpp:24`, `src/Graphics/AssertRenderThread.hpp:24`, `src/Graphics/GpuApi.hpp:5`,
  `src/Info/DebugSettings.hpp:214`, `src/InputActionHandler.hpp:38`, `src/Script/LuaEntity.hpp:37`,
  `src/Script/LuaObjectsData.hpp:3`, `:33`, `src/Script/LuaPlayer.hpp:32`, `:125`, `:179`,
  `src/Space/TransitionalRequestBuffers.hpp:55` and `libraries/Agui/Widget.hpp:939`.
- **The two `BuildConstants` values:** `IsFinalRelease` and `IsPackagedBuild`.
- **`#error` and `static_assert`:** `src/BuildConstants.hpp:32` refuses FINAL_RELEASE together with
  DEBUG, so a final build is always non-debug. `src/Map/Map.cpp:132`'s static_assert is checked only
  in FinalRelease.
- **`ALL_IN_ONE=1`:** the all-in-one target's one define of its own (fbuild.bff's
  `.factorioallinoneConfig`). The converter lists it as undefined on purpose today. It makes
  GlobalContext's test flags constexpr false (src/GlobalContext.hpp:445: `inTests`,
  `inChartingTests`, `inTestUsingAppManager`, `inHeavyModeTests()`, `inBuildingRendererTests`,
  `inForkGraphicsTests`, `prepareTestCacheFiles`), so every `if (global->inTests ...)` folds away. It
  also drops ClientItemStackLocation's test-only constructor (src/Input/ClientItemStackLocation.hpp:11,
  .cpp:13). The kpp final game keeps them as runtime members until the converter defines it. The
  test executable (factorio-test) is not all-in-one and never defines it.
- **C libraries:** they are compiled with Release's defines (build.kpp's `fromSource`). None of
  them reads `FINAL_RELEASE` or `PACKAGED_BUILD`: in all of `libraries\`, the only site is Agui's
  Widget.hpp, which is converted C++. So `fromSource` needs only the two defines added, for
  consistency.

## For B: a kpp final cell

kppc's `--release` today, for a program of several modules:
- clang `-O2 -flto=thin` on each module;
- lld with ThinLTO (`/opt:lldltojobs`, a cache next to the executable) and `/OPT:REF` (main.cpp:622,
  1590, 1929).

The game's C libraries are compiled separately with clang-cl `/O2 /MT` and are not part of the LTO.
To match FinalRelease:

1. **Whole-program optimization over the whole game.** FinalRelease's `/GL` + `/LTCG` is full LTCG:
   one optimization over every object, the C and C++ libraries included. Its closest equivalent is
   full LTO (`-flto` rather than `-flto=thin`), or one module. Which gives the faster game is a
   measurement: ThinLTO with good partitioning is often close to full LTO. The C libraries would
   need `-flto` too (clang-cl `-flto`), so that lld optimizes across them as LTCG does across
   Lua's and zlib's objects.
2. **Unreferenced functions and data removed:** `/OPT:REF` is in kpp's release already. `/Gw` (data
   in COMDATs) has an equivalent in `-fdata-sections`.
3. **The game's FINAL_RELEASE configuration**, from C (above).
4. **The same runtime and checks as far as they matter:** `/MT` (the static CRT; the libraries are
   /MT already) and `/GS-` (no stack cookies). kppc's own IR gets no stack protector. The libraries
   (Lua, zlib and the game's other C and C++ libraries) are a different case: kpp compiles them with
   clang-cl `/nologo /c /O2 /MT /DNDEBUG /w` (project.cpp:902, with no flags from build.kpp's
   `fromSource`), and clang-cl's `/GS` is on by default. So kpp's libraries carry stack cookies,
   which both C++ builds compile out with `/GS-`. Every kpp release number so far, B's plan-B run
   of 2026-11-03 included, has them. The final cell passes `/GS-` (B).
   `/Oi /Ot /Ob2` are what clang's -O2 does by default.

Optimization level: `/O2` is MSVC's highest. The fair comparison is kpp's best: -O2 and -O3 both
measured, with the faster one kept and named.

## C++ FinalRelease build

`cd C:\Factorio\fastbuild && FBuild.exe -clean -summary vs2022-FinalReleasex64-factorio-run`, run on
C:\Factorio 4c178a5b915 (2026-11-03 22:58 UTC). It writes `vs2022\FinalReleasex64` and
`bin\FinalReleasex64vs2022`.

| | |
|---|---|
| Wall time | **3 min 48.4 s** |
| Of that, the LTCG link (`/CGTHREADS:20`) | 157.4 s |
| CPU | 23 min 17.6 s (6.1:1), 862 objects |
| The machine | loaded: 100% CPU at the start, from other threads' gates. A rough figure only. |
| Result | factorio-run.exe 49.4 MB (Release's 65.1 MB), "2.1.25 (build 87847, win64, full)" |

### C++ FinalRelease all-in-one, the reference

`FBuild.exe -clean -summary vs2022-FinalReleasex64-factorio-all-in-one` (fbuild.bff's "Factorio all
in one", line 3021), run on the same commit (2026-11-04 08:09 UTC). It is FinalRelease's flags and
defines, plus `ALL_IN_ONE=1`. All of `src` is one unity file (`.UnityNumFiles = 1`), compiled without
the precompiled header (`/bigobj /Zm300`). It writes `bin\FinalReleasex64vs2022\factorio.exe`, beside
the factorio-run.exe above.

| | |
|---|---|
| Wall time | **4 min 52.1 s** (load 5% at the start) |
| Of that, the one unity file's compile | 119.8 s |
| Of that, the LTCG link | 172.2 s |
| CPU | 9 min 5.9 s (1.9:1): the unity file and the link run one after the other |
| Result | factorio.exe 49.4 MB; checksums equal to the other builds' on both saves |

The benchmark root copies it as `bench\cppaio\bin\FinalReleasex64\factorio-run.exe`. The bench runs a
factorio-run.exe as C++ (without kpp's `--no-crash-handler`), and the game finds its paths from its
folder, not its name.

## The benchmark against the all-in-one (2026-11-04)

The same method as the next section, in a quiet slot from 08:29 to 08:44 UTC:
- `--benchmark SAVE --benchmark-ticks 1000`, 5 rounds interleaved with the order rotated, each build
  from its own root;
- load before each run 0.1–1.9%;
- checksums equal in all 25 runs of each save.

The builds:
- **C++ FinalRelease all-in-one:** above, the reference.
- **C++ FinalRelease's factorio-run:** above.
- **kpp release:** B's plan-B game, as in the next section.
- **kpp final:** B's final cell (branch final-cell): `kppc --lto full`, full LTO of the kpp code in
  32 partitions, the C libraries built with `/GS-` outside the LTO. Still without FINAL_RELEASE's and
  ALL_IN_ONE's configuration (C's, above).
- **kpp final at -O3:** the same with `--opt-level 3`.

performance.zip (checksum 21933964):

| build | rounds (ms) | median | vs all-in-one |
|---|---|---:|---:|
| C++ FinalRelease all-in-one | 7068, 7116, 7033, 7045, 7014 | 7,045 | 1.00x |
| C++ FinalRelease, factorio-run | 7060, 7108, 7134, 7226, 7053 | 7,108 | 1.01x |
| kpp release | 7119, 6862, 6909, 6923, 6924 | 6,923 | **0.98x** |
| kpp final | 6879, 6883, 7131, 7111, 6979 | 6,979 | **0.99x** |
| kpp final at -O3 | 6948, 6836, 6912, 6757, 7006 | 6,912 | **0.98x** |

kovarex_modern_playthrough_155.zip (checksum 816201650):

| build | rounds (ms) | median | vs all-in-one |
|---|---|---:|---:|
| C++ FinalRelease all-in-one | 7941, 8079, 7941, 8061, 8102 | 8,061 | 1.00x |
| C++ FinalRelease, factorio-run | 8119, 8095, 7890, 8126, 7923 | 8,095 | 1.00x |
| kpp release | 7838, 8018, 8004, 7943, 8158 | 8,004 | **0.99x** |
| kpp final | 7790, 7700, 7820, 7954, 7946 | 7,820 | **0.97x** |
| kpp final at -O3 | 7655, 7811, 7823, 7800, 7604 | 7,800 | **0.97x** |

What these show:
- The all-in-one is 0.9% and 0.4% faster than FinalRelease's factorio-run. One unity file adds
  little on top of `/GL` and `/LTCG`, which already optimize across the whole program.
- kpp's builds are 1–3% faster than the reference. The rounds overlap, though: kpp final's
  performance.zip rounds span 6,879–7,131 ms. -O3 is level with -O2 within that spread.
- kpp final still runs FINAL_RELEASE's checks and ALL_IN_ONE's runtime test flags, which the C++
  builds compile out. C's configuration is the next lever.

## The benchmark: C++ FinalRelease, C++ Release, kpp release (2026-11-04)

The first comparison, the night before, was against FinalRelease's factorio-run (the all-in-one was
not yet the reference).

The method is B's from 2026-11-03 (the bench.py in thread B's scratch), copied:
- `--benchmark SAVE --benchmark-ticks 1000`, 5 rounds interleaved with the order rotated;
- each build from its own root, with read-data C:/Factorio/data and write-data that root;
- the machine's load logged before each run: 0.3–0.9% throughout, in a quiet slot from 23:54 to
  00:03 UTC;
- the game's own "Performed 1000 updates in X ms" is the time, and the median of 5 rounds is the
  result;
- checksums equal in all 15 runs of each save.

The builds:
- **C++ FinalRelease:** the build above.
- **C++ Release:** factorio-run.exe of 2026-10-31, the same source.
- **kpp release:** B's plan-B game: kpp 0.2.170 (master 29007274) building factorio-in-kpp c9cda971,
  converted from the same source, with `kpp factorio release`. This is ThinLTO, with its C
  libraries outside the LTO and compiled with stack cookies (above), and FINAL_RELEASE's checks
  still in. It is not yet the final cell.

performance.zip (checksum 21933964):

| build | rounds (ms) | median | ms/tick | vs C++ FinalRelease |
|---|---|---:|---:|---:|
| C++ FinalRelease | 7210, 7038, 7279, 7076, 7115 | 7,115 | 7.12 | 1.00x |
| C++ Release | 7812, 8051, 7821, 7868, 7861 | 7,861 | 7.86 | 1.10x |
| kpp release | 6853, 6888, 6863, 6958, 6988 | 6,888 | 6.89 | **0.97x** |

kovarex_modern_playthrough_155.zip (checksum 816201650):

| build | rounds (ms) | median | ms/tick | vs C++ FinalRelease |
|---|---|---:|---:|---:|
| C++ FinalRelease | 8213, 8099, 8085, 8082, 8191 | 8,099 | 8.10 | 1.00x |
| C++ Release | 8851, 8878, 8853, 8907, 8923 | 8,878 | 8.88 | 1.10x |
| kpp release | 8474, 7809, 7960, 8135, 7944 | 7,960 | 7.96 | **0.98x** |

What these show:
- FinalRelease's whole-program optimization and FINAL_RELEASE make the C++ game 9.5% faster on
  performance.zip and 8.8% faster on the modern save than Release.
- kpp's release game is 0.88x and 0.90x C++ Release here, as B measured (0.88x, 0.89x). It is
  already 3% and 2% faster than the official build's configuration, before a final cell exists.
- The kpp final cell is the next row: full LTO with the libraries in it, `/GS-`, and C's
  FINAL_RELEASE configuration.

Raw output: `C:\kpp-sandbox\scratch\configs\bench\runs.txt` (thread H's scratch).
