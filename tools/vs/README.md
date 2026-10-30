# Kpp in Visual Studio 2022

Background and measurements: [docs/build.md](../../docs/build.md), "Visual Studio".

- **Open a program:** File > Open > Project/Solution, then `tools\vs\hello\hello.vcxproj` or
  `tools\vs\factorio\factorio.vcxproj`. Visual Studio makes a solution for it on first open.
- **Build:** Ctrl+Shift+B runs `kppc --server --msvc-diagnostics` (Kpp.props). The first build
  starts the compile server, and later builds reuse it. Errors appear in the Error List;
  double-click one to open its line.
- **Run, debug:** F5 builds, then starts the program under the debugger.
- **Configurations:** Debug adds `--debug-info` (line tables and a PDB), unless the project sets
  `KppDebugInfo` to false. Release adds `--release`.
- **Colouring:** run `tools\vs\install.cmd` once, then restart Visual Studio. It links the
  TextMate grammar `kpp\Syntaxes\kpp.tmLanguage.json` into `%USERPROFILE%\.vs\Extensions\kpp`.

A new program: copy `hello\hello.vcxproj` next to its sources, give it a new ProjectGuid, and set
`KppSources` (directories or files, separated by spaces) and `KppOutput`. Add more kppc options
with `KppArguments`.

**programs/factorio:** `factorio\build.cmd` builds as `programs\factorio\run.cmd` does and copies
factorio.exe and its PDB into the game's directory, C:\factorio-in-kpp\bin\x64. F5 starts it
there with `--no-crash-handler`. It compiles the converted tree in C:\factorio-in-kpp\kpp.
Environment variables, set before Visual Studio starts, change this: `FIK` names another
converted tree, `GAME` another game directory.

`measure.ps1` times edit→build inside a Visual Studio of its own, through DTE, and checks the
Error List and its navigation (usage in the script's header).
