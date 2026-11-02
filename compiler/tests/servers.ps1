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
function Alive([int]$id) { return $null -ne (Get-Process -Id $id -ErrorAction SilentlyContinue) }
function Build([string]$name)
{
  $ErrorActionPreference = 'Continue'
  $report = & $copy --server --std $std --exe "$work\$name.exe" "$work\$name.kpp" 2>&1 | Out-String
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($code -ne 0) { Fail "$name did not build through a server: $report" }
}
# Waits up to $seconds for a process to end.
function Ended([int]$id, [int]$seconds)
{
  for ($i = 0; $i -lt $seconds * 4; $i++)
  {
    if (-not (Alive $id)) { return $true }
    Start-Sleep -Milliseconds 250
  }
  return $false
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
  if (-not (Ended $id 20)) { Fail "the server ($id) still runs 20 s after its kppc changed" }
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
foreach ($id in $running) { if (-not (Ended $id 10)) { Fail "the server $id still runs after --stop-all-servers" } }

StopLeftovers
if ($failed -eq 0) { "ok   servers: one exits when its kppc changes, --stop-all-servers stops them all" }
exit $failed
