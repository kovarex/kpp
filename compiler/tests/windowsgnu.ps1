# Windows' GNU dialect (--target windows-gnu, design.md 17): every tests\run\*.kpp built with the
# bundled toolchain's llvm-mingw (tools\sysroot\llvm-mingw.ps1, downloaded once into build\llvm-mingw)
# and run, its output compared with the .expected as for the default target. run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\windowsgnu'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$root = (& "$PSScriptRoot\..\..\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1)
$env:KPP_CLANG = "$root\bin\clang.exe"
$ErrorActionPreference = 'Continue' # (a failing build's stderr is reported below, not thrown)
$passed = 0
$failed = @()
Push-Location "$PSScriptRoot\.."
try
{
  foreach ($f in Get-ChildItem "$PSScriptRoot\run\*.kpp")
  {
    $n = $f.BaseName
    & $KppExe --quiet --target windows-gnu --exe "$work\$n.exe" $f.FullName > "$work\$n.compile.txt" 2>&1
    if ($LASTEXITCODE -ne 0) { $failed += "$n (compile: $work\$n.compile.txt)"; continue }
    $got = (& "$work\$n.exe" 2>&1 | Out-String).Trim() -replace "`r", ''
    $want = (Get-Content -Raw ($f.FullName -replace '\.kpp$', '.expected')).Trim() -replace "`r", ''
    if ($got -eq $want) { $passed++ } else { $failed += "$n (output differs)" }
  }
}
finally { Pop-Location }
# A --debug-info build's PDB holds the .natvis as the MSVC target's does (lld's /natvis through
# --Xlink), so Visual Studio shows the library's values in a windows-gnu program (docs/debugging.md).
& $KppExe --quiet --target windows-gnu --debug-info --exe "$work\natvis.exe" "$PSScriptRoot\natvis\values.kpp" > "$work\natvis.compile.txt" 2>&1
if ($LASTEXITCODE -ne 0) { $failed += "natvis (compile: $work\natvis.compile.txt)" }
else
{
  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
  $streams = (& (Join-Path $vs 'VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe') dump --named-streams "$work\natvis.pdb") -join "`n"
  if ($streams -notmatch '/src/files/\S*natvis\.natvis') { $failed += "natvis (the PDB has no /src/files/...natvis.natvis)" }
}
# A release build links through LTO, whose back end is lld's: the globals built in definition order
# (MinGW-w64's startup runs .ctors from its end), thin and whole-program.
$startup = "$PSScriptRoot\spec\language\startup_in_definition_order"
foreach ($lto in @(), @('--lto', 'full'))
{
  & $KppExe --quiet --target windows-gnu --release @lto --exe "$work\startup.exe" "$startup.kpp" > "$work\startup.compile.txt" 2>&1
  $out = (& "$work\startup.exe" | Out-String).Trim() -replace "`r", ''
  if ($out -ne (Get-Content -Raw "$startup.expected").Trim().Replace("`r", '')) { $failed += "startup order in a release build $lto" }
}
# A project with C libraries (tests/targets/clibrary: a C file, a C++ file, Windows' own library with
# <WinSock2.h> and <Windows.h>), built in GNU's dialect with --target=x86_64-w64-windows-gnu, its C++
# file's libc++ and libunwind linked from their archives (Project/CLibraryBuild.cpp: none were, and
# operator delete and __cxa_throw were undefined; KPP_CLANG naming llvm-mingw built clang-cl objects).
$project = Join-Path $work 'clibrary'
if (Test-Path $project) { cmd /c rd /s /q $project }
Copy-Item -Recurse "$PSScriptRoot\targets\clibrary" $project
Push-Location $project
try
{
  foreach ($configuration in 'debug', 'release')
  {
    & $KppExe $configuration --no-server --target windows-gnu > "$work\clibrary.$configuration.txt" 2>&1
    $out = if (Test-Path "build\$configuration\app.exe") { (& "build\$configuration\app.exe" | Out-String).Trim() } else { '' }
    if ($out -ne '42 windows 10') { $failed += "the C library project ($configuration): '$out' ($work\clibrary.$configuration.txt)" }
  }
}
finally { Pop-Location }
foreach ($f in $failed) { "FAIL windows-gnu $f" }
if (-not $failed.Count) { "ok   windows-gnu: $passed end-to-end programs, the .natvis in a --debug-info build's PDB, a project's C libraries" }
exit $failed.Count
