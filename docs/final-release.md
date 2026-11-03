# kpp's best release against C++ FinalRelease

The official Factorio releases are built in fbuild.bff's FinalRelease configuration. A fair runtime
comparison puts kpp's best release build against that, not against C++ Release (kovarex,
2026-11-03). This page records what FinalRelease is, what the converted game and kppc need to build
the same program, and the measurements. Thread H leads it. The converter's part belongs to C, the
build cell's to B.

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
   /MT already) and `/GS-` (no stack cookies: check that kppc's clang adds no stack protector).
   `/Oi /Ot /Ob2` are what clang's -O2 does by default.

Optimization level: `/O2` is MSVC's highest. The fair comparison is kpp's best: -O2 and -O3 both
measured, with the faster one kept and named.

## C++ FinalRelease build

`cd C:\Factorio\fastbuild && FBuild.exe -clean -summary vs2022-FinalReleasex64-factorio-run`. It
writes `build\FinalReleasex64vs2022` and `bin\FinalReleasex64vs2022`. Its time, and the benchmark,
follow when measured.
