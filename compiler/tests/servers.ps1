# Compile servers' lifetime (docs/compile-speed.md, "The compile server"): a server exits once the kppc
# it was started from changes (a rebuild, an install: it never serves a new client, whose key differs,
# and should not hold its memory either), and `kppc --stop-all-servers` stops every server of this
# user. The servers here run from a copy of kppc with a TEMP of the test's own: --stop-all-servers
# stops those alone (it finds the servers by their files in %TEMP%\kppc-server). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\servers'
# The servers an earlier run left (a failed check): processes running from this test's own folder.
function StopLeftovers()
{
  $root = [IO.Path]::GetFullPath($work)
  Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.Id -Force }
}
if (Test-Path $work) { StopLeftovers; cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force "$work\bin", "$work\tmp" | Out-Null
$failed = 0
function Fail([string]$what) { "FAIL servers: $what"; $script:failed++ }
$env:TMP = "$work\tmp"
$env:TEMP = "$work\tmp"
$env:KPP_WATCH = '1'; $env:KPP_WATCH_CHECK = '1' # (the watched listings, each build's compared with a listing: thread A's review)
$std = (Resolve-Path "$PSScriptRoot\..\..\stdlib").Path
$copy = "$work\bin\kppc.exe"
Copy-Item $Kppc $copy
Set-Content -Encoding ascii "$work\one.kpp" "int main() { println(1); return 0; }"
Set-Content -Encoding ascii "$work\two.kpp" "int main() { println(2); return 0; }"

# The process ids of the servers whose logs are in the test's TEMP, by log.
function Servers()
{
  $out = @{}
  foreach ($log in @(Get-ChildItem "$work\tmp\kppc-server\*.log" -ErrorAction SilentlyContinue))
  {
    $first = Get-Content $log.FullName -TotalCount 1
    if ($first -match '\[(\d+)\] started') { $out[$log.FullName] = [int]$Matches[1] }
  }
  return $out
}
# (Alive only while the process runs from this test's folder: a server that ended may have its id reused
# by another process, and with other threads' builds running, one was, giving "3 servers running", 2026-11-04.)
function Alive([int]$id)
{
  $process = Get-Process -Id $id -ErrorAction SilentlyContinue
  return $null -ne $process -and $process.Path -and $process.Path.StartsWith([IO.Path]::GetFullPath($work), [StringComparison]::OrdinalIgnoreCase)
}
function Build([string]$name)
{
  $ErrorActionPreference = 'Continue'
  $report = & $copy --server --std $std --exe "$work\$name.exe" "$work\$name.kpp" 2>&1 | Out-String
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($code -ne 0) { Fail "$name did not build through a server: $report" }
}
# Waits up to $seconds for a server to end: on its process (WaitForExit returns when it ends), so the
# limits are generous for a loaded machine (other threads' gates) and cost nothing when it ends at once.
function Ended([int]$id, [int]$seconds)
{
  if (-not (Alive $id)) { return $true }
  $process = Get-Process -Id $id -ErrorAction SilentlyContinue
  if (-not $process) { return $true }
  return $process.WaitForExit($seconds * 1000)
}

# 1. A server exits once its kppc changes (here: written again, with a later time).
Build 'one'
$servers = Servers
if ($servers.Count -ne 1) { Fail "$($servers.Count) servers after one build, 1 expected" }
else
{
  $log = @($servers.Keys)[0]
  $id = $servers[$log]
  if (-not (Alive $id)) { Fail "the server ($id) is not running after its build" }
  Copy-Item $Kppc $copy -Force
  (Get-Item $copy).LastWriteTime = (Get-Date).AddMinutes(1)
  if (-not (Ended $id 60)) { Fail "the server ($id) still runs 60 s after its kppc changed" }
  elseif (-not (Select-String -Path $log -SimpleMatch 'changed: exiting' -Quiet)) { Fail "the server's log does not say why it ended: $(Get-Content $log -Tail 2)" }
}

# 2. --stop-all-servers stops every server (two of the new kppc's, for two programs).
Build 'one'
Build 'two'
$running = @((Servers).Values | Where-Object { Alive $_ })
if ($running.Count -ne 2) { Fail "$($running.Count) servers running after two builds, 2 expected" }
$ErrorActionPreference = 'Continue'
$said = & $copy --stop-all-servers 2>&1 | Out-String
$ErrorActionPreference = 'Stop'
if ($said -notmatch '2 compile servers stopped') { Fail "--stop-all-servers said: $said" }
foreach ($id in $running) { if (-not (Ended $id 30)) { Fail "the server $id still runs after --stop-all-servers" } }

# 3. The server directory stays bounded: a server's start removes what is more than ten minutes old and
# in no one's use (a copy of kppc whose server ended, the files of a key without a copy, a verify's
# outputs, a copy's part), and leaves a running server's copy and its files, a copy a client holds
# open (one it is starting) and what is younger.
$dir = "$work\tmp\kppc-server"
function Plant([string]$name, [bool]$old)
{
  $path = "$dir\$name"
  Set-Content -Encoding ascii $path 'planted'
  if ($old) { $t = (Get-Date).AddHours(-2); (Get-Item $path).CreationTime = $t; (Get-Item $path).LastWriteTime = $t }
}
# What the servers above left (their copies and logs, none running now) is made two hours old: the
# copy of the replaced kppc's server goes, the one 'one' runs from again (below) stays.
$earlier = @(Get-ChildItem $dir -File)
foreach ($f in $earlier) { $t = (Get-Date).AddHours(-2); $f.CreationTime = $t; $f.LastWriteTime = $t }
# The server of 1., which ended when its kppc was replaced: its copy and log, old and unused.
$replaced = @(Select-String -Path "$dir\*.log" -SimpleMatch 'changed: exiting' | ForEach-Object Path)
if ($replaced.Count -ne 1) { Fail "$($replaced.Count) logs of a server whose kppc changed, 1 expected" }
$replaced += $replaced | ForEach-Object { $_ -replace '\.log$', '.exe' }
$gone = 'aaaaaaaaaaaaaaaa.exe', 'aaaaaaaaaaaaaaaa.log', 'aaaaaaaaaaaaaaaa.args', 'dddddddddddddddd.log', 'verify.4242.out', 'verify.4242.err', 'eeeeeeeeeeeeeeee.exe.999'
$stay = 'bbbbbbbbbbbbbbbb.exe', 'bbbbbbbbbbbbbbbb.log', 'cccccccccccccccc.exe', 'cccccccccccccccc.log'
foreach ($name in $gone + 'bbbbbbbbbbbbbbbb.exe', 'bbbbbbbbbbbbbbbb.log') { Plant $name $true }
foreach ($name in 'cccccccccccccccc.exe', 'cccccccccccccccc.log') { Plant $name $false }
$held = [IO.File]::Open("$dir\bbbbbbbbbbbbbbbb.exe", 'Open', 'Read', 'Read') # (as startServer holds the copy it starts)
# A running server whose copy and log are old ('one' runs again from its aged copy): its copy is its
# image, which is not deleted.
Build 'one'
# (A key's log is appended to: its latest server is its last "started" line, not Servers' first.)
$runningServer = @(Get-ChildItem "$dir\*.log" | ForEach-Object {
  $last = @(Select-String -Path $_.FullName -Pattern '\[(\d+)\] started') | Select-Object -Last 1
  if ($last) { [pscustomobject]@{ Key = $_.FullName; Value = [int]$last.Matches[0].Groups[1].Value } }
} | Where-Object { Alive $_.Value }) | Select-Object -First 1
if (-not $runningServer) { Fail "no server runs after building one" }
$runningCopy = $runningServer.Key -replace '\.log$', '.exe'
if ((Get-Item $runningCopy).CreationTime -gt (Get-Date).AddHours(-1)) { Fail "'one' did not run from its aged copy: the check proves nothing" }
Build 'two' # (another key: its server's start prunes)
$held.Close()
foreach ($name in $gone) { if (Test-Path "$dir\$name") { Fail "$name is still in the server directory: stale and in no one's use" } }
foreach ($name in $stay) { if (-not (Test-Path "$dir\$name")) { Fail "$name was deleted: held open, or younger than ten minutes" } }
foreach ($path in $runningCopy, $runningServer.Key) { if (-not (Test-Path $path)) { Fail "a running server's $(Split-Path -Leaf $path) was deleted" } }
foreach ($path in $replaced) { if (Test-Path $path) { Fail "$(Split-Path -Leaf $path), of the server whose kppc was replaced, is still there" } }
$ErrorActionPreference = 'Continue'
& $copy --stop-all-servers 2>&1 | Out-Null
$ErrorActionPreference = 'Stop'

# 4. One server for Visual Studio's builds and a terminal's: --msvc-diagnostics is not in the key
# (server.cpp, serverKey). The server marks each diagnostic's place and the client writes it in its
# own form (source.hpp, renderDiagnostics): `path(line)` with --msvc-diagnostics, `path:line`
# without, with the column under --diagnostic-columns (which is in the key: another server). The
# errors come in the order and the words of a build without a server; a replay (no input changed)
# is in the asking client's form, not the first's.
Set-Content -Encoding ascii "$work\bad.kpp" "int main()`n{`n  int a = noSuchName;`n  return otherMissing;`n}"
function Errors([string[]]$options)
{
  $ErrorActionPreference = 'Continue'
  $out = & $copy @options --std $std --exe "$work\bad.exe" "$work\bad.kpp" 2>&1 | ForEach-Object { "$_" }
  $ErrorActionPreference = 'Stop'
  return @($out | Where-Object { $_ -match ': error: ' })
}
foreach ($columns in @($false, $true))
{
  $c = @(if ($columns) { '--diagnostic-columns' })
  $alone = Errors $c
  foreach ($msvc in @($true, $false, $true))
  {
    $m = @(if ($msvc) { '--msvc-diagnostics' })
    $place = if ($msvc) { if ($columns) { '\(3,\d+\)' } else { '\(3\)' } } else { if ($columns) { ':3:\d+' } else { ':3' } }
    $got = Errors (@('--server') + $m + $c)
    $want = Errors ($m + $c)
    $what = "kppc --server $($m + $c -join ' ')"
    if ($got.Count -ne 2 -or $got[0] -notmatch "bad\.kpp$place`: error: .*noSuchName") { Fail "${what}: the errors are not in its form:`n$($got -join "`n")" }
    elseif (($got -join "`n") -match "[\x01\x02\x03]") { Fail "${what}: a marked place was written as it is" }
    elseif (-not $msvc -and ($got -join "`n") -ne ($alone -join "`n")) { Fail "${what}: not what a build without a server writes:`n$($got -join "`n")`nwithout a server:`n$($alone -join "`n")" }
    elseif ($msvc -and ((@($got) | Sort-Object) -join "`n") -ne ((@($want) | Sort-Object) -join "`n")) { Fail "${what}: other errors than without a server:`n$($got -join "`n")`nwithout a server:`n$($want -join "`n")" }
  }
  $running = @((Servers).Values | Where-Object { Alive $_ })
  if ($running.Count -ne 1) { Fail "kppc --server $($c -join ' '), with and without --msvc-diagnostics: $($running.Count) servers running, 1 expected" }
  $ErrorActionPreference = 'Continue'
  & $copy --stop-all-servers 2>&1 | Out-Null
  $ErrorActionPreference = 'Stop'
  foreach ($id in $running) { Ended $id 30 | Out-Null }
}

# A build with nothing changed is a replay (the server's log: "no input changed") whatever Visual
# Studio's own variables say (VSTEL_CurrentSolutionBuildID changes with each of its builds; thread K,
# 2026-11-05), and not one when a variable a build may read changed (any other: here KPP_FINGERPRINT_TEST).
function Replays() { @(Get-ChildItem "$work\tmp\kppc-server\*.log" -ErrorAction SilentlyContinue | ForEach-Object { Get-Content $_.FullName } | Where-Object { $_ -match 'no input changed' }).Count }
Build 'one'
$before = Replays
foreach ($step in @(@('VSTEL_CurrentSolutionBuildID', 1), @('ServiceHubLogSessionKey', 1), @("VsPerMonitorDpiAwarenessEnabled.$PID", 1), @('KPP_FINGERPRINT_TEST', 0)))
{
  Set-Item "env:$($step[0])" ([guid]::NewGuid().ToString())
  Build 'one'
  $after = Replays
  if ($after - $before -ne $step[1]) { Fail "a build with nothing changed but $($step[0]): $(if ($step[1]) { 'not a replay' } else { 'a replay, though a variable a build may read changed' })" }
  $before = $after
  Remove-Item "env:$($step[0])"
  Build 'one' # (the variable gone: a build of its own, then the next step's starts from it)
  $before = Replays
}
$ErrorActionPreference = 'Continue'
& $copy --stop-all-servers 2>&1 | Out-Null
$ErrorActionPreference = 'Stop'

StopLeftovers
if ($failed -eq 0) { "ok   servers: one exits when its kppc changes, --stop-all-servers stops them all, stale server files go, one server writes each client's diagnostic form, Visual Studio's own variables leave a replay a replay" }
exit $failed
