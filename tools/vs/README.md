# kpp in Visual Studio 2022 and VS Code

Background and measurements: [docs/build.md](../../docs/build.md), "Project files for an IDE".

**kppc writes the project files.** Give it the build's own arguments and one or both options:

```
kppc --generate-visual-studio-files --generate-vs-code-files --exe build\hello.exe src
```

- `build\visual-studio\hello.sln`: a Makefile project with every source file, in folders by source
  root and directory, the standard library as *Standard Library*.
- `.vscode\tasks.json`, `launch.json` and `settings.json`. A `.vscode` file kppc did not write is
  never overwritten.

Both have debug (`--debug-info`, incremental link) and release (`--release`). Each executable
goes to `build\<configuration>\` next to the `--exe` given. Generate again after changing the
arguments or adding a file. Once build.kpp exists, the arguments come from it.

- **Build:** Ctrl+Shift+B in either IDE runs kppc through its compile server with
  `--msvc-diagnostics`. Errors go to the Error List or the Problems view, with absolute paths, so
  a double-click opens the line.
- **Run, debug:** F5 builds, then starts the program under the debugger. VS Code needs the C/C++
  extension (cppvsdbg) for that; the generated settings allow breakpoints in .kpp files.
- **More options for generation:**
  - `--debug-command EXE`, `--debug-arguments "ARGS"`, `--debug-directory DIR`: what the debugger
    starts.
  - `--before-build CMD`, `--after-build CMD`: commands around the build, where `{exe}` is the
    configuration's executable and `{configuration}` its name. With `--keep-going` the
    after-build command runs even when the build fails.
- **IntelliSense and colouring in Visual Studio:** the extension kpp-visualstudio.vsix (`vsix\README.md`).
- **Colouring alone in Visual Studio,** without the extension: run `tools\vs\install.cmd` once, then restart Visual Studio. It
  links the TextMate grammar `kpp\Syntaxes\kpp.tmLanguage.json` into
  `%USERPROFILE%\.vs\Extensions\kpp`.

**programs/factorio:** `factorio\generate.cmd` writes `programs\factorio\build\visual-studio\factorio.sln`
and `programs\factorio\.vscode`. The project has run.cmd's build with these steps:
- `native\build.cmd` before it;
- `copy-to-game.cmd` after it, which puts factorio.exe and its PDB into the game's
  `bin\x64` directory;
- F5 starts the game there with `--no-crash-handler`.

It compiles the converted tree in the factorio-in-kpp checkout C:\factorio-in-kpp (its src\ and libraries\). The environment variables `FIK` (another
converted tree) and `GAME` (another game directory) change this when generate.cmd runs.

`measure.ps1` times edit→build inside a Visual Studio of its own, through DTE, and checks the
Error List and its navigation (usage in the script's header).
