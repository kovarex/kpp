# Kpp in editors

IntelliSense comes from one place: the compiler's language server, `kpp --lsp`. It speaks the
Language Server Protocol over stdin and stdout, so any editor with an LSP client can use it. This
page has the server's interface and the setup for each editor. For building and debugging from
an IDE, see [build.md](build.md), "Project files for an IDE".

Status (2026-10-31):
- `kppc --lsp` is on master (thread F, ff6054ab), on the interface below. run_tests checks it
  with the smoke client.
- The VS Code extension is ready. Its protocol is checked; a run inside a real VS Code is not yet,
  because VS Code is not installed on PC-PRACOVNA.
- The Visual Studio extension is not built yet: the VS SDK workload and the .NET 8 SDK are not
  installed.

## The language server

- **Launch.** `kpp --lsp` (in a checkout: `compiler\build\kppc.exe --lsp`), with no other
  arguments. `--lsp-log FILE` writes a trace. The standard library is found next to the compiler,
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
    configuration's name in the generated Visual Studio files (`Debug`); `libraries` the Kpp
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
- **Install:** the Kpp installer offers to install it into VS Code. By hand, run
  `code --install-extension kpp.vsix`, with `kpp.vsix` from the release or from `npm run package`
  in tools\vscode.
- **What it adds:** colouring, from the same TextMate grammar Visual Studio uses, and a language
  client that starts `kpp --lsp` for the open folder.
- **Settings:** `kpp.path`, `kpp.configuration`, `kpp.target` (`kpp.executable`, its old name, is
  still read), `kpp.settings` and `kpp.trace.server`.
- **The cell:** the status bar shows the target and configuration the code is checked in; a click
  (or *Kpp: Select the Target and Configuration*) picks another from `kpp/cells` and writes the
  workspace's `kpp.target` and `kpp.configuration`.
- **Build, run and debug:** use `kpp --generate-vs-code-files` (build.md).

## Visual Studio 2022

**Visual Studio has no setting that starts a language server: an extension (VSIX) has to provide
it.** Two ways exist:
1. **VisualStudio.Extensibility** (Visual Studio 17.9 and later). An out-of-process .NET extension
   declares a `LanguageServerProvider` for `.kpp` files and returns the `kpp --lsp` process's
   streams. It works in Open Folder mode and in solutions. This is the current API and the one to
   use.
2. **The older in-process API.** A VSIX exports an `ILanguageClient`
   (Microsoft.VisualStudio.LanguageServer.Client).

Either way, building a VSIX needs the "Visual Studio extension development" workload (the VS SDK)
and a .NET SDK. Neither is installed on PC-PRACOVNA, so the VSIX waits for kovarex's go-ahead
(question 1 below).

What works today without it:
- **Colouring:** `tools\vs\install.cmd`.
- **Errors in the Error List:** the build of the generated solution (build.md).

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

There is no tree-sitter grammar for Kpp yet. `vim.treesitter.language.register('cpp', 'kpp')`
gives C++ colouring, which fits most Kpp code.

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
- Until Kpp has a `.sublime-syntax` of its own, the client also attaches to C++ files. Enable it
  per project (in the project's settings), not globally.

**Emacs** (eglot, built in since Emacs 29):

```elisp
(define-derived-mode kpp-mode c++-mode "Kpp")
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
   `npm run grammar`, or the `extension` directory of kpp.vsix unzipped. JetBrains reads a VS Code
   extension's grammar from it.

## Questions for kovarex

1. **Installing the tools** (allowed by kovarex, relayed by the coordinator 2026-10-31). E's session
   permissions refused the install commands, so they still need running, by kovarex or on his
   direct instruction in E's session:
   - VS Code: `winget install --id Microsoft.VisualStudioCode -e --scope user`;
   - the .NET 8 SDK: `winget install --id Microsoft.DotNet.SDK.8 -e`;
   - the "Visual Studio extension development" workload, added with the Visual Studio Installer.
