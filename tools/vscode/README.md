# Kpp for VS Code

Colouring for `.kpp` files, and IntelliSense from the Kpp compiler's language server:
- errors as you type;
- hover, go to definition, find references, symbols;
- completion and rename.

The extension starts `kpp --lsp` for the open folder. That server checks the project as `kpp`
builds it: build.kpp's sources for one configuration, or with no build.kpp, every `.kpp` file in the
folder.

## Install
- **From a Kpp release:** the installer offers to install `kpp.vsix` into VS Code. By hand, run
  `code --install-extension "C:\Program Files\Kpp\kpp.vsix"`, or use *Extensions > ... > Install from
  VSIX*.
- **From the repository:** in `tools\vscode`, run `npm ci`, then `npm run package`, then
  `code --install-extension kpp.vsix`.

## Settings
- `kpp.path`: the compiler, when `kpp` is not on the PATH (`C:\kpp\compiler\build\kppc.exe` in a
  checkout).
- `kpp.configuration`: one of build.kpp's configurations, `debug` by default.
- `kpp.target`: one of build.kpp's executables. Unset, it covers all of them. (`kpp.executable`, its
  old name, is still read.)

The status bar shows the target and configuration the code is checked in; clicking it (or
*Kpp: Select the Target and Configuration*) lists build.kpp's and switches. The server keeps the
program of each one it checked, so switching back is quick.
- `kpp.settings`: build.kpp's settings, as `kpp --set NAME=VALUE` gives them:
  `{ "PREBUILT": "D:/Factorio/vs2022/Releasex64" }`.
- `kpp.trace.server`: `messages` or `verbose` logs the protocol in the *Kpp* output.

## Commands
- *Kpp: Select the Target and Configuration*.
- *Kpp: Restart the Language Server*.
- *Kpp: Show the Language Server's Output*.

Building and debugging go through the project files `kpp --generate-vs-code-files` writes. Other
editors are covered in [docs/editors.md](https://github.com/kovarex/kpp/blob/master/docs/editors.md).

## Developing the extension
- `npm ci` installs the dependencies. `npm run grammar` copies the TextMate grammar from
  `tools\vs\kpp\Syntaxes` into `syntaxes\`; packaging does that by itself.
- `npm run smoke` checks the protocol the extension relies on against `test\stub-server.js`, a
  stand-in server. `node test\lsp-smoke.js PATH\kppc.exe --lsp` runs the same check against the
  compiler's own server.
