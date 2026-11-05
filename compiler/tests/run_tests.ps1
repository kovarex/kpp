# run_tests' checks, side by side (run_tests.cmd runs this): each check is a process of its own (a
# tests\*.ps1 script, or a section of tests\inline.cmd: an end-to-end program, a library's tests, ...),
# -Jobs of them at once, the longest first. Each check's output is printed whole when it ends, and
# the last line sums up every part, as it did when they ran one after another:
#   run_tests: ALL PASSED (end-to-end 45/45, checks 45/45)                        exit 0
#   run_tests: 2 FAILED: e2e enums, targets (end-to-end 44/45, checks 44/45)       exit 1
# A check that cannot run here (lsp-smoke without Node.js) is added as ", skipped: ...". -Part 1 or 2
# runs one half, for CI's two parallel jobs (docs/build.md, "Continuous integration"): part 2 is
# determinism, concurrency, generate and project, part 1 the rest.
# Every check has a directory of its own under build\tests, and the ones that start compile servers
# find theirs by their own directories (incremental.ps1, project.ps1) or a TEMP of their own
# (servers.ps1, editcost.ps1, ...), so that they can run at once, and beside other worktrees' runs.
# Without -Full, the quick gate a thread runs before a push (kovarex, 2026-11-04: speed over zero
# risk): the checks marked Full are left out (they rarely fail and take the longest: concurrency's
# hundreds of builds, incremental's server edits, windows-gnu) and determinism runs its quick mode.
# -Full: all of it, as CI runs it on every push to master and thread H's batch runs on master.
param([string]$Part = '', [int]$Jobs = 0, [switch]$Full)
$ErrorActionPreference = 'Stop'
$compiler = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location $compiler
$kppc = 'build\kppc.exe'
$logs = Join-Path $compiler 'build\tests\logs'
# What an earlier run's deletion left (it was still deleting, or stopped).
foreach ($d in @(Get-ChildItem -Directory 'build\tests.old-*' -ErrorAction SilentlyContinue)) { cmd /c "rd /s /q `"$($d.FullName)`" 2> nul" }
New-Item -ItemType Directory -Force $logs | Out-Null
# Streaming's barrier check in codegen (KPP_CHECK_BARRIER, docs/codegen-speed.md), on for every build
# here until program pieces rely on it.
$env:KPP_CHECK_BARRIER = '1'
# No shared object cache from the caller's environment (KPP_OBJECT_CACHE): checks count the modules a
# build compiles (tests\incremental.ps1); tests\objectcache.ps1 sets its own.
Remove-Item Env:KPP_OBJECT_CACHE -ErrorAction SilentlyContinue
if ($Jobs -le 0) { $Jobs = [Math]::Max(4, [Math]::Min(12, [int]([Environment]::ProcessorCount / 3))) }

# The checks. Kind: e2e (an end-to-end program: counted apart), check (counted in "checks"). Skip: an
# exit code that means "could not run here" -> its reason, Counted: whether such a skip still counts
# as a passed check (targets' Linux part does: the rest of targets ran). Seconds: about how long it
# takes alone, so that the longest start first. After: names of checks it waits for. Note: a line
# printed after a failure's output.
$checks = New-Object System.Collections.Generic.List[object]
function Add([string]$name, [string[]]$command, [int]$part = 1, [double]$seconds = 5, [hashtable]$skip = @{}, [bool]$counted = $false, [string]$kind = 'check', [string]$note = '', [string[]]$after = @(), [bool]$fullOnly = $false)
{
  $checks.Add([pscustomobject]@{ Name = $name; Command = $command; Part = $part; Seconds = $seconds; Skip = $skip; Counted = $counted; Kind = $kind; Note = $note; After = $after; FullOnly = $fullOnly })
}
function PsCheck([string]$script) { return @('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script, '-Kppc', $kppc) }
function Inline([string[]]$arguments) { return @('cmd', '/c', 'tests\inline.cmd') + $arguments }

# Part 1: the end-to-end programs (tests\run\*.kpp), then the checks.
foreach ($f in Get-ChildItem 'tests\run\*.kpp' | Sort-Object Name) { Add "e2e $($f.BaseName)" (Inline @('e2e', $f.BaseName)) -seconds 2 -kind 'e2e' }
Add 'access warnings' (Inline @('access')) -seconds 2
Add 'plain diagnostics' (Inline @('diagnostics', 'plain')) -seconds 2
Add 'msvc diagnostics' (Inline @('diagnostics', 'msvc')) -seconds 2
Add 'clashes' (Inline @('clashes')) -seconds 1
Add 'missing input' (Inline @('missing')) -seconds 1
Add 'runbuild' (Inline @('runbuild')) -seconds 2
Add 'clashes allowed' (Inline @('allowed')) -seconds 2
Add 'rejected module' (Inline @('rejected')) -seconds 2
# The same through LLVM's back end in kppc (tests\backend.ps1), when this kppc has it: an IR bug (the
# verifier) or what the back end rejects (LLVM's diagnostics) fails its module with LLVM's message.
Add 'LLVM back end' (PsCheck 'tests\backend.ps1') -seconds 5 -skip @{ 3 = 'LLVM back end (none in this kppc)' }
Add 'verifier' (PsCheck 'tests\verifier.ps1') -seconds 17
Add 'stubs' (PsCheck 'tests\stubs.ps1') -seconds 7
Add 'static constants' (PsCheck 'tests\staticconstants.ps1') -seconds 1
Add 'lto' (PsCheck 'tests\lto.ps1') -seconds 8
Add 'sections' (PsCheck 'tests\sections.ps1') -seconds 2
Add 'debug info' (PsCheck 'tests\debuginfo.ps1') -seconds 8
Add 'natvis' (PsCheck 'tests\natvis.ps1') -seconds 2
# --target (tests\targets.ps1): Linux run under WSL, skipped without it (3) or when it does not answer
# in 30 s (4); the rest passed.
Add 'targets' ((PsCheck 'tests\targets.ps1') + @(if (-not $Full) { '-NoLinux' })) -seconds 20 -counted $true -skip @{ 3 = 'linux targets (no WSL)'; 4 = 'linux targets (WSL did not answer in 30 s)'; 5 = 'linux targets (left to the full run)' }
Add 'specfiles' @('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tests\specfiles.ps1') -seconds 1
Add 'runcache' (PsCheck 'tests\runcache.ps1') -seconds 3
Add 'toolchain' (PsCheck 'tests\toolchain.ps1') -seconds 14
Add 'windows-gnu' (PsCheck 'tests\windowsgnu.ps1') -seconds 46 -fullOnly $true
Add 'import cache' (PsCheck 'tests\importcache.ps1') -seconds 4
Add 'servers' (PsCheck 'tests\servers.ps1') -seconds 13
Add 'edit cost' (PsCheck 'tests\editcost.ps1') -seconds 12
Add 'check errors' (PsCheck 'tests\checkerrors.ps1') -seconds 12
# KPP_OBJECT_CACHE: a second output directory takes the modules' objects from it, an edit compiles
# what changed, builds at once share it (tests\objectcache.ps1).
Add 'object cache' (PsCheck 'tests\objectcache.ps1') -seconds 6
# An MSVC link runs link.exe directly with the clang driver's translation, kept while its key holds
# (tests\directlink.ps1).
Add 'direct link' (PsCheck 'tests\directlink.ps1') -seconds 10
Add 'checkout' @('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', '..\programs\factorio\tests\checkout.ps1') -seconds 8
# Part 2.
Add 'determinism' ((PsCheck 'tests\determinism.ps1') + @(if ($Full) { '-Full' })) -part 2 -seconds 144 -note 'FAIL determinism: the output depends on the thread count'
Add 'concurrency' (PsCheck 'tests\concurrency.ps1') -part 2 -seconds 39 -fullOnly $true -note 'FAIL concurrency: the output depends on timing'
Add 'generate' (PsCheck 'tests\generate.ps1') -part 2 -seconds 11
Add 'project' (PsCheck 'tests\project.ps1') -part 2 -seconds 133
# Part 1 again.
Add 'lower case' @('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tests\lowercase.ps1') -seconds 1
# Code packages (tests\packages.ps1, docs/design.md): kppc --packages lists each directory as a package,
# with its README.md's first paragraph and the names each file declares.
Add 'packages' (PsCheck 'tests\packages.ps1') -seconds 1
Add 'lsp' (PsCheck 'tests\lsp.ps1') -seconds 9
$skipped = New-Object System.Collections.Generic.List[string]
if (Get-Command node -ErrorAction SilentlyContinue) { Add 'lsp-smoke' @('node', '..\tools\vscode\test\lsp-smoke.js', $kppc, '--lsp') -seconds 3 }
else { $skipped.Add('lsp-smoke (no Node.js)') }
Add 'incremental' (PsCheck 'tests\incremental.ps1') -seconds 108 -fullOnly $true -note "FAIL incremental: the compile server's build differs from a new process's"
$libraries = @(Get-ChildItem -Directory '..\libraries' | Sort-Object Name | ForEach-Object { "library $($_.Name)" })
foreach ($l in $libraries) { Add $l (Inline @('library', $l.Substring(8))) -seconds 10 }
# (after the libraries: it reads the repository, where their builds write)
Add 'kppsite' (Inline @('kppsite')) -seconds 10 -after $libraries

$leftOut = @($checks | Where-Object { -not $Full -and $_.FullOnly } | ForEach-Object Name)
$run = @($checks | Where-Object { (-not $Part -or $_.Part -eq [int]$Part) -and ($Full -or -not $_.FullOnly) } | Sort-Object -Property @{ Expression = 'Seconds'; Descending = $true })
$names = @($run | ForEach-Object Name)
foreach ($c in $run) { $c.After = @($c.After | Where-Object { $names -contains $_ }) }

# llvm-mingw (tools\sysroot\llvm-mingw.ps1: downloaded and unpacked once), which several checks use,
# made before they start: on a fresh clone verifier.ps1 and toolchain.ps1 fetched it at once and the
# second failed on its zip, "being used by another process" (thread K, 2026-11-05). A failure here is
# the checks' to report.
$ErrorActionPreference = 'Continue'
$mingw = @(& powershell -NoProfile -ExecutionPolicy Bypass -File '..\tools\sysroot\llvm-mingw.ps1' 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) { "llvm-mingw.ps1 failed (exit $LASTEXITCODE): $($mingw -join ' ')" }
$ErrorActionPreference = 'Stop'

# Runs them, -Jobs at once; a check waits for those in its After.
$started = Get-Date
$pending = New-Object System.Collections.ArrayList
foreach ($c in $run) { [void]$pending.Add($c) }
$runningNow = @{}
$done = @{}
$results = New-Object System.Collections.Generic.List[object]
function Start-Check($c)
{
  $log = Join-Path $logs (($c.Name -replace '[^A-Za-z0-9_.-]', '_') + '.log')
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $env:ComSpec
  # (cmd's own redirection: one file with stdout and stderr in the order they were written)
  $line = ($c.Command | ForEach-Object { if ($_ -match '[\s&()^]') { "`"$_`"" } else { $_ } }) -join ' '
  $info.Arguments = "/d /s /c `"$line > `"$log`" 2>&1`""
  $info.WorkingDirectory = $compiler
  $info.UseShellExecute = $false
  $p = [System.Diagnostics.Process]::Start($info)
  return [pscustomobject]@{ Check = $c; Process = $p; Log = $log; Start = Get-Date }
}
while ($pending.Count -or $runningNow.Count)
{
  foreach ($c in $pending.ToArray())
  {
    if ($runningNow.Count -ge $Jobs) { break }
    if (@($c.After | Where-Object { -not $done.ContainsKey($_) }).Count) { continue }
    $pending.Remove($c)
    $runningNow[$c.Name] = Start-Check $c
  }
  Start-Sleep -Milliseconds 100
  foreach ($r in @($runningNow.Values))
  {
    if (-not $r.Process.HasExited) { continue }
    $r.Process.WaitForExit()
    $code = $r.Process.ExitCode
    $runningNow.Remove($r.Check.Name)
    $done[$r.Check.Name] = $true
    $seconds = ((Get-Date) - $r.Start).TotalSeconds
    $text = if (Test-Path $r.Log) { [IO.File]::ReadAllText($r.Log).TrimEnd() } else { '' }
    if ($text) { $text }
    $outcome = if ($r.Check.Skip.ContainsKey($code)) { 'skip' } elseif ($code -eq 0) { 'ok' } else { 'fail' }
    if ($outcome -eq 'fail' -and $r.Check.Note) { $r.Check.Note }
    if ($outcome -eq 'fail' -and $r.Check.Kind -eq 'check' -and $text -notmatch '(?m)^FAIL') { "FAIL $($r.Check.Name): exit $code" }
    $results.Add([pscustomobject]@{ Check = $r.Check; Outcome = $outcome; Code = $code; Seconds = $seconds })
  }
}

# The summary, as run_tests.cmd's was.
$e2e = @($results | Where-Object { $_.Check.Kind -eq 'e2e' })
$checked = @($results | Where-Object { $_.Check.Kind -eq 'check' -and ($_.Outcome -ne 'skip' -or $_.Check.Counted) })
$failedOnes = @($results | Where-Object { $_.Outcome -eq 'fail' } | Sort-Object { $names.IndexOf($_.Check.Name) })
foreach ($r in $results | Where-Object { $_.Outcome -eq 'skip' } | Sort-Object { $names.IndexOf($_.Check.Name) }) { $skipped.Add($r.Check.Skip[$r.Code]) }
$e2ePassed = @($e2e | Where-Object { $_.Outcome -eq 'ok' }).Count
$checksPassed = @($checked | Where-Object { $_.Outcome -ne 'fail' }).Count
"{0} passed, {1} failed (end-to-end)" -f $e2ePassed, ($e2e.Count - $e2ePassed)
$slowest = @($results | Sort-Object Seconds -Descending | Select-Object -First 5 | ForEach-Object { "{0} {1:N0} s" -f $_.Check.Name, $_.Seconds })
"run_tests took {0:N0} s at {1} checks at once; the longest: {2}" -f ((Get-Date) - $started).TotalSeconds, $Jobs, ($slowest -join ', ')
$summary = if ($Part -eq '2') { "part 2/2: checks $checksPassed/$($checked.Count)" } else { "end-to-end $e2ePassed/$($e2e.Count), checks $checksPassed/$($checked.Count)" }
if ($Part -eq '1') { $summary = "part 1/2: $summary" }
if (-not $Full) { $summary = "quick: $summary, left to CI: $($leftOut -join ', ')" }
if ($skipped.Count) { $summary += ", skipped: " + ($skipped -join ', ') }
if ($failedOnes.Count)
{
  $failedNames = @($failedOnes | ForEach-Object { $_.Check.Name })
  "run_tests: $($failedOnes.Count) FAILED: $($failedNames -join ', ') ($summary)"
  exit 1
}
# Everything passed: the checks' builds go (build\tests held some 50,000 files of objects and IR after
# a run, and the kernel keeps a record of each cached file in its pool; a failed run keeps them).
# (renamed at once and deleted by a process of its own, which takes half a minute; the next run deletes
# what one left)
$old = "build\tests.old-$PID"
cmd /c "move build\tests $old > nul 2> nul"
if (Test-Path $old) { Start-Process -WindowStyle Hidden $env:ComSpec -ArgumentList '/c', "rd /s /q `"$compiler\$old`"" }
else { cmd /c "rd /s /q build\tests 2> nul" }
"run_tests: ALL PASSED ($summary)"
exit 0
