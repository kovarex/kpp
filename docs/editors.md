# Kpp in editors

IntelliSense comes from one place: the compiler's language server, `kpp --lsp`. It speaks the
Language Server Protocol over stdin and stdout, so any editor with an LSP client can use it. This
page has the server's interface and the setup for each editor. For building and debugging from
an IDE, see [build.md](build.md), "Project files for an IDE".

Status (2026-10-31):
- Thread F is implementing the server on the compile server's kept program.
- The VS Code extension is ready and checked against a stand-in server (tools/vscode/test).
- The editor setups below follow the agreed interface and become live when `kpp --lsp` lands.

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
    "executable": null,
    "settings": { "PREBUILT": "D:/Factorio/vs2022/Releasex64" },
    "buildFile": "C:/project/build.kpp" }
  ```

  - `configuration`: `debug` (the default) or `release`.
  - `executable`: one of build.kpp's executables. `null` takes every executable's roots and
    excludes together.
  - `settings`: build.kpp's settings, the `kpp --set` values.
  - `buildFile`: a build.kpp to use instead of the walk up.

  `workspace/didChangeConfiguration` with `{ "kpp": { the same keys } }` makes the server describe
  the project again. So does saving build.kpp.
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
- **Settings:** `kpp.path`, `kpp.configuration`, `kpp.executable`, `kpp.settings` and
  `kpp.trace.server`.
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

1. **The Visual Studio language client.** May I install the "Visual Studio extension development"
   workload and the .NET 8 SDK, so that the VisualStudio.Extensibility VSIX can be built and
   tested here and in the release workflow?
2. **Testing the VS Code extension end to end.** VS Code is not installed on PC-PRACOVNA. One
   option is installing it. The other is letting the tests download a VS Code build
   (@vscode/test-electron), so that a test opens a .kpp file in a real VS Code and waits for the
   server's diagnostics. Until then, only the protocol is checked (the smoke client), not the
   extension running inside VS Code.
