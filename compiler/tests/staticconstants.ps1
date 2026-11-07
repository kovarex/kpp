# Static locals with constant initialization (C++'s [basic.start.static]): a const or constexpr static
# local whose initializer folds is a constant of the program in the IR, with no guard, as
# spec language/static_local_constant_initialization's are:
# - a double, an object a constexpr constructor builds, an array, each template instance's own (a function
#   template's, a class template's member function's), an inline function's: `internal constant`;
# - a const object with a mutable member: `internal global`, initialized;
# - one whose initializer calls a function or reads a parameter keeps its guard (it runs on the first pass).
# The spec checks what the program prints; this checks that nothing is left to the pass. run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\staticconstants'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$KppExe = (Resolve-Path $KppExe).Path
$source = (Resolve-Path "$PSScriptRoot\spec\language\static_local_constant_initialization.kpp").Path
$failed = @()

& $KppExe --quiet --emit-llvm "$work\statics.ll" --exe "$work\statics.exe" $source 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$work\statics.ll")) { "FAIL static constants: the snippet does not build (exit $LASTEXITCODE)"; exit 1 }
$ir = Get-Content -Raw "$work\statics.ll"
$output = (& "$work\statics.exe" | Out-String).Trim()
if ($output -ne '10 1677721 31 12 20 16 2 0.5 true 3 2 1 10') { $failed += "the program printed '$output'" }

foreach ($case in @(
    @('scaled\(double\)::factor', '= internal constant double 0x3FE999999999999A'),
    @('minimum\(\)::least', '= internal constant %struct\.[0-9a-f]+ \{ i64 1677721 \}'),
    @('sumTable\(\)::table', '= internal constant \[5 x i32\] \[i32 1, i32 2, i32 4, i32 8, i32 16\]'),
    @('timesFour<3>\(\)::product', '= internal constant i32 12'),
    @('timesFour<5>\(\)::product', '= internal constant i32 20'),
    @('Box<int64>::scale\(\)::factor', '= internal constant i32 16'),
    @('Box<char>::scale\(\)::factor', '= internal constant i32 2'),
    @('rate\(\)::value', '= internal constant double 0x3FD0000000000000'),
    @('counted\(\)::counter', '= internal global %struct\.[0-9a-f]+ \{ i32 1, i32 0 \}')))
{
  if ($ir -notmatch ('(?m)^@"' + $case[0] + '\.\d+" ' + $case[1] + '$')) { $failed += "$($case[0] -replace '\\', ''): not a constant `"$($case[1] -replace '\\', '')`"" }
  if ($ir -match ('(?m)^@"' + $case[0] + '\.\d+\.guard"')) { $failed += "$($case[0] -replace '\\', ''): still has a guard" }
}
if ($ir -notmatch '(?m)^@"once\(\)::first\.\d+\.guard" = internal global i8 0$') { $failed += 'once()::first: its initializer calls a function, and it has no guard' }
if ($ir -notmatch '(?m)^@"firstArgument\(int32\)::remembered\.\d+\.guard" = internal global i8 0$') { $failed += 'firstArgument(int32)::remembered: its initializer reads a parameter, and it has no guard' }

if ($failed.Count) { $failed | ForEach-Object { "FAIL static constants: $_" }; exit 1 }
"ok   static constants: a const or constexpr static local that folds is a constant (no guard); a call's or a parameter's keeps its guard"
exit 0
