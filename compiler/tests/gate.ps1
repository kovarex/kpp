# The quick gate a thread runs before pushing (kovarex, 2026-11-04: "it is more important to keep going
# forward fast"): run_tests (its quick run) and the spec side by side, each a process of its own, their output
# in build\gate\run_tests.log and build\gate\spec.log. Prints run_tests' output, the spec's FAIL lines,
# both summary lines, and last its own, with the exit code that agrees with it:
#   gate: ALL PASSED (quick: run_tests ALL PASSED, spec ALL PASSED; 190 s)      exit 0
#   gate: FAILED (run_tests: 1 FAILED: lto (...); spec ALL PASSED; 201 s)       exit 1
# What the quick run leaves out, CI runs on every push to master, and thread H runs the converted game's
# tests on master in batches. -Full: run_tests --full (the whole of it) beside the spec.
# gate.cmd runs it.
# -WslExe: what the Linux check's first call of wsl.exe runs (a stub that hangs tests its skip).
param([switch]$Full, [string]$WslExe = 'wsl.exe')
$ErrorActionPreference = 'Stop'
$compiler = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location $compiler
$out = Join-Path $compiler 'build\gate'
New-Item -ItemType Directory -Force $out | Out-Null
$started = Get-Date
# A kpp older than its sources tests something else than the change: C's gate went red on one
# (2026-11-05). The gate does not build it (a thread may have put a kpp variant there on purpose):
# it stops and says so.
$kpp = Join-Path $compiler 'build\kpp.exe'
if (-not (Test-Path $kpp)) { "gate: FAILED (no $kpp`: run compiler\build.cmd)"; exit 1 }
$newest = Get-ChildItem -Recurse -File (Join-Path $compiler 'src'), (Join-Path $compiler 'CMakeLists.txt') | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newest.LastWriteTime -gt (Get-Item $kpp).LastWriteTime)
{
  "gate: FAILED (build\kpp.exe, $((Get-Item $kpp).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')), is older than $($newest.FullName.Substring($compiler.Length + 1)), $($newest.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')): run compiler\build.cmd first)"
  exit 1
}
# Each run's log is deleted before it starts: a run that never started (a command line over cmd's 8191
# characters) left the previous gate's log, read as this one's (D's gate, 2026-11-08).
function Start-Logged([string]$command, [string]$log)
{
  Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $log) { throw "gate: cannot delete the previous $log" }
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $env:ComSpec
  $info.Arguments = "/d /s /c `"$command > `"$log`" 2>&1`""
  $info.WorkingDirectory = $compiler
  $info.UseShellExecute = $false
  return [System.Diagnostics.Process]::Start($info)
}
# A list of snippets for run_spec.py's --only, in a file (--only @FILE): a change that adds or edits 150
# snippets makes a list longer than cmd's line.
function Write-OnlyList([string]$name, [string[]]$snippets)
{
  $path = Join-Path $out $name
  [IO.File]::WriteAllLines($path, $snippets)
  return "--only `"@$path`""
}
# A spec run's summary: its last line, which says `spec: ...`. A run that wrote none (no log, or a log
# without it: it did not start, or died) fails, naming the run and its first lines.
function Get-SpecSummary([System.Diagnostics.Process]$run, [string]$log, [string]$label)
{
  $lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Where-Object { $_ })
  if ($lines.Count -and $lines[-1] -match '^spec: ')
  {
    return [pscustomobject]@{ Ok = $run.ExitCode -eq 0 -and $lines[-1] -match 'ALL PASSED'; Last = $lines[-1] -replace '^spec: ', "$label`: "; Lines = $lines }
  }
  $what = if (-not (Test-Path -LiteralPath $log)) { 'no log' } elseif ($lines.Count) { "its log: $(($lines | Select-Object -First 3) -join ' / ')" } else { 'an empty log' }
  return [pscustomobject]@{ Ok = $false; Last = "$label`: FAILED, the run did not start or ended without its summary (exit $($run.ExitCode), $what)"; Lines = $lines }
}
$rtLog = Join-Path $out 'run_tests.log'
$specLog = Join-Path $out 'spec.log'
$fullArg = if ($Full) { ' --full' } else { '' }
$spec = Start-Logged 'tests\spec.cmd' $specLog
$rt = Start-Logged "tests\run_tests.cmd$fullArg" $rtLog
# The snippets this change adds or edits (against origin/master, committed or not), built for FreeBSD
# too (run_spec.py --target freebsd --only: built, not run): a snippet that builds for Windows alone
# turned master's CI red, which builds every snippet for FreeBSD (2026-11-06, a bit-field compare on
# the SysV layout). FreeBSD's sysroot is made once per worktree (tools\sysroot\freebsd.ps1, 210 MB).
$ErrorActionPreference = 'Continue'
$changed = @(& git diff --name-only --diff-filter=AM origin/master -- 'tests/spec/*.kpp' 2>$null) + @(& git ls-files --others --exclude-standard -- 'tests/spec/*.kpp' 2>$null)
$ErrorActionPreference = 'Stop'
$changed = @($changed | Where-Object { $_ } | ForEach-Object { ($_ -replace '^compiler/', '') -replace '^tests/spec/', '' } | Sort-Object -Unique)
$bsd = $null
$bsdLog = Join-Path $out 'spec-freebsd.log'
if ($changed.Count)
{
  $sysroot = Join-Path $compiler 'build\sysroots\freebsd-x86_64'
  if (-not (Test-Path (Join-Path $sysroot 'usr\lib\libc.a')))
  {
    $made = Start-Logged "powershell -NoProfile -ExecutionPolicy Bypass -File ..\tools\sysroot\freebsd.ps1" (Join-Path $out 'freebsd-sysroot.log')
    $made.WaitForExit()
  }
  $bsd = Start-Logged "python -X utf8 -I tests\spec\run_spec.py --target freebsd --no-doc $(Write-OnlyList 'only-freebsd.txt' $changed)" $bsdLog
}
# And with --debug-info: the whole spec when the change touches the code that writes debug information
# (a file or a line naming it: DebugInfo, DIBuilder, CodeView, DW_), else the snippets it adds or edits.
# 7e7749c3 (2026-11-06) declared member functions in the debug information and every enumeration with
# one failed to build with --debug-info; only CI's --debug-info shards saw it.
$ErrorActionPreference = 'Continue'
$sourceDiff = (@(& git diff origin/master -- 'src' 2>$null) | Where-Object { $_ -match '^[-+]' -and $_ -notmatch '^(---|\+\+\+) ' }) -join "`n"
$sourceFiles = @(& git diff --name-only origin/master -- 'src' 2>$null)
$ErrorActionPreference = 'Stop'
$touchesDebugInfo = ($sourceFiles -match 'DebugInfo').Count -gt 0 -or $sourceDiff -match 'DebugInfo|debugInfo|DIBuilder|CodeView|DW_'
$dbg = $null
$dbgLog = Join-Path $out 'spec-debug-info.log'
$dbgWhat = if ($touchesDebugInfo) { 'the whole spec, the change touching debug information' } elseif ($changed.Count) { "$($changed.Count) snippets" } else { $null }
if ($touchesDebugInfo) { $dbg = Start-Logged "python -X utf8 -I tests\spec\run_spec.py --debug-info --no-doc" $dbgLog }
elseif ($changed.Count) { $dbg = Start-Logged "python -X utf8 -I tests\spec\run_spec.py --debug-info --no-doc $(Write-OnlyList 'only-debug-info.txt' $changed)" $dbgLog }
# And the release snippets (*_release.kpp, a few seconds) built for Linux and run under WSL: 0ef4abd1
# (2026-11-06) made every throw crash in a Linux or FreeBSD release build, its ThinLTO dropping what
# the C runtime's personality reaches, and only CI's linux_host and freebsd_run saw it. A WSL that does
# not answer, or a program that times out under it, skips this with a notice: CI still runs it.
$lnx = $null
$lnxLog = Join-Path $out 'spec-linux.log'
$lnxLast = 'spec linux release: skipped, no WSL'
$lnxOk = $true
if (Get-Command $WslExe -ErrorAction SilentlyContinue)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = (Get-Command $WslExe).Source
  $info.Arguments = '-e true'
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $probe = [System.Diagnostics.Process]::Start($info)
  if (-not $probe.WaitForExit(120000))
  {
    $probe.Kill()
    $lnxLast = 'spec linux release: skipped, WSL did not answer in 2 minutes'
  }
  elseif ($probe.ExitCode -eq 0)
  {
    if (-not $env:KPP_SYSROOT_LINUX)
    {
      $made = Start-Logged "powershell -NoProfile -ExecutionPolicy Bypass -File ..\tools\sysroot\linux.ps1" (Join-Path $out 'linux-sysroot.log')
      $made.WaitForExit()
    }
    $releaseSnippets = @(Get-ChildItem -Recurse -File (Join-Path $compiler 'tests\spec') -Filter '*_release.kpp' | ForEach-Object { $_.FullName.Substring((Join-Path $compiler 'tests\spec\').Length) -replace '\\', '/' })
    $lnx = Start-Logged "python -X utf8 -I tests\spec\run_spec.py --target linux --no-doc $(Write-OnlyList 'only-release.txt' $releaseSnippets)" $lnxLog
  }
}
$rt.WaitForExit()
$rtLines = @(Get-Content -LiteralPath $rtLog -ErrorAction SilentlyContinue)
$rtLines
$spec.WaitForExit()
$specLines = @(Get-Content -LiteralPath $specLog -ErrorAction SilentlyContinue)
$specLines | Where-Object { $_ -match '^(FAIL|FAILED|error)' }
$rtLast = if (@($rtLines | Where-Object { $_ }).Count) { @($rtLines | Where-Object { $_ })[-1] } else { 'run_tests: FAILED, it did not start (no log)' }
$specLast = if (@($specLines | Where-Object { $_ }).Count) { @($specLines | Where-Object { $_ })[-1] } else { 'spec: FAILED, it did not start (no log)' }
$specLast
$bsdLast = "spec freebsd: no snippet added or edited"
$bsdOk = $true
if ($bsd)
{
  $bsd.WaitForExit()
  $summary = Get-SpecSummary $bsd $bsdLog "spec freebsd ($($changed.Count) snippets)"
  $summary.Lines | Where-Object { $_ -match '^(FAIL|no FreeBSD)' }
  $bsdLast = $summary.Last
  $bsdOk = $summary.Ok
  $bsdLast
}
$dbgLast = "spec debug-info: no snippet added or edited, no debug information touched"
$dbgOk = $true
if ($dbg)
{
  $dbg.WaitForExit()
  $summary = Get-SpecSummary $dbg $dbgLog "spec debug-info ($dbgWhat)"
  $summary.Lines | Where-Object { $_ -match '^FAIL' }
  $dbgLast = $summary.Last
  $dbgOk = $summary.Ok
  $dbgLast
}
if ($lnx)
{
  # (by now run_tests and the spec have ended: this seldom waits, and a WSL that hung after the probe
  # is given up on after 2 minutes)
  if (-not $lnx.WaitForExit(120000))
  {
    & taskkill /T /F /PID $lnx.Id 2>&1 | Out-Null
    $lnxLast = 'spec linux release: skipped, WSL did not finish in 2 minutes'
  }
  else
  {
    $summary = Get-SpecSummary $lnx $lnxLog 'spec linux release'
    $lnxFails = @($summary.Lines | Where-Object { $_ -match '^FAIL' })
    if ($lnxFails.Count -and -not @($lnxFails | Where-Object { $_ -notmatch 'program timed out' }).Count) { $lnxLast = "spec linux release: skipped, $($lnxFails.Count) timed out under WSL" }
    else
    {
      $lnxFails
      $lnxLast = $summary.Last
      $lnxOk = $summary.Ok
    }
  }
}
$lnxLast
$seconds = [int]((Get-Date) - $started).TotalSeconds
$passed = $rt.ExitCode -eq 0 -and $spec.ExitCode -eq 0 -and $rtLast -match '^run_tests: ALL PASSED' -and $specLast -match '^spec: ALL PASSED' -and $bsdOk -and $dbgOk -and $lnxOk
$kind = if ($Full) { 'full' } else { 'quick' }
if ($passed) { "gate: ALL PASSED ($kind`: $($rtLast -replace '^run_tests: ', 'run_tests ') ; $($specLast -replace '^spec: ', 'spec ') ; $bsdLast ; $dbgLast ; $lnxLast ; $seconds s)"; exit 0 }
"gate: FAILED ($kind`: $rtLast, exit $($rt.ExitCode); $specLast, exit $($spec.ExitCode); $bsdLast; $dbgLast; $lnxLast; $seconds s)"
exit 1
