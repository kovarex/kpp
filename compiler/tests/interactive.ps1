# The test runner's interactive mode (-i, docs/testing.md "Interactive mode"): tests\interactive\'s
# tests built with --tests, the menu's keys given on the standard input (a pipe: the runner then reads
# bytes, where on a console it reads one key at a time). Checked: a (in parallel: worker processes), A, f (the failed one again), T with a
# search typed and Enter (a test and the suites it depends on), s a suite, space the last choice, a
# second run's space taking the first run's last choice from <executable>.interactive, an unknown key,
# and a program's own key (testsInteractiveKey, the game's h). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\interactive'))
if (Test-Path $work) { cmd /c "rd /s /q `"$work`"" | Out-Null }
New-Item -ItemType Directory -Force $work | Out-Null
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0
function Fail([string]$what) { Write-Output "FAIL interactive: $what"; $script:failed++ }
$exe = Join-Path $work 'tests.exe'
$ErrorActionPreference = 'Continue'
$built = & $Kppc --quiet --tests --exe $exe "$PSScriptRoot\interactive\tests.kpp" "$PSScriptRoot\interactive\hook.kpp" 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $exe)) { Fail "the tests did not build: $($built -join "`n")"; exit 1 }

# The keys through cmd's pipe from a file (bytes as written: Enter as \r); the output as lines.
function Keys([string]$keys)
{
  $in = Join-Path $work 'keys.txt'
  [IO.File]::WriteAllText($in, $keys)
  $out = Join-Path $work 'out.txt'
  cmd /c "`"$exe`" -i < `"$in`" > `"$out`" 2>&1"
  return @([IO.File]::ReadAllText($out) -split "`r?`n|`r")
}
function Ran([string[]]$lines) { return @($lines | Where-Object { $_ -match '^running ' } | ForEach-Object { $_ }) }

$lines = Keys "aAfTuses`rsbas`r q"
$ran = Ran $lines
$expected = @('running 4 tests: all the tests, in parallel', 'running 4 tests: all the tests, in this process', 'running 1 test: the failed tests (1)',
              'running 3 tests: test Later.uses, with the suites it depends on', 'running 2 tests: suite Basics', 'running 2 tests: suite Basics')
if (($ran -join '|') -ne ($expected -join '|')) { Fail "the runs were:`n$($ran -join "`n")`nnot:`n$($expected -join "`n")" }
if (-not ($lines -match '^failed: Basics\.fails$')) { Fail "no failed: Basics.fails line" }
if (@($lines -match '^4 tests, 1 failed \(\d+\.\d{3} s\)$').Count -ne 2) { Fail "not two summary lines '4 tests, 1 failed (S s)', a's and A's" }
$kept = Join-Path $work 'tests.interactive'
if (-not (Test-Path $kept) -or ([IO.File]::ReadAllText($kept) -notmatch '(?m)^last suite 0 Basics\r?$')) { Fail "tests.interactive does not keep the last choice: $(Get-Content $kept -ErrorAction SilentlyContinue)" }

$lines = Keys " xhq"
$ran = Ran $lines
if (($ran -join '|') -ne 'running 2 tests: suite Basics') { Fail "a second run's space ran: $($ran -join '; ')" }
if (-not ($lines -match '^no such key$')) { Fail "x did not say: no such key" }
if (-not ($lines -match '^heavy mode on$') -or -not ($lines -match 'h heavy mode \(on\)')) { Fail "the program's key h was not its: $($lines -join ' / ')" }

if ($failed -eq 0) { Write-Output "ok   interactive: a (in parallel), A, f, T with a search, s, space, the next run's space, an unknown key, the program's key" }
cmd /c "rd /s /q `"$work`"" | Out-Null
exit $failed
