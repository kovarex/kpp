# Mac development of a kpp Factorio: gap analysis and plan (proposal, 2026-11-13, thread O)

A Factorio developer on macOS listed what a Mac workflow needs (kovarex forwarded it on 2026-11-13).
This document takes each item, says what kpp has on master (901d881d1), what is missing, and how to get
there, with options, a recommendation, an effort estimate and an order. Nothing here is built yet: the
decisions at the end are kovarex's.

Effort is in thread-days (one thread working on it, gates included): S up to 3, M up to 10, L more.

## 0. The question under all of it: where does kpp run?

Everything on the list (CLion's language support, builds from Xcode or CLion, debugging) runs on the
developer's Mac. Today:
- **Target:** macOS arm64 and x86-64 compile to Mach-O and link with ld64.lld (design.md, "Stage 8"),
  built on Windows or Linux. Nothing built by kpp has run on a Mac yet (M10), and there is no .dSYM (M5).
- **Host:** kpp runs on Windows and Linux. host.cpp already has `__APPLE__` branches (executable path,
  stack guard, processes), but kpp has never been built or tested on macOS. Q5 (2026-11-07) says
  programs for macOS are built on Windows and Linux and only run on macOS; the roadmap lists "macOS as
  a host" as under consideration, and compilation outside Visual Studio as stage 2.
- **The game:** the kpp branch of Factorio has no macOS layer. master has 3 Objective-C++ files
  (src/MacOS/*.mm) and a Metal renderer (58 files under src/Graphics/Metal, through metal-cpp, 193 uses);
  the kpp branch keeps 25 of the Metal files and none of the .mm ones. kpp's interop is C only.

**Options:**
- **A. kpp on macOS (recommended).** Build kpp with Apple's clang (or Homebrew LLVM) on GitHub's macOS
  runners, run run_tests and the spec there, package it (a notarized tarball or .pkg, with clang, lld
  and dsymutil from LLVM 19 as Windows bundles them). The language server, builds and lldb are then
  local, as on Windows. The host layer is POSIX and Linux already works, so the work is mostly CI,
  packaging and the differences (Unix socket path, `sysctl` CPU count, case-insensitive APFS paths).
  Effort: M (about 1 to 2 weeks), plus signing and notarization (needs Wube's Apple developer account).
  It changes Q5: programs could then also be built on a Mac.
- **B. kpp on another machine, the Mac only edits and debugs.** The stage-2 compile server on a Linux
  or Windows box: the editor's language server and the build go over the network (SSH or the server's
  channel over TCP), and the built executable and .dSYM are copied back. Every request pays the network,
  every path needs mapping both ways, and the source must be the same on both machines (a sync or a
  shared checkout). Effort: M to L, and a worse result than A for an editor.
- **C. Both:** A for the developer's own Mac; B later for a fast build farm. Not needed for this list.

The rest of this document assumes A. With B, every "on the Mac" below becomes "on the server, mapped".

## 1. Debug symbols (all editors)

**Has:** for non-Windows targets kpp writes DWARF (one module flag per target): line tables, every
function as a DISubprogram, and in a debug build variables, scopes, classes with members, bases and
vtable pointer (docs/debugging.md). An optimized build keeps line tables. On macOS the DWARF stays in
the object files, which the executable's debug map names (Driver/Link.cpp).

**Missing:**
1. **The .dSYM (M5):** run `dsymutil` after the link, as Xcode and FASTBuild's Mac build do (the Xcode
   script copies `factorio-run.dSYM` beside the executable). Without it, debugging works only while the
   object files stay where they were built, and a crash report or another machine sees nothing. S.
2. **A DWARF check:** tests\debuginfo.ps1 reads only the PDB (llvm-pdbutil). Its lines.expected and
   values.expected need a DWARF twin through llvm-dwarfdump, run on every push, and a run under lldb on
   the macOS runner (break on a kpp line, `frame variable`, step) as the one end-to-end check. Things
   CodeView and DWARF do differently (member functions, which docs/debugging.md says stay free functions
   in DWARF; lambdas' names; static locals, which are not described yet) are found there. M.
3. **Views (debugValue) for LLDB:** design.md 26 says natvis first, then LLDB and GDB "once kpp writes
   DWARF". Two forms:
   - **Python in the .dSYM (recommended first):** `NAME.dSYM/Contents/Resources/Python/NAME.py`, which
     lldb loads with the symbols (it asks once, or `settings set target.load-script-from-symbol-file
     true`). Summary providers and synthetic children written per class instance from the same
     translation natvis.cpp makes. Python has loops, so the open loop question (lists, trees) goes away
     here. CLion's and Xcode's lldb both load it.
   - **LLDB's formatter bytecode** in the binary (`__lldbformatters`, LLDB 20 and later): no Python, no
     prompt, but no loops and a newer lldb than Xcode may ship. Later, if wanted.
   The standard library's own rules (compiler/src/natvis.cpp: String, Vector, HashMap ...) need the same
   in Python. M to L (1 to 2 weeks for the translation and its test beside tests\natvis.ps1).

## 2. CLion and the IntelliJ platform

### Options

- **A. A small kpp plugin on JetBrains' LSP API (recommended).** CLion (a paid IDE) has the platform's
  own LSP client (`com.intellij.platform.lsp`): a plugin of a few hundred lines of Kotlin that starts
  `kpp --lsp`, maps `*.kpp`, adds kpp's TextMate grammar (tools\vscode already has one) and the build,
  run and debug configurations (section 3). Everything the language server answers comes for free;
  each new server feature reaches CLion, VS Code, Visual Studio and the others at once. What the
  platform's client shows of each LSP request depends on the IDE version; where it shows nothing
  (clangd's inactive regions, `kpp/cells`), the plugin draws it itself. Effort: M for the plugin, S per
  extra request.
- **B. LSP4IJ, no plugin of ours.** docs/editors.md, "Other editors", already says how (works in the
  free IntelliJ editions too). Zero work, but no run configurations, no cell picker, no inactive code,
  and each developer sets it up by hand. Good for trying today; not tested by anyone on a Mac yet.
- **C. A native IntelliJ language plugin (PSI, indexes, stubs).** The platform's full model: a kpp lexer
  and parser in Kotlin, a symbol index, references, refactorings through the platform. It duplicates the
  compiler's front end in another language and must keep up with every language change (when, flags,
  debugValue, this-reference ...), and CLion's own C++ engine cannot be extended with a new language.
  L (months), and a second resolver that can disagree with kpp's. Not recommended.

### The list, item by item (with A or B)

| Item | kpp's language server today | Missing |
|---|---|---|
| highlighting | semantic tokens for every name's kind (types, functions, locals, members, enumerators ...), refreshed after each check; TextMate grammar for the rest | nothing in the server; the plugin registers the grammar |
| scoping | names resolved by the compiler's own resolver; document symbols, folding, highlights of a name in its file | nothing |
| macro expansion | see below | nothing to expand; inactive code is shown already |
| type deduction | hover names a declaration's type | **inlay hints** (`textDocument/inlayHint`): the deduced type after `auto`, a lambda's return type. S to M |
| template parameter deduction | hover on a call names the function | inlay hints or hover naming the chosen instance (`max<int32>`), and a template's body checked per instance shown for the instance under the cursor. M |
| errors and warnings | pushed diagnostics, also for files not open; quick fixes as code actions | nothing |
| files and directories of the build | the server reads build.kpp (sources, roots, excludes, libraries, per cell) | CLion's own project view (section 3) |
| build and run targets | `kpp/cells` lists every target and configuration | CLion configurations (section 3) |
| index all symbols, cross-reference | the whole program is checked and kept in memory per cell; workspace symbols | nothing |
| jump to definition | definition, declaration, type definition, implementation (derived classes, overrides) | nothing |
| find usages (not text search) | references by resolved symbol, not text | nothing |
| documentation in the tooltip | hover shows the comment above a declaration | nothing (a doc-comment convention, if wanted, is a separate decision) |
| rename | rename with prepareRename, every reference by symbol (flags declarations since 1cbdcf55d) | nothing |
| change function signature | none: LSP has no request for it | a server code action (reorder, add with a default at calls, remove, rename parameters, every call and override updated) plus a dialog in the plugin. M to L |

### "Macro expansion" in kpp

kpp has no preprocessor (design.md 7). What macros did in Factorio is done by language features the
language server already sees as code: `config` constants (hover says who set them), `when` and `if
constexpr` (the code a cell does not build is shown as inactive), `for constexpr` and reflection
(`members(T)`), caller location, assert. A C header's object-like macros are imported as constants and
show as such; its function-like macros are not imported at all (the converter expanded the game's uses
of Lua's). So there is nothing to expand. The nearest thing a developer might want is "what does this
`for constexpr` produce for this type": a hover on the loop listing its iterations would do; not
proposed unless asked for.

## 3. Build integration: CLion, Xcode, and FASTBuild's place

### What the Mac build is today (Factorio master)

- Factorio.xcodeproj has two native targets (Factorio, the app bundle; factorio-run) whose only build
  phase runs `fastbuild/fastbuild-xcode.sh`: it maps Xcode's settings (FBUILD_BUILD_TYPE,
  FBUILD_ENVIRONMENT_NAME, ARCHS) to a FASTBuild target (`<env>-<type><arch>-factorio-run`, and
  `factorio-asset-catalog` for the bundle), runs `fastbuild/osx/fbuild`, then rsyncs the executable,
  its .dSYM and the asset catalog's Resources into the place Xcode expects, merging Info.plist with
  PlistBuddy. Both architectures selected means a universal binary.
- `xcshareddata/build/generate_version_config.sh` writes Version.xcconfig from version.txt and
  scripts/build/get_build_num.sh. It does not use FASTBuild and can stay as it is.
- Eight schemes: Debug and Release, each also "AppleClang" (FASTBuild's environment choice).
- **On Factorio's kpp branch fastbuild/ is gone, and the Xcode project still calls
  fastbuild/fastbuild-xcode.sh**: its build is broken there today, and build.kpp has no macOS
  libraries or settings yet.

### The replacement

1. **kpp-xcode.sh** in place of fastbuild-xcode.sh, same contract: read CONFIGURATION (or a
   KPP_CONFIGURATION setting) and ARCHS, run `kpp factorio-run <configuration> --target macos` (and
   `macos-x64`), `lipo -create` the two for a universal binary (kpp builds one architecture per target),
   then the same rsync of the executable and .dSYM and the same Info.plist merge. Errors in Xcode's
   `file:line: error:` form, which kpp already writes. The AppleClang schemes go (one compiler). S.
2. **The asset catalog** (`factorio-asset-catalog`: actool over the xcassets): build.kpp gets a step
   for it, or kpp-xcode.sh calls `xcrun actool` itself (simpler, recommended). S.
3. **build.kpp for macOS:** the C libraries' Mac defines, sources and frameworks (SDL3's Cocoa .m files,
   curl, png ...), as it has Linux's now. Needs kpp to compile `.m` files in a C library and link
   frameworks (`-framework Cocoa`, Metal, IOKit ...), which M3 lists as unchecked. M.
4. **CLion:** CLion knows CMake, Makefiles, compile_commands.json, Bazel, Gradle and "custom build
   targets". compile_commands.json is the wrong tool: CLion would parse .kpp files as C++. Options:
   - **a generator (recommended first):** `kpp --generate-clion-files`, beside
     --generate-visual-studio-files: writes .idea's custom build targets (`kpp TARGET CONFIGURATION`,
     clean, rebuild) and one "Custom Build Application" run/debug configuration per cell, pointing at
     the executable, the game's working directory and arguments. CLion builds, runs and debugs it with
     its lldb (with the .dSYM, section 1). S to M.
   - **the plugin's own project model** (option 2A): configurations made from `kpp/cells` at runtime,
     no files written. Better once the plugin exists; can replace the generator then.

## 4. Xcode as an editor

Right: Xcode cannot be given a new language (it has taken no plugins since Xcode 8, and its source
editor extensions only edit text). What a Mac
developer keeps from Xcode with the above: building through kpp-xcode.sh, running, the debugger (lldb
with the .dSYM: breakpoints by file and line, the Python views of section 1), Instruments, the app
bundle and signing. Editing .kpp files in Xcode is plain text with no language support.

## 5. The game itself on macOS (not on the list, but needed before any of it matters)

A Mac developer needs the converted game to build and run on a Mac: the Metal renderer and the
Objective-C++ platform files. Options: port metal-cpp to kpp (it is thin wrappers over `objc_msgSend`
and selectors, which are C), or keep it and the .mm files as a C library behind a C interface, as imgui
is. That is threads B and C's stage-2 work (L), and it decides when the rest is first used for real.

## Recommended order

1. **Decide the host** (D1). With A: kpp built and tested on GitHub's macOS runners; M10 (programs run on
   the Mac); M5 (dsymutil, .dSYM). M.
2. **DWARF checked** (debuginfo.ps1's twin, an lldb run on the runner). M.
3. **The game on macOS** (section 5, B and C, in parallel with 2 to 4). L.
4. **Builds from Xcode and CLion:** kpp-xcode.sh, actool, build.kpp's Mac libraries, the CLion
   generator. M.
5. **The CLion plugin** on the LSP API, with inlay hints in the server. M.
6. **LLDB views from debugValue** (Python in the .dSYM), the standard library's rules included. M to L.
7. **Change signature** (server code action and the plugin's dialog). M to L.

Until 5, LSP4IJ (option 2B) gives a Mac developer the language server's features the day kpp runs on
the Mac.

## Decisions for kovarex

- **D1:** kpp on macOS as a host (A, recommended), or a remote compile server (B)? A changes Q5.
- **D2:** CLion: our plugin on JetBrains' LSP API (recommended), LSP4IJ only, or a native IntelliJ
  plugin?
- **D3:** LLDB views: Python in the .dSYM first (recommended), or formatter bytecode?
- **D4:** CLion's build and run: a generator first (recommended), or straight to the plugin's project
  model?
- **D5:** Change signature: in this plan (step 7), or later?
- **D6:** Who asks Wube's Mac developer what they use day to day (CLion or Xcode first), which decides
  whether step 4's Xcode part or step 5 comes first.
