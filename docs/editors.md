# kpp in editors

IntelliSense comes from one place: the compiler's language server, `kpp --lsp`. It speaks the
Language Server Protocol over stdin and stdout, so any editor with an LSP client can use it. This
page has the server's interface and the setup for each editor. For building and debugging from
an IDE, see [build.md](build.md), "Project files for an IDE".

Status (2026-10-31):
- `kppc --lsp` is on master (thread F, ff6054ab), on the interface below. run_tests checks it
  with the smoke client.
- The VS Code extension is ready and checked in a real VS Code 1.144 (2026-11-02, see "VS Code"):
  errors, completion, go to definition, hover with config:: values, and switching cells, on
  tests\lsp\cells and on factorio-in-kpp.
- The Visual Studio extension, kpp-visualstudio.vsix (tools\vs\vsix), is checked in a real Visual Studio
  17.14 (2026-11-02, see "Visual Studio 2022"): colouring, go to definition, go to declaration
  and errors, on tests\lsp\cells and on factorio-in-kpp.

## The language server

- **Launch.** `kpp --lsp` (in a checkout: `compiler\build\kppc.exe --lsp`), with no other
  arguments. `--stdio`, which vscode-languageclient adds, is taken; `--pipe`, `--socket` and
  `--node-ipc` are refused. `--lsp-log FILE` writes a trace. A check that fails with no diagnostic
  (an option the build does not take) is shown as a `window/showMessage` error, once per failure. The standard library is found next to the compiler,
  as for builds.
- **Project.** The client's root (rootUri, or the first workspace folder) is the starting point.
  The server walks up from it to build.kpp, as `kpp` does. It checks the sources build.kpp
  describes, for one configuration. With no build.kpp, every `.kpp` file under the root is the
  program, as for `kppc DIR`.
- **initializationOptions.** Every key is optional:

  ```json
  { "configuration": "debug",
    "target": null,
    "settings": { "PREBUILT": "D:/Factorio/vs2022/Releasex64" },
    "buildFile": "C:/project/build.kpp" }
  ```

  - `configuration` and `target`: the cell (below). `target` is one of build.kpp's executables;
    `null` takes every executable's roots and excludes together. `executable` is read as `target`
    too (its old name).
  - `settings`: build.kpp's settings, the `kpp --set` values.
  - `buildFile`: a build.kpp to use instead of the walk up.

  - `keptCells`: how many cells keep their program (4 by default; about 6 GB each on the game).

  `workspace/didChangeConfiguration` with `{ "kpp": { the same keys } }` makes the server describe
  the project again. So does saving build.kpp.
- **Cells.** The code is checked in the context of one cell, a target and a configuration of
  build.kpp's: the one selected in the editor. The server switches cells without a restart and
  keeps the program of each cell it checked, so switching back is quick.
  - `kpp/cells` (request, no parameters) answers
    `{ "cells": [{ "target", "configuration", "visualStudio", "libraries": [{ "name", "unscoped" }] }], "active": { "target", "configuration" } }`:
    every target in every configuration, in build.kpp's order; `visualStudio` is the
    configuration's name in the generated Visual Studio files (`Debug`); `libraries` the kpp
    libraries the cell uses, each scoped (its names are `NAME::x`) or unscoped (design.md 16).
    Without a build.kpp the list is empty and `active` null.
  - `kpp/selectCell` (request) `{ "target", "configuration" }` makes that cell the checked one and
    answers with it, or with an error (-32803) naming what build.kpp has; the cell then stays.
    `configuration` may be a Visual Studio solution configuration: `Debug|x64` is matched by the
    part before `|` against `visualStudio`, any case.
  - `kpp/activeCell` (notification from the server) `{ "target", "configuration" }` after each
    switch, the first check's cell too: for a status bar.
  - What the clients send: VS Code the settings `kpp.target` and `kpp.configuration`
    (didChangeConfiguration), which its status-bar picker on `kpp/cells` sets; other editors the
    same settings, or `kpp/selectCell`; Visual Studio (its extension, not built yet) its active
    solution configuration and startup project, with `kpp/selectCell` when either changes. A cell
    build.kpp does not have is shown as an error and the checked cell stays.
- **Capabilities:**
  - incremental text sync;
  - pushed diagnostics (`textDocument/publishDiagnostics`, source `kpp`), also for files the
    editor has not opened;
  - hover, go to definition, references, document symbols, workspace symbols;
  - completion, triggered by `.`, `>` and `:`;
  - rename, with prepareRename.
- **Positions** are UTF-16 columns, the protocol's default.
- **Checked by** `tools\vscode\test\lsp-smoke.js`: `node lsp-smoke.js kppc.exe --lsp`. It
  initializes, expects these capabilities, expects an error for an undeclared name and expects
  it gone after the fix, then asks a hover. It needs only Node.js.

## VS Code

The extension is in `tools/vscode`; its README covers the details.
- **Install:** the kpp installer offers to install it into VS Code. By hand, run
  `code --install-extension kpp-vscode.vsix`, with `kpp-vscode.vsix` from the release or from `npm run package`
  in tools\vscode.
- **What it adds:** colouring, from the same TextMate grammar Visual Studio uses, and a language
  client that starts `kpp --lsp` for the open folder.
- **Settings:** `kpp.path`, `kpp.configuration`, `kpp.target` (`kpp.executable`, its old name, is
  still read), `kpp.settings` and `kpp.trace.server`.
- **The cell:** the status bar shows the target and configuration the code is checked in; a click
  (or *kpp: Select the Target and Configuration*) picks another from `kpp/cells` and writes the
  workspace's `kpp.target` and `kpp.configuration`.
- **Build, run and debug:** use `kpp --generate-vs-code-files` (build.md).
- **Checked in VS Code** by `tools\vscode\test\vscode\run.ps1`. It installs kpp-vscode.vsix into a profile
  of its own (`--user-data-dir`, `--extensions-dir`), opens a project in VS Code, and drives the
  extension through VS Code's commands. `cells.js` runs on a copy of tests\lsp\cells, `game.js` on
  a copy of factorio-in-kpp. It opens a window and writes `NAME.log` and a screenshot of the window,
  `NAME.png`, into compiler\build\vscode; the game's copy there is 4.7 GB, to delete afterwards. Results on 2026-11-02 (factorio-in-kpp e4cfd9ba, a copy):
  - cells: 19 of 19 checks pass. A typed error appears and goes after undo. Completion offers
    `shared`. Go to definition works. Hover on `config::sound::volume` gives `7`, who set it and
    the default. The status-bar picker switches between the 6 cells, and the release cell's
    static_assert comes and goes.
  - The game, two runs: the first check of `factorio | debug` takes 14.8 and 15.7 s; then go to
    definition 3 and 6 ms, completion after `SystemUtil::` 0.9 s (43 items). The switch to release
    takes 16.0 and 15.0 s (its first check), back to debug 0.7 and 1.5 s, to release again 3.7 and
    5.8 s (kept cells). The check after an erroring one is slow: typing `SystemUtil::` checks in
    0.6 s with its error, the undo that fixes it in 11.3 s.

## Visual Studio 2022

**kpp-visualstudio.vsix** (`tools\vs\vsix`, its README has the details) is the extension: colouring, from the
TextMate grammar it bundles, and IntelliSense from `kpp --lsp`. Install it by double-clicking
kpp-visualstudio.vsix (`tools\vs\vsix\build.cmd` makes it; it needs the .NET 8 SDK), then restart Visual
Studio. `tools\vs\install.cmd`, the grammar alone, is not needed with it.
- **What it gives:** errors as squiggles and in the Error List, go to definition (F12,
  Edit.GoToDefinition), go to declaration (Ctrl+F12, Edit.GoToDeclaration), hover, completion and
  references.
- **Go to declaration:** Visual Studio's language client sends `textDocument/definition` for
  Edit.GoToDefinition but leaves Edit.GoToDeclaration unavailable. The extension handles that
  command in .kpp editors: it sends `textDocument/declaration` itself and opens the answer.
- **Why in-process:** that handler needs to send the server a request of its own. The in-process
  client (`ILanguageClient`, Microsoft.VisualStudio.LanguageServer.Client) allows it through
  `ILanguageClientCustomMessage2`. VisualStudio.Extensibility's out-of-process
  `LanguageServerProvider` does not, so the first build, which used it, had no go to declaration.
- **Checked in Visual Studio 17.14.42** by `tools\vs\vsix\test.ps1`, in the experimental instance on
  copies of the projects, opened as folders (2026-11-02):
  - tests\lsp\cells, from `println(shared())` in src\game\main.kpp: go to definition reaches
    src\common\shared.kpp:1 in 12.0 s (the server's first check included), go to declaration the
    same in 0.5 s. A typed error is in the Error List after 1.2 s and gone 2.4 s after undo.
  - factorio-in-kpp e4cfd9ba, from `super::setSize` in VideoBitmapDX11.kpp:77: go to definition
    reaches VideoBitmap.kpp:134 (the out-of-line body) in 40.5 s, the server's first check taking
    32.0 s of it; go to declaration reaches VideoBitmap.kpp:20 (the declaration in the class) in
    0.6 s. A typed error is in the Error List after 16.7 s and gone 35.4 s after undo (checks of
    15.5 and 22.6 s).
  - tests\lsp\cells as the solution `kpp --generate-visual-studio-files` writes: the server starts
    in the cell *game, debug*; go to definition in 5.3 s, go to declaration in 0.6 s. The
    configuration Release shows the game's static_assert error after 2.7 s. The startup project
    tool clears it after 2.4 s, and game in Debug is clean after 1.3 s.
  - factorio-in-kpp as its generated solution (2 projects, 9339 files): the server starts in
    *factorio, debug*. Go to definition takes 33.3 s with the first check (22.9 s, beside
    Visual Studio loading the solution), go to declaration 0.6 s. A typed error is in the Error
    List after 3.9 s and gone 3.7 s after undo (checks of 0.8 and 1.0 s).
  - In a folder, the server checks every target at once (no cell is chosen), which on the game
    is factorio and factorio-test together: 32 s for the first check where VS Code's `factorio`
    takes 15 s.
- **The cell** (see "Cells") is the solution's. In a solution kppc generated (one project per
  target, named after it), the startup project is the target and the active configuration is the
  configuration. The server starts in that cell (initializationOptions). Picking another
  configuration or startup project sends `kpp/selectCell`, and the status bar shows the server's
  answer. In a folder, or in a solution kppc did not write, the server checks every target in
  debug.

## Other editors

The commands below assume `kpp` is on the PATH (the installer puts it there). In a checkout, use
the full path of `compiler\build\kppc.exe` instead.

**Neovim** (0.11 or later), in init.lua:

```lua
vim.filetype.add({ extension = { kpp = 'kpp' } })
vim.lsp.config('kpp', {
  cmd = { 'kpp', '--lsp' },
  filetypes = { 'kpp' },
  root_markers = { 'build.kpp', '.git' },
  init_options = { configuration = 'debug' },
})
vim.lsp.enable('kpp')
```

There is no tree-sitter grammar for kpp yet. `vim.treesitter.language.register('cpp', 'kpp')`
gives C++ colouring, which fits most kpp code.

**Helix**, in languages.toml (Helix sends `config` as the initializationOptions):

```toml
[language-server.kpp]
command = "kpp"
args = ["--lsp"]
config = { configuration = "debug" }

[[language]]
name = "kpp"
scope = "source.kpp"
file-types = ["kpp"]
roots = ["build.kpp"]
comment-token = "//"
indent = { tab-width = 2, unit = "  " }
language-servers = ["kpp"]
grammar = "cpp"
```

**Sublime Text**, with the LSP package, in Preferences > Package Settings > LSP > Settings:

```json
{
  "clients": {
    "kpp": {
      "enabled": true,
      "command": ["kpp", "--lsp"],
      "selector": "source.c++",
      "initializationOptions": { "configuration": "debug" }
    }
  }
}
```

- Open a `.kpp` file and choose *View > Syntax > Open all with current extension as... > C++*.
- Until kpp has a `.sublime-syntax` of its own, the client also attaches to C++ files. Enable it
  per project (in the project's settings), not globally.

**Emacs** (eglot, built in since Emacs 29):

```elisp
(define-derived-mode kpp-mode c++-mode "kpp")
(add-to-list 'auto-mode-alist '("\\.kpp\\'" . kpp-mode))
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(kpp-mode . ("kpp" "--lsp" :initializationOptions (:configuration "debug")))))
```

**CLion, Rider and other JetBrains IDEs**, with the LSP4IJ plugin (Red Hat; it works in the
Community editions too):
1. *Settings > Languages & Frameworks > Language Servers*, then *+*.
   - Command: `kpp --lsp`.
   - Mappings: file name pattern `*.kpp`.
   - Configuration: initialization options `{ "configuration": "debug" }`.
2. For colouring: *Settings > Editor > TextMate Bundles*, then *+*. Pick tools\vscode after
   `npm run grammar`, or the `extension` directory of kpp-vscode.vsix unzipped. JetBrains reads a VS Code
   extension's grammar from it.
