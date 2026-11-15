# Coverage files (docs/design.md 28): two runs of one program's tests, each of one suite, merged by
# `kpp --coverage-merge`, are the run of both; `kpp --coverage-export lcov` says it as lcov (each
# function, each condition's two branches, each line); a file of another version of a source does
# not merge. run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\coverage'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$KppExe = (Resolve-Path $KppExe).Path
$failed = 0
function Fail([string]$what) { "FAIL coverage: $what"; $script:failed++ }
$program = @'
int sign(int x)
{
  if (x < 0)
    return -1;
  if (x > 0 && x != 7)
    return 1;
  return 0;
}

suite Negative
{
  test negative
  {
    check(sign(-4) == -1);
  }
}

suite Positive
{
  test positive
  {
    check(sign(4) == 1);
  }
}
'@
Set-Content -Encoding ascii "$work\sign.kpp" $program
Push-Location $work
$ErrorActionPreference = 'Continue' # (kpp's messages on stderr are what is checked)
try
{
  # (each its own build: the same program, so the same hash)
  foreach ($run in @(@('neg.json', '--suite', 'Negative'), @('pos.json', '--suite', 'Positive'), @('all.json')))
  {
    $out = & $KppExe --no-server --coverage --test sign.kpp -- --coverage $run[0] @($run | Select-Object -Skip 1) 2>&1
    if ($LASTEXITCODE -ne 0) { Fail "the run for $($run[0]) exited $LASTEXITCODE`: $out" }
  }
  $merged = & $KppExe --coverage-merge neg.json pos.json -o merged.json 2>&1
  if ($LASTEXITCODE -ne 0) { Fail "--coverage-merge exited $LASTEXITCODE`: $merged" }
  $expected = Get-Content all.json -Raw
  $got = Get-Content merged.json -Raw
  if ($got -ne $expected) { Fail "the merge of the two suites' runs is not the run of both:`n$got`nexpected`n$expected" }
  if ("$merged" -notmatch '^coverage: lines 80\.0% \(4 of 5\), conditions 62\.5% \(5 of 8\), functions 100\.0% \(1 of 1\): merged\.json$')
  { Fail "the merge's summary: $merged" }
  $lcov = (& $KppExe --coverage-export lcov merged.json -o - 2>&1) -join "`n"
  foreach ($line in @('SF:sign.kpp', 'FN:1,sign', 'FNDA:1,sign', 'BRDA:3,0,0,1', 'BRDA:3,0,1,1', 'BRDA:5,2,1,0', 'DA:7,0', 'LF:5', 'LH:4', 'end_of_record'))
  {
    if ($lcov -notmatch "(?m)^$([regex]::Escape($line))$") { Fail "lcov has no line $line`:`n$lcov" }
  }
  # Another version of the source: its coverage does not merge with the first's.
  Add-Content -Encoding ascii sign.kpp '// changed'
  $out = & $KppExe --no-server --coverage --test sign.kpp -- --coverage changed.json 2>&1
  $refused = & $KppExe --coverage-merge neg.json changed.json -o - 2>&1
  if ($LASTEXITCODE -ne 1 -or "$refused" -notmatch 'is another version of it') { Fail "two versions of sign.kpp merged (exit $LASTEXITCODE): $refused" }
}
finally
{
  Pop-Location
}
if ($failed -eq 0) { 'ok   coverage: two suites'' runs merged are the run of both, lcov, two versions of a source refused' }
exit $failed
