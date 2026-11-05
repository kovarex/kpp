# The quick gate a thread runs before pushing (kovarex, 2026-11-04: "it is more important to keep going
# forward fast"): run_tests (its quick run) and the spec side by side, each a process of its own, their output
# in build\gate\run_tests.log and build\gate\spec.log. Prints run_tests' output, the spec's FAIL lines,
# both summary lines, and last its own, with the exit code that agrees with it:
#   gate: ALL PASSED (quick: run_tests ALL PASSED, spec ALL PASSED; 190 s)      exit 0
#   gate: FAILED (run_tests: 1 FAILED: lto (...); spec ALL PASSED; 201 s)       exit 1
# What the quick run leaves out, CI runs on every push to master, and thread H runs the converted game's
# tests on master in batches. -Full: run_tests --full (the whole of it) beside the spec.
# gate.cmd runs it.
param([switch]$Full)
$ErrorActionPreference = 'Stop'
$compiler = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location $compiler
$out = Join-Path $compiler 'build\gate'
New-Item -ItemType Directory -Force $out | Out-Null
$started = Get-Date
# A kppc older than its sources tests something else than the change: C's gate went red on one
# (2026-11-05). The gate does not build it (a thread may have put a kppc variant there on purpose):
# it stops and says so.
$kppc = Join-Path $compiler 'build\kppc.exe'
if (-not (Test-Path $kppc)) { "gate: FAILED (no $kppc`: run compiler\build.cmd)"; exit 1 }
$newest = Get-ChildItem -Recurse -File (Join-Path $compiler 'src'), (Join-Path $compiler 'CMakeLists.txt') | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newest.LastWriteTime -gt (Get-Item $kppc).LastWriteTime)
{
  "gate: FAILED (build\kppc.exe, $((Get-Item $kppc).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')), is older than $($newest.FullName.Substring($compiler.Length + 1)), $($newest.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')): run compiler\build.cmd first)"
  exit 1
}
function Start-Logged([string]$command, [string]$log)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $env:ComSpec
  $info.Arguments = "/d /s /c `"$command > `"$log`" 2>&1`""
  $info.WorkingDirectory = $compiler
  $info.UseShellExecute = $false
  return [System.Diagnostics.Process]::Start($info)
}
$rtLog = Join-Path $out 'run_tests.log'
$specLog = Join-Path $out 'spec.log'
$fullArg = if ($Full) { ' --full' } else { '' }
$spec = Start-Logged 'tests\spec.cmd' $specLog
$rt = Start-Logged "tests\run_tests.cmd$fullArg" $rtLog
$rt.WaitForExit()
Get-Content $rtLog
$spec.WaitForExit()
$specLines = @(Get-Content $specLog)
$specLines | Where-Object { $_ -match '^(FAIL|FAILED|error)' }
$rtLast = @(Get-Content $rtLog | Where-Object { $_ })[-1]
$specLast = @($specLines | Where-Object { $_ })[-1]
$specLast
$seconds = [int]((Get-Date) - $started).TotalSeconds
$passed = $rt.ExitCode -eq 0 -and $spec.ExitCode -eq 0 -and $rtLast -match '^run_tests: ALL PASSED' -and $specLast -match '^spec: ALL PASSED'
$kind = if ($Full) { 'full' } else { 'quick' }
if ($passed) { "gate: ALL PASSED ($kind`: $($rtLast -replace '^run_tests: ', 'run_tests ') ; $($specLast -replace '^spec: ', 'spec ') ; $seconds s)"; exit 0 }
"gate: FAILED ($kind`: $rtLast, exit $($rt.ExitCode); $specLast, exit $($spec.ExitCode); $seconds s)"
exit 1
