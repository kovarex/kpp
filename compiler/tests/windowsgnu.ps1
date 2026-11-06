# Windows' GNU dialect (--target windows-gnu, design.md 17): every tests\run\*.kpp built with the
# bundled toolchain's llvm-mingw (tools\sysroot\llvm-mingw.ps1, downloaded once into build\llvm-mingw)
# and run, its output compared with the .expected as for the default target. run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
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
    & $Kppc --quiet --target windows-gnu --exe "$work\$n.exe" $f.FullName > "$work\$n.compile.txt" 2>&1
    if ($LASTEXITCODE -ne 0) { $failed += "$n (compile: $work\$n.compile.txt)"; continue }
    $got = (& "$work\$n.exe" 2>&1 | Out-String).Trim() -replace "`r", ''
    $want = (Get-Content -Raw ($f.FullName -replace '\.kpp$', '.expected')).Trim() -replace "`r", ''
    if ($got -eq $want) { $passed++ } else { $failed += "$n (output differs)" }
  }
}
finally { Pop-Location }
# A --debug-info build's PDB holds the .natvis as the MSVC target's does (lld's /natvis through
# --Xlink), so Visual Studio shows the library's values in a windows-gnu program (docs/debugging.md).
& $Kppc --quiet --target windows-gnu --debug-info --exe "$work\natvis.exe" "$PSScriptRoot\natvis\values.kpp" > "$work\natvis.compile.txt" 2>&1
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
  & $Kppc --quiet --target windows-gnu --release @lto --exe "$work\startup.exe" "$startup.kpp" > "$work\startup.compile.txt" 2>&1
  $out = (& "$work\startup.exe" | Out-String).Trim() -replace "`r", ''
  if ($out -ne (Get-Content -Raw "$startup.expected").Trim().Replace("`r", '')) { $failed += "startup order in a release build $lto" }
}
foreach ($f in $failed) { "FAIL windows-gnu $f" }
if (-not $failed.Count) { "ok   windows-gnu: $passed end-to-end programs, and the .natvis in a --debug-info build's PDB" }
exit $failed.Count
