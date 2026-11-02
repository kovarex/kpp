# Packages a built kppc as a kpp release (docs/build.md, "Installing"): build\release\kpp\ with
# kpp.exe (compiler\build\kppc.exe under the name users type), LICENSE.txt, stdlib\, projectlib\, the TextMate
# grammar, the VS Code extension kpp-vscode.vsix (tools/vscode) and the bundled toolchain (design.md 17:
# llvm-mingw's clang, lld and MinGW-w64, so a program builds with nothing of Visual Studio's); then
# build\release\kpp-<version>.zip, and kpp-<version>-setup.exe when Inno Setup's compiler (ISCC) is
# found. The CI workflows (ci.yml, release.yml) run it; it runs the same by hand:
#   tools\release\package.cmd
# (package.cmd runs it with -ExecutionPolicy Bypass: Windows' default policy forbids scripts.)
# The version and commit are always kppc's own (kppc --version: VERSION and the commits since it
# changed, docs/build.md "Versions"), never given by hand; they name the files, the README, the
# installer and the VS Code extension.
# The toolchain comes from tools\sysroot\llvm-mingw.ps1 (pinned, SHA-256 checked, unpacked once into
# build\llvm-mingw). -IsccPath names Inno Setup's compiler when it is not installed in a usual place.
# -NoSmokeTest leaves the smoke test out, for iterating by hand; CI always runs it.
# The VS Code extension builds in a job of its own while the rest is copied and tested (each step's
# time is printed at the end; docs/build.md, "Installing").
param([string]$IsccPath = '', [switch]$NoSmokeTest, [string]$Version, [string]$Commit)
$ErrorActionPreference = 'Stop'
if ($Version -or $Commit) { throw "package.ps1 takes no -Version or -Commit: they are kppc's own (kpp --version; docs/build.md, `"Versions`")" }
if ($NoSmokeTest -and $env:CI -eq 'true') { throw 'package.ps1 -NoSmokeTest is for local runs: CI always runs the smoke test' }
$repository = (Resolve-Path "$PSScriptRoot\..\..").Path
$out = Join-Path $repository 'build\release'
$kpp = Join-Path $out 'kpp'

$printed = @(& "$repository\compiler\build\kppc.exe" --version)
if ($LASTEXITCODE -ne 0 -or $printed[0] -notmatch '^kpp (\d+\.\d+\.\d+(?:\+dirty)?), commit ([0-9a-f]{40})$') { throw "compiler\build\kppc.exe has no version to package (kppc --version: $($printed -join ' / '))" }
$Version = $Matches[1]
$Commit = $Matches[2]
Write-Output "packaging kpp $Version, commit $Commit"
# Each step's time, printed at the end (docs/build.md, "Installing").
$clock = [Diagnostics.Stopwatch]::StartNew()
$times = [ordered]@{}
function Lap([string]$step) { $script:times[$step] = $script:clock.Elapsed.TotalSeconds; $script:clock.Restart() }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $kpp | Out-Null
Lap 'clean'

# The VS Code extension needs Node.js (npm). CI's runner has it, and there it is an error without; by
# hand the package leaves kpp-vscode.vsix out and says so.
$npm = [bool](Get-Command npm -ErrorAction SilentlyContinue)
if (-not $npm)
{
  if ($env:CI -eq 'true') { throw 'Node.js (npm) not found: CI builds the VS Code extension kpp-vscode.vsix' }
  Write-Output 'kpp-vscode.vsix skipped: Node.js (npm) not found; install Node.js LTS to build it'
}
# The VS Code extension (tools/vscode), versioned as the release, with its dependencies from
# package-lock.json: a job of its own, waited for before the zip.
$vsix = if ($npm)
{
  Start-Job -ArgumentList "$repository\tools\vscode", $repository, $Version, "$kpp\kpp-vscode.vsix" -ScriptBlock {
    param($folder, $repository, $Version, $target)
    $ErrorActionPreference = 'Continue'
    Set-Location $folder
    # node_modules as npm ci made it from this package-lock.json (its SHA-256 kept beside it) is used
    # again: npm ci takes 15 s. CI's checkout is new each run.
    $lock = (Get-FileHash -Algorithm SHA256 package-lock.json).Hash
    if ((Get-Content node_modules\.kpp-lock-sha256 -ErrorAction SilentlyContinue) -ne $lock)
    {
      npm ci --no-audit --no-fund 2>&1 | ForEach-Object { "$_" }
      if ($LASTEXITCODE -ne 0) { throw "npm ci failed ($LASTEXITCODE)" }
      Set-Content node_modules\.kpp-lock-sha256 $lock
    }
    else { 'node_modules: as npm ci made it from this package-lock.json' }
    # vsce puts the extension folder's LICENSE into the .vsix: the repository's, copied (.gitignore'd).
    Copy-Item "$repository\LICENSE" LICENSE
    npx vsce package $Version --no-git-tag-version --no-update-package-json --out $target 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { throw "vsce failed ($LASTEXITCODE)" }
  }
}
# Directories copied by robocopy, many files at once (Copy-Item -Recurse took 10 to 17 s); its exit
# codes below 8 are success.
function CopyTree([string]$from, [string]$to)
{
  robocopy $from $to /E /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "copying $from to $to failed (robocopy $LASTEXITCODE)" }
  $global:LASTEXITCODE = 0
}

Copy-Item "$repository\compiler\build\kppc.exe" "$kpp\kpp.exe"
# Apache 2.0's section 4(a): every copy carries the license (LICENSE, with its third-party section).
Copy-Item "$repository\LICENSE" "$kpp\LICENSE.txt"

# The bundled toolchain, at its paths in llvm-mingw so clang finds the rest relative to its bin\ and
# kpp finds bin\ next to itself: clang (which also compiles a project's C and C++ sources), lld,
# llvm-ar, llvm-rc, llvm-dlltool and their DLLs; MinGW-w64's and libc++'s headers; clang's headers
# and builtins; the x86_64 import libraries and runtime. Not the i686, armv7 and aarch64 trees,
# python, lldb, clangd or clang-tidy.
$llvmMingw = & "$repository\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1
Lap 'toolchain'
New-Item -ItemType Directory -Force "$kpp\bin", "$kpp\lib\clang\19\lib\windows", "$kpp\x86_64-w64-mingw32\share" | Out-Null
foreach ($file in 'clang.exe', 'clang-19.exe', 'ld.lld.exe', 'llvm-ar.exe', 'llvm-rc.exe', 'llvm-dlltool.exe', 'libLLVM-19.dll',
  'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll', 'x86_64-w64-windows-gnu.cfg', 'mingw32-common.cfg')
{
  Copy-Item "$llvmMingw\bin\$file" "$kpp\bin"
}
CopyTree "$llvmMingw\include" "$kpp\include"
CopyTree "$llvmMingw\lib\clang\19\include" "$kpp\lib\clang\19\include"
Copy-Item "$llvmMingw\lib\clang\19\lib\windows\libclang_rt.builtins-x86_64.a" "$kpp\lib\clang\19\lib\windows"
CopyTree "$llvmMingw\x86_64-w64-mingw32\lib" "$kpp\x86_64-w64-mingw32\lib"
CopyTree "$llvmMingw\x86_64-w64-mingw32\share\mingw32" "$kpp\x86_64-w64-mingw32\share\mingw32"
Copy-Item "$llvmMingw\LICENSE.TXT" "$kpp\LICENSE-llvm-mingw.txt"
@"
The bundled toolchain (bin\, include\, lib\, x86_64-w64-mingw32\) is llvm-mingw 20250114
(https://github.com/mstorsjo/llvm-mingw), unchanged. Its parts keep their own licences:
- LLVM (clang, lld, the LLVM tools, libc++, libunwind, compiler-rt): Apache-2.0 WITH LLVM-exception,
  as kpp; LICENSE-llvm-mingw.txt.
- MinGW-w64's headers and runtime: public domain or ZPL 2.1; the DirectX headers taken from Wine are
  LGPL-2.1 or later. x86_64-w64-mingw32\share\mingw32\COPYING*.
- llvm-mingw's own scripts: ISC.
"@ | Set-Content "$kpp\NOTICE-toolchain.txt"

CopyTree "$repository\stdlib" "$kpp\stdlib"
CopyTree "$repository\projectlib" "$kpp\projectlib"
New-Item -ItemType Directory -Force "$kpp\grammar" | Out-Null
Copy-Item "$repository\tools\vs\kpp\Syntaxes\kpp.tmLanguage.json" "$kpp\grammar"
Lap 'copy'
@"
kpp $Version$(if ($Commit) { " ($Commit)" })

kpp.exe is the compiler. In a project's directory (one with build.kpp), run kpp for a menu, or
kpp (every target, debug), kpp release, kpp TARGET CONFIGURATION, kpp TARGET --run,
kpp --generate-visual-studio-files. kpp new makes a project. Documentation: https://github.com/kovarex/kpp/blob/master/docs/build.md

Nothing else is needed: kpp brings its own toolchain (bin\: llvm-mingw's clang and lld with
MinGW-w64, NOTICE-toolchain.txt), and its programs use only DLLs every Windows 10 and 11 has.
kpp --toolchain msvc builds with an installed Visual Studio's clang instead, on request.

kpp is licensed under the Apache License 2.0 with LLVM Exceptions: LICENSE.txt.

Editors:
$(if ($npm) { "- VS Code: kpp-vscode.vsix (colouring, and IntelliSense through kpp --lsp). The installer offers to
  install it; or run code --install-extension kpp-vscode.vsix. Not by double-click: Windows opens
  every .vsix in Visual Studio's installer, which does not take a VS Code extension." } else { "- VS Code: the extension (kpp-vscode.vsix) is not in this package: it was made without Node.js." })
- Visual Studio: the grammar below; Visual Studio's own extension, when a package has it, is
  kpp-visualstudio.vsix.
- Others: https://github.com/kovarex/kpp/blob/master/docs/editors.md

grammar\kpp.tmLanguage.json colours .kpp files in Visual Studio: copy it into
%USERPROFILE%\.vs\Extensions\kpp\Syntaxes\ and restart Visual Studio.
"@ | Set-Content "$kpp\README.txt"

# A smoke test of the package itself: kpp new and kpp --run in a project outside the repository, as on
# a machine without Visual Studio: PATH cut to Windows' own directories, no INCLUDE, LIB or KPP_CLANG
# (CI sets it for the tests). It must build with the bundled toolchain for windows-gnu, and the
# program must import only Windows' own DLLs (llvm-objdump from the unpacked llvm-mingw, not shipped).
if ($NoSmokeTest) { Write-Output 'smoke test skipped (-NoSmokeTest)' }
else
{
$smoke = Join-Path $out 'smoke'
New-Item -ItemType Directory -Force $smoke | Out-Null
$saved = @{}
foreach ($name in 'PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'KPP_CLANG') { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location $smoke
try
{
  $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
  foreach ($name in 'INCLUDE', 'LIB', 'LIBPATH', 'KPP_CLANG') { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
  & "$kpp\kpp.exe" new | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "the packaged kpp new failed ($LASTEXITCODE)" }
  $output = (& "$kpp\kpp.exe" --no-server --run 2>&1 | ForEach-Object { "$_" }) -join "`n"
  if ($LASTEXITCODE -ne 0 -or $output -notmatch 'Hello from kpp') { throw "the packaged kpp did not build a new project:`n$output" }
  # A project build is --quiet (no toolchain line); its link names the target, which only the bundled
  # toolchain builds (Visual Studio's clang with windows-gnu is an error).
  $rsp = Get-ChildItem -Recurse "$smoke\build" -Filter *.link.rsp | Select-Object -First 1
  $link = if ($rsp) { Get-Content -Raw $rsp.FullName } else { '' }
  if ($link -notmatch '--target=x86_64-w64-windows-gnu' -or $link -notmatch '-fuse-ld=lld') { throw "the packaged kpp did not link with its bundled toolchain for windows-gnu:`n$link" }
  $exe = Get-ChildItem -Recurse "$smoke\build" -Filter *.exe | Select-Object -First 1
  if (-not $exe) { throw "the smoke project's build left no .exe" }
  $dlls = & "$llvmMingw\bin\llvm-objdump.exe" -p $exe.FullName | Where-Object { $_ -match 'DLL Name:\s*(\S+)' } | ForEach-Object { $Matches[1] }
  $foreign = @($dlls | Where-Object { $_ -notmatch '^(ucrtbase|api-ms-win-crt-[a-z0-9-]+|kernel32|user32|advapi32|shell32|ole32|ws2_32|ntdll)\.dll$' })
  if (-not $dlls -or $foreign.Count) { throw "the smoke program imports $($foreign -join ', '), which Windows does not have (all: $($dlls -join ', '))" }
  Write-Output "smoke test: built with the bundled toolchain for windows-gnu; the program imports $($dlls -join ', ')"
  # In the project's directory, as kovarex ran it: the version, never a build.
  $printed = (& "$kpp\kpp.exe" --version 2>&1 | ForEach-Object { "$_" }) -join "`n"
  if ($LASTEXITCODE -ne 0 -or $printed -notmatch "^kpp $([regex]::Escape($Version))" -or
      $printed -notmatch [regex]::Escape("toolchain: $kpp\bin\clang.exe (bundled") -or $printed -notmatch "(?m)^stdlib: $([regex]::Escape("$kpp\stdlib"))$") { throw "kpp --version printed:`n$printed" }
  Write-Output $printed
}
finally
{
  foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
  Pop-Location
}
Remove-Item -Recurse -Force $smoke
}
Lap 'smoke test'

# The VS Code extension's job (started at the beginning): only the wait is this step's.
if ($vsix)
{
  Wait-Job $vsix | Out-Null
  $log = Receive-Job $vsix -ErrorAction SilentlyContinue -ErrorVariable failure
  $state = $vsix.State
  Remove-Job $vsix
  if ($state -ne 'Completed' -or $failure -or -not (Test-Path "$kpp\kpp-vscode.vsix")) { throw "the VS Code extension failed:`n$($log -join "`n")`n$failure" }
  $log | Select-Object -Last 2 | Out-Host
}
Lap 'kpp-vscode.vsix (wait)'

# The zip and the installer one after the other: at once they took 43 to 49 s against 26 to 29 s
# (Inno Setup's 8 compression threads already fill the machine). .NET's ZipFile, as small as
# Compress-Archive's zip and ten times faster (10 against 104 s).
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($kpp, "$out\kpp-$Version.zip", [IO.Compression.CompressionLevel]::Optimal, $false)
Write-Output "wrote $out\kpp-$Version.zip"
Lap 'zip'

$iscc = @($IsccPath, "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { $_ } |
  Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc)
{
  & $iscc /Q "/DVersion=$Version" "/DSource=$kpp" "/O$out" "$PSScriptRoot\kpp.iss"
  if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
  Write-Output "wrote $out\kpp-$Version-setup.exe"
}
else { Write-Output 'no Inno Setup: no installer (the zip is complete)' }
Lap 'installer'
Write-Output ("times: " + (($times.GetEnumerator() | ForEach-Object { '{0} {1:N1} s' -f $_.Key, $_.Value }) -join ', '))
