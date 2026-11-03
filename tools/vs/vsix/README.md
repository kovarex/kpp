# kpp-visualstudio.vsix: kpp in Visual Studio 2022

The Visual Studio extension (docs/editors.md, "Visual Studio 2022"). It needs Visual Studio
2022 17.14 or later. It gives:
- colouring, from the TextMate grammar `tools\vs\kpp\Syntaxes\kpp.tmLanguage.json`, and VS
  Code's `language-configuration.json` (comments, brackets), both registered by `kpp.pkgdef`;
- IntelliSense from the compiler's language server, `kpp --lsp`: errors (squiggles and the Error
  List), go to definition (F12, Edit.GoToDefinition), hover, completion and references, through
  Visual Studio's language client;
- go to declaration (Ctrl+F12, Edit.GoToDeclaration). Visual Studio's language client leaves that
  command unavailable, so `GoToDeclaration.cs` handles it in .kpp editors: it sends
  `textDocument/declaration` on the client's connection and opens the answer;
- the cell (docs/editors.md, "Cells") from a solution kppc generated: `Cell.cs` takes the startup
  project as the target and the active configuration as the configuration. They go to the server
  at its start, and as `kpp/selectCell` when either changes.

It is an in-process extension (`ILanguageClient`, `ILanguageClientCustomMessage2`), not a
VisualStudio.Extensibility `LanguageServerProvider`. The out-of-process provider gives an
extension no way to send the server a request of its own, so it cannot serve go to
declaration. Installing it therefore needs a restart of Visual Studio.

- **The server:** `KPP_LSP_SERVER` when that is set, else `kpp.exe` on the PATH (the installer puts
  it there), else `kppc.exe`. Its log goes to `%LOCALAPPDATA%\kpp\visual-studio-lsp.log`. When no
  server is found, Visual Studio's info bar says so.
- **Build:** `build.cmd` writes `tools\vs\kpp-visualstudio.vsix`. It needs the .NET 8 SDK (`winget install
  --id Microsoft.DotNet.SDK.8 -e`) and Visual Studio's MSBuild; the "Visual Studio extension
  development" workload is not needed.
- **Install:** double-click kpp-visualstudio.vsix, or run `VSIXInstaller.exe kpp-visualstudio.vsix` from Visual
  Studio's Common7\IDE.
- **Test:** `test.ps1` installs it into the experimental instance (`/rootsuffix Exp`), so the user's
  own Visual Studio is untouched. It opens a copy of a project as a folder, or with `-Solution` as
  the solution kppc generates. It checks through DTE and UI Automation:
  - where Edit.GoToDefinition and Edit.GoToDeclaration land;
  - that a typed error reaches the Error List, and leaves it after undo;
  - with `-HoverAt` and `-CompleteAt`, Quick Info's text and the completion list's items, read from
    their popups through UI Automation;
  - with `-CellError`, that the active configuration and the startup project switch the cell.

  It ends with a screenshot of the window (usage in its header). The first start of the
  experimental instance shows Visual Studio's sign-in, which finishes on its own.
