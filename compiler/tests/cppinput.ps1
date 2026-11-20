# C++ input the converter reads (kpp --convert), one case per file in tests\cppinput\: NAME.cpp is converted
# alone, with no parse error; its kpp is NAME.expected.kpp, and it builds and runs, returning 0. The C++
# forms are the ones code bases beside Factorio's use (Unreal Engine's, 2026-11-20), written here by hand.
# run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\cppinput'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$KppExe = (Resolve-Path $KppExe).Path
$failed = @()
$cases = @(Get-ChildItem "$PSScriptRoot\cppinput\*.cpp" | Sort-Object Name)
foreach ($case in $cases)
{
  $name = $case.BaseName
  $in = Join-Path $work "$name\in"
  New-Item -ItemType Directory -Force $in | Out-Null
  Copy-Item $case.FullName $in
  $out = Join-Path $work "$name\out"
  $ErrorActionPreference = 'Continue' # (kpp writes its errors to stderr)
  $log = & $KppExe --no-std --quiet --convert $out --ext .cpp $in 2>&1 | Out-String
  $ErrorActionPreference = 'Stop'
  if ($LASTEXITCODE -ne 0 -or $log -match 'error:') { $failed += "${name}: the conversion failed (exit $LASTEXITCODE): $($log.Trim())"; continue }
  $converted = Join-Path $out "in\$name.kpp"
  if (-not (Test-Path $converted)) { $failed += "${name}: no $name.kpp written"; continue }
  $expected = Join-Path $PSScriptRoot "cppinput\$name.expected.kpp"
  $got = (Get-Content -Raw $converted) -replace "`r`n", "`n"
  if (-not (Test-Path $expected)) { $failed += "${name}: no cppinput\$name.expected.kpp (the conversion wrote $converted)"; continue }
  if ($got -ne ((Get-Content -Raw $expected) -replace "`r`n", "`n")) { $failed += "${name}: the conversion differs from $name.expected.kpp (written to $converted)"; continue }
  $ErrorActionPreference = 'Continue'
  $run = & $KppExe --this-pointer --quiet --run $converted 2>&1 | Out-String
  $ErrorActionPreference = 'Stop'
  if ($LASTEXITCODE -ne 0) { $failed += "${name}: the converted program returned $LASTEXITCODE`: $($run.Trim())" }
}
if ($failed.Count) { $failed | ForEach-Object { "FAIL cppinput: $_" }; "cppinput: FAILED ($($failed.Count) of $($cases.Count))"; exit 1 }
"cppinput: ALL PASSED ($($cases.Count) cases)"
