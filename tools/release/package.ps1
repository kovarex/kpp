# Packages a built kppc as a Kpp release (docs/build.md, "Installing"): build\release\kpp\ with
# kpp.exe (compiler\build\kppc.exe under the name users type), LICENSE.txt, stdlib\, projectlib\, the TextMate
# grammar, the VS Code extension kpp.vsix (tools/vscode) and the bundled toolchain (design.md 17:
# llvm-mingw's clang, lld and MinGW-w64, so a program builds with nothing of Visual Studio's); then
# build\release\kpp-<Version>.zip, and kpp-<Version>-setup.exe when Inno Setup's compiler (ISCC) is
# found. The CI workflows (ci.yml, release.yml) run it; it runs the same by hand:
#   powershell -File tools\release\package.ps1 -Version 0.0.0
# The toolchain comes from tools\sysroot\llvm-mingw.ps1 (pinned, SHA-256 checked, unpacked once into
# build\llvm-mingw). -IsccPath names Inno Setup's compiler when it is not installed in a usual place.
param([Parameter(Mandatory)][string]$Version, [string]$Commit = '', [string]$IsccPath = '')
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..\..").Path
$out = Join-Path $repository 'build\release'
$kpp = Join-Path $out 'kpp'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $kpp | Out-Null

Copy-Item "$repository\compiler\build\kppc.exe" "$kpp\kpp.exe"
# Apache 2.0's section 4(a): every copy carries the license (LICENSE, with its third-party section).
Copy-Item "$repository\LICENSE" "$kpp\LICENSE.txt"

# The bundled toolchain, at its paths in llvm-mingw so clang finds the rest relative to its bin\ and
# kpp finds bin\ next to itself: clang (which also compiles a project's C and C++ sources), lld,
# llvm-ar, llvm-rc, llvm-dlltool and their DLLs; MinGW-w64's and libc++'s headers; clang's headers
# and builtins; the x86_64 import libraries and runtime. Not the i686, armv7 and aarch64 trees,
# python, lldb, clangd or clang-tidy.
$llvmMingw = & "$repository\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1
New-Item -ItemType Directory -Force "$kpp\bin", "$kpp\lib\clang\19\lib\windows", "$kpp\x86_64-w64-mingw32\share" | Out-Null
foreach ($file in 'clang.exe', 'clang-19.exe', 'ld.lld.exe', 'llvm-ar.exe', 'llvm-rc.exe', 'llvm-dlltool.exe', 'libLLVM-19.dll',
  'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll', 'x86_64-w64-windows-gnu.cfg', 'mingw32-common.cfg')
{
  Copy-Item "$llvmMingw\bin\$file" "$kpp\bin"
}
Copy-Item -Recurse "$llvmMingw\include" "$kpp\include"
Copy-Item -Recurse "$llvmMingw\lib\clang\19\include" "$kpp\lib\clang\19\include"
Copy-Item "$llvmMingw\lib\clang\19\lib\windows\libclang_rt.builtins-x86_64.a" "$kpp\lib\clang\19\lib\windows"
Copy-Item -Recurse "$llvmMingw\x86_64-w64-mingw32\lib" "$kpp\x86_64-w64-mingw32\lib"
Copy-Item -Recurse "$llvmMingw\x86_64-w64-mingw32\share\mingw32" "$kpp\x86_64-w64-mingw32\share\mingw32"
Copy-Item "$llvmMingw\LICENSE.TXT" "$kpp\LICENSE-llvm-mingw.txt"
@"
The bundled toolchain (bin\, include\, lib\, x86_64-w64-mingw32\) is llvm-mingw 20250114
(https://github.com/mstorsjo/llvm-mingw), unchanged. Its parts keep their own licences:
- LLVM (clang, lld, the LLVM tools, libc++, libunwind, compiler-rt): Apache-2.0 WITH LLVM-exception,
  as Kpp; LICENSE-llvm-mingw.txt.
- MinGW-w64's headers and runtime: public domain or ZPL 2.1; the DirectX headers taken from Wine are
  LGPL-2.1 or later. x86_64-w64-mingw32\share\mingw32\COPYING*.
- llvm-mingw's own scripts: ISC.
"@ | Set-Content "$kpp\NOTICE-toolchain.txt"

Copy-Item -Recurse "$repository\stdlib" "$kpp\stdlib"
Copy-Item -Recurse "$repository\projectlib" "$kpp\projectlib"
New-Item -ItemType Directory -Force "$kpp\grammar" | Out-Null
Copy-Item "$repository\tools\vs\kpp\Syntaxes\kpp.tmLanguage.json" "$kpp\grammar"
@"
Kpp $Version$(if ($Commit) { " ($Commit)" })

kpp.exe is the compiler. In a project's directory (one with build.kpp), run kpp for a menu, or
kpp (every target, debug), kpp release, kpp TARGET CONFIGURATION, kpp TARGET --run,
kpp --generate-visual-studio-files. kpp new makes a project. Documentation: https://github.com/kovarex/kpp/blob/master/docs/build.md

Nothing else is needed: kpp brings its own toolchain (bin\: llvm-mingw's clang and lld with
MinGW-w64, NOTICE-toolchain.txt), and its programs use only DLLs every Windows 10 and 11 has.
kpp --toolchain msvc builds with an installed Visual Studio's clang instead, on request.

Kpp is licensed under the Apache License 2.0 with LLVM Exceptions: LICENSE.txt.

kpp.vsix is the VS Code extension (colouring, and IntelliSense through kpp --lsp): the installer
offers to install it, or run code --install-extension kpp.vsix. Other editors:
https://github.com/kovarex/kpp/blob/master/docs/editors.md

grammar\kpp.tmLanguage.json colours .kpp files in Visual Studio: copy it into
%USERPROFILE%\.vs\Extensions\kpp\Syntaxes\ and restart Visual Studio.
"@ | Set-Content "$kpp\README.txt"

# The VS Code extension (tools/vscode), versioned as the release, with its dependencies from
# package-lock.json. Needs Node.js (npm), as the CI runner has.
Push-Location "$repository\tools\vscode"
try
{
  $ErrorActionPreference = 'Continue'
  npm ci --no-audit --no-fund 2>&1 | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "npm ci failed ($LASTEXITCODE)" }
  # vsce puts the extension folder's LICENSE into the .vsix: the repository's, copied (.gitignore'd).
  Copy-Item "$repository\LICENSE" LICENSE
  npx vsce package $Version --no-git-tag-version --no-update-package-json --out "$kpp\kpp.vsix" 2>&1 | Select-Object -Last 2 | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "vsce failed ($LASTEXITCODE)" }
  $ErrorActionPreference = 'Stop'
}
finally { Pop-Location }

# A smoke test of the package itself: kpp new and kpp --run in a project outside the repository, as on
# a machine without Visual Studio: PATH cut to Windows' own directories, no INCLUDE, LIB or KPP_CLANG
# (CI sets it for the tests). It must build with the bundled toolchain for windows-gnu, and the
# program must import only Windows' own DLLs (llvm-objdump from the unpacked llvm-mingw, not shipped).
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
  $output = (& "$kpp\kpp.exe" --no-server --run 2>&1 | ForEach-Object { "$_" }) -join "`n"
  if ($LASTEXITCODE -ne 0 -or $output -notmatch 'Hello from Kpp') { throw "the packaged kpp did not build a new project:`n$output" }
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
}
finally
{
  foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
  Pop-Location
}
Remove-Item -Recurse -Force $smoke

Compress-Archive -Path "$kpp\*" -DestinationPath "$out\kpp-$Version.zip"
Write-Output "wrote $out\kpp-$Version.zip"

$iscc = @($IsccPath, "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { $_ } |
  Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc)
{
  & $iscc /Q "/DVersion=$Version" "/DSource=$kpp" "/O$out" "$PSScriptRoot\kpp.iss"
  if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
  Write-Output "wrote $out\kpp-$Version-setup.exe"
}
else { Write-Output 'no Inno Setup: no installer (the zip is complete)' }
