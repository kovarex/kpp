# Getting started with kpp

kpp is a C++-like language and its compiler. This page covers installing it, making a first program
and building a project. The decided language rules are in [design.md](design.md).

## Install

kpp runs on 64-bit Windows 10 and 11, and builds programs for Linux and FreeBSD too (`--target`);
other hosts are [planned](../readme.md#planned).

1. Download the newest release from
   [github.com/kovarex/kpp/releases/latest](https://github.com/kovarex/kpp/releases/latest):
   - `kpp-VERSION-setup.exe` installs into `C:\Program Files\kpp` and adds it to the PATH;
   - or `kpp-VERSION.zip`: unzip it anywhere and run `kpp.exe` from that folder.
2. Open a new terminal and check it:
   ```
   kpp --version
   ```

A release brings its own toolchain (clang and lld from LLVM, with MinGW-w64's Windows headers and
libraries), so a kpp program needs nothing else installed: no Visual Studio, no Windows SDK.

## A first program

```
kpp new hello
cd hello
kpp hello --run
```

`kpp new` writes `build.kpp`, `src\main.kpp` and a `.gitignore`. Edit `src\main.kpp` and run the last
command again; the compile server keeps the program in memory, so a rebuild compiles only what
changed.

A single file needs no project: `kpp --run hello.kpp`.

## Building a project

In a directory with a `build.kpp`, or below one:

| Command | What it does |
|---|---|
| `kpp` | a menu of the commands below |
| `kpp debug`, `kpp release` | builds every target in that configuration |
| `kpp TARGET CONFIGURATION` | builds one target in one configuration, into `build\CONFIGURATION\TARGET.exe` |
| `kpp TARGET --run ARGS` | builds and runs it |
| `kpp --test` | builds and runs the tests |
| `kpp options` | lists the used libraries' options and their values |
| `kpp --generate-visual-studio-files` | writes a Visual Studio solution (one project per target, one configuration per named configuration) |
| `kpp --generate-vs-code-files` | the same for VS Code |
| `kpp --version`, `kpp --help` | the version, the toolchain a build would use, the usage |

Targets, configurations (`debug`, `release`, your own), library options and the toolchain are set in
`build.kpp`, written in kpp itself: see [build.md](build.md).

## Editors

- **VS Code:** the installer offers the extension (`kpp-vscode.vsix`, also in the release); it colours `.kpp`
  files and gives completion, errors and navigation through the compiler's language server
  (`kpp --lsp`).
- **Visual Studio:** generate a solution with `kpp --generate-visual-studio-files`.
- **Other editors:** anything that speaks the Language Server Protocol, see [editors.md](editors.md).

## Projects that link Visual Studio-built C++ libraries

A project whose `build.kpp` says `--toolchain msvc` (factorio-in-kpp does, because it links C++
libraries built the Visual Studio way) needs Visual Studio 2022 or its Build Tools with:
- "MSVC v143 - VS 2022 C++ x64/x86 build tools",
- "C++ Clang Compiler for Windows",
- a Windows 10 or 11 SDK.

kpp says so before it compiles anything when one of them is missing.

## Building kpp itself

```
git clone https://github.com/kovarex/kpp.git
kpp\compiler\build.cmd
```

This needs the same Visual Studio components plus "C++ CMake tools for Windows"; the result is
`compiler\build\kppc.exe`, the same program a release names `kpp.exe`. `tools\release\package.cmd`
makes the release package and installer.
