# The test runner's interactive mode (-i, docs/testing.md "Interactive mode"): tests\interactive\'s
# tests built with --tests, the menu's keys given on the standard input (a pipe: the runner then reads
# bytes, where on a console it reads one key at a time). Checked: a (in parallel: worker processes), A, f (the failed one again), T with a
# search typed and Enter (a test and the suites it depends on), s a suite, space the last choice, a
# second run's space taking the first run's last choice from <executable>.interactive, an unknown key,
# a program's own key (testsInteractiveKey, the game's h), and the result in colour (CLICOLOR_FORCE: a pipe's is
# plain). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\interactive'))
if (Test-Path $work) { cmd /c "rd /s /q `"$work`"" | Out-Null }
New-Item -ItemType Directory -Force $work | Out-Null
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0
$env:NO_COLOR = $null
$env:CLICOLOR_FORCE = $null
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
if (@($lines -match '^FAILURE: 1 out of 4 tests failed \(\d+\.\d{3} s\)$').Count -ne 2) { Fail "not two summary lines 'FAILURE: 1 out of 4 tests failed (S s)', a's and A's" }
if ($lines -match ([char]27)) { Fail "a pipe's output has colour codes" }
$kept = Join-Path $work 'tests.interactive'
if (-not (Test-Path $kept) -or ([IO.File]::ReadAllText($kept) -notmatch '(?m)^last suite 0 Basics\r?$')) { Fail "tests.interactive does not keep the last choice: $(Get-Content $kept -ErrorAction SilentlyContinue)" }

$lines = Keys " xhq"
$ran = Ran $lines
if (($ran -join '|') -ne 'running 2 tests: suite Basics') { Fail "a second run's space ran: $($ran -join '; ')" }
if (-not ($lines -match '^no such key$')) { Fail "x did not say: no such key" }
if (-not ($lines -match '^heavy mode on$') -or -not ($lines -match 'h heavy mode \(on\)')) { Fail "the program's key h was not its: $($lines -join ' / ')" }

# The result in colour, as Factorio's C++ runner printed it: a green Success, a red FAILURE and failed.
$env:CLICOLOR_FORCE = '1'
$lines = Keys "toutside`rsBas`rq"
$env:CLICOLOR_FORCE = $null
$e = [char]27
if (-not ($lines -match "^$e\[32mSuccess$e\[0m: 1 test passed ")) { Fail "no green Success: $(($lines -join ' / ') -replace $e, '<esc>')" }
if (-not ($lines -match "^$e\[31mFAILURE$e\[0m: 1 out of 2 tests failed ")) { Fail "no red FAILURE: $(($lines -join ' / ') -replace $e, '<esc>')" }
if (-not ($lines -match "^$e\[31mfailed$e\[0m: Basics\.fails$")) { Fail "no red failed: $(($lines -join ' / ') -replace $e, '<esc>')" }
$env:NO_COLOR = '1'
$env:CLICOLOR_FORCE = '1'
$lines = Keys "toutside`rq"
$env:NO_COLOR = $null
$env:CLICOLOR_FORCE = $null
if (-not ($lines -match '^Success: 1 test passed ')) { Fail "NO_COLOR did not take the colours away: $(($lines -join ' / ') -replace $e, '<esc>')" }

# Workers that end in their testsSetup (the game's without --write-data, 2026-11-16): a's tests did not
# run, and they failed (the summary said 0 failed), f runs them again; with testsWorkerArguments' argument
# (g) the workers run them.
$exe = Join-Path $work 'workers.exe'
$ErrorActionPreference = 'Continue'
$built = & $Kppc --quiet --tests --exe $exe "$PSScriptRoot\interactive\tests.kpp" "$PSScriptRoot\interactive\workers.kpp" 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $exe)) { Fail "the workers' tests did not build: $($built -join "`n")" }
else
{
  $lines = Keys "agfq"
  $summaries = @($lines | Where-Object { $_ -match '^(Success|FAILURE): ' })
  if ($summaries.Count -ne 2 -or $summaries[0] -notmatch '^FAILURE: 4 out of 4 tests failed, 4 of them not run \(\d+\.\d{3} s\)$' -or $summaries[1] -notmatch '^FAILURE: 1 out of 4 tests failed \(\d+\.\d{3} s\)$')
  { Fail "a without the workers' argument, then f with it, said:`n$($summaries -join "`n")`nnot 'FAILURE: 4 out of 4 tests failed, 4 of them not run' and 'FAILURE: 1 out of 4 tests failed'" }
  if (-not ($lines -match '^worker \d+ without --worker-ok$')) { Fail "no worker said: without --worker-ok" }
  if (-not ($lines -match '^running 4 tests: the failed tests \(4\)$')) { Fail "f did not run the 4 tests not run" }
}

if ($failed -eq 0) { Write-Output "ok   interactive: a (in parallel), A, f, T with a search, s, space, the next run's space, an unknown key, the program's key, tests a's workers did not run fail, the result in colour" }
cmd /c "rd /s /q `"$work`"" | Out-Null
exit $failed
