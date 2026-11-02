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
foreach ($f in $failed) { "FAIL windows-gnu $f" }
if (-not $failed.Count) { "ok   windows-gnu: $passed end-to-end programs" }
exit $failed.Count
