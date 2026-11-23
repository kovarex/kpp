# PRACOVNA's heavy-job lock (kovarex, 2026-11-23: "make sure every thread benchmarks builds etc one at
# a time"). heavy.cmd runs one command line while holding the machine's one heavy slot: gates, game
# builds, game test runs, convert.cmd, the differential suite, links. --exclusive (benchmarks and every
# timing run) takes the same slot and then also waits until the machine is otherwise idle.
#
#   heavy.cmd [--exclusive] [--name TEXT] COMMAND LINE...
#
# The slot is a named mutex (Global\kpp-heavy): Windows frees it when its holder dies, so a killed job
# never leaves the machine locked. Waiters queue in arrival order (C:\kpp-sandbox\heavy\queue, one file
# per waiting process; a dead waiter's file is dropped). The holder is in C:\kpp-sandbox\heavy\holder.txt;
# `heavy.cmd --status` prints it and the queue. The command runs with KPP_HEAVY_HELD set: a heavy script
# it calls (gate.cmd calls run_tests.cmd and spec.cmd) runs at once instead of waiting for its own parent.
# It also waits while C:\kpp-sandbox\scratch\schedule.md's line 1 is not "HOLD: no" or a
# quiet-window.flag whose holder is alive exists, and an --exclusive job writes that flag for the scripts
# that still check it (guard.sh, guard.cmd). The exit code is the command's.
$ErrorActionPreference = 'Stop'
$root = 'C:\kpp-sandbox\heavy'
$queueDir = Join-Path $root 'queue'
$holderFile = Join-Path $root 'holder.txt'
$scratch = 'C:\kpp-sandbox\scratch'
$flag = Join-Path $scratch 'quiet-window.flag'
$schedule = Join-Path $scratch 'schedule.md'
$line = $env:KPP_HEAVY_COMMAND
if ($null -eq $line) { $line = '' }
$line = $line.Trim()
$exclusive = $false
$name = ''
$status = $false
while ($true)
{
  if ($line -match '^--exclusive(\s+|$)(.*)$') { $exclusive = $true; $line = $matches[2]; continue }
  if ($line -match '^--status(\s+|$)(.*)$') { $status = $true; $line = $matches[2]; continue }
  if ($line -match '^--name\s+"([^"]*)"\s*(.*)$') { $name = $matches[1]; $line = $matches[2]; continue }
  if ($line -match '^--name\s+(\S+)\s*(.*)$') { $name = $matches[1]; $line = $matches[2]; continue }
  break
}

function Alive([int]$id) { return $null -ne (Get-Process -Id $id -ErrorAction SilentlyContinue) }

function Show-Status
{
  $text = if (Test-Path $holderFile) { (Get-Content -Raw $holderFile -ErrorAction SilentlyContinue).Trim() } else { '' }
  if ($text -match '^pid (\d+)' -and (Alive ([int]$matches[1]))) { 'holder: ' + $text } else { 'holder: none' }
  $waiting = @(Get-ChildItem $queueDir -File -ErrorAction SilentlyContinue | Sort-Object Name)
  'waiting: ' + $waiting.Count
  foreach ($w in $waiting) { '  ' + (Get-Content -Raw $w.FullName -ErrorAction SilentlyContinue).Trim() }
}

# status.html (C:\kpp-sandbox\heavy, kovarex 2026-11-23: "some document with a table which gets updated
# and I can check"): the holder, the queue and the last 200 finished jobs (history.tsv), written again on
# every enqueue, start and finish; the open page reloads itself every 5 s and counts the running times.
$historyFile = Join-Path $root 'history.tsv'
$pageFile = Join-Path $root 'status.html'

function Html([string]$text) { return [System.Net.WebUtility]::HtmlEncode($text) }
function Iso([DateTime]$time) { return $time.ToString('yyyy-MM-ddTHH:mm:sszzz') }
function MdText([string]$text) { return ($text -replace '`', "'" -replace '\|', '/') }

function Who([string]$cwd)
{
  if ($cwd -match 'worktrees\\([^\\]+)') { return $matches[1] }
  if ($cwd -match 'scratch\\([^\\]+)') { return 'scratch\' + $matches[1] }
  return $cwd
}

# "pid N, KIND, since MM-dd HH:mm:ss, cwd DIR: LABEL" (holder.txt and the queue's files)
function Parse-Record([string]$text)
{
  if ($text -notmatch '^pid (\d+), (\w+), since (\d\d-\d\d \d\d:\d\d:\d\d), cwd (.*?): (.*)$') { return $null }
  $since = [DateTime]::ParseExact("$((Get-Date).Year)-$($matches[3])", 'yyyy-MM-dd HH:mm:ss', $null)
  if ($since -gt (Get-Date).AddDays(1)) { $since = $since.AddYears(-1) }
  return [pscustomobject]@{ Pid = [int]$matches[1]; Kind = $matches[2]; Since = $since; Cwd = $matches[4]; Label = $matches[5].Trim() }
}

function Duration([TimeSpan]$t)
{
  if ($t.TotalHours -ge 1) { return '{0}h {1:D2}m' -f [int][Math]::Floor($t.TotalHours), $t.Minutes }
  if ($t.TotalMinutes -ge 1) { return '{0}m {1:D2}s' -f $t.Minutes, $t.Seconds }
  return '{0}s' -f [int]$t.TotalSeconds
}

function Write-Page
{
  try
  {
    $md = New-Object System.Text.StringBuilder
    $jRunning = $null; $jQueue = New-Object System.Collections.ArrayList; $jFinished = New-Object System.Collections.ArrayList
    $now = Get-Date
    $rows = New-Object System.Text.StringBuilder
    $holder = $null
    if (Test-Path $holderFile) { $holder = Parse-Record ((Get-Content -Raw $holderFile -ErrorAction SilentlyContinue) + '').Trim() }
    if ($holder -and (Alive $holder.Pid))
    {
      $iso = $holder.Since.ToString('s')
      $jRunning = [ordered]@{ kind = $holder.Kind; folder = (Who $holder.Cwd); label = $holder.Label; since = (Iso $holder.Since) }
      [void]$rows.Append("<tr><td class='kind $($holder.Kind)'>$($holder.Kind)</td><td>$(Html (Who $holder.Cwd))</td><td class='cmd'>$(Html $holder.Label)</td><td>$($holder.Since.ToString('HH:mm:ss'))</td><td class='run' data-since='$iso'></td><td>$($holder.Pid)</td></tr>")
      [void]$md.Append("| $($holder.Kind) | $(Who $holder.Cwd) | ``$(MdText $holder.Label)`` | $($holder.Since.ToString('HH:mm')) | $(Duration ($now - $holder.Since)) |`n")
    }
    else { [void]$rows.Append("<tr><td colspan='6' class='none'>free</td></tr>"); [void]$md.Append("| free | | | | |`n") }
    $mdQueue = New-Object System.Text.StringBuilder
    $queue = New-Object System.Text.StringBuilder
    $n = 0
    foreach ($w in @(Get-ChildItem $queueDir -File -ErrorAction SilentlyContinue | Sort-Object Name))
    {
      $r = Parse-Record ((Get-Content -Raw $w.FullName -ErrorAction SilentlyContinue) + '').Trim()
      if (-not $r -or -not (Alive $r.Pid)) { continue }
      $n++
      [void]$jQueue.Add([ordered]@{ kind = $r.Kind; folder = (Who $r.Cwd); label = $r.Label; since = (Iso $r.Since) })
      [void]$queue.Append("<tr><td>$n</td><td class='kind $($r.Kind)'>$($r.Kind)</td><td>$(Html (Who $r.Cwd))</td><td class='cmd'>$(Html $r.Label)</td><td>$($r.Since.ToString('HH:mm:ss'))</td><td class='run' data-since='$($r.Since.ToString('s'))'></td></tr>")
      [void]$mdQueue.Append("| $n | $($r.Kind) | $(Who $r.Cwd) | ``$(MdText $r.Label)`` | $($r.Since.ToString('HH:mm')) | $(Duration ($now - $r.Since)) |`n")
    }
    if ($n -eq 0) { [void]$queue.Append("<tr><td colspan='6' class='none'>nobody waiting</td></tr>") }
    $mdHistory = New-Object System.Text.StringBuilder
    $history = New-Object System.Text.StringBuilder
    $lines = @(Get-Content $historyFile -Tail 200 -ErrorAction SilentlyContinue)
    [array]::Reverse($lines)
    foreach ($l in $lines)
    {
      # queued, started, finished (ISO), exit code, kind, cwd, label
      $f = $l -split "`t"
      if ($f.Count -lt 7) { continue }
      $queued = [DateTime]$f[0]; $started = [DateTime]$f[1]; $ended = [DateTime]$f[2]
      $ok = if ($f[3] -eq '0') { 'ok' } else { 'bad' }
      if ($jFinished.Count -lt 100) { [void]$jFinished.Add([ordered]@{ kind = $f[4]; folder = (Who $f[5]); label = $f[6]; queued = (Iso $queued); started = (Iso $started); ended = (Iso $ended); exit = [int]$f[3] }) }
      [void]$history.Append("<tr><td class='kind $($f[4])'>$($f[4])</td><td>$(Html (Who $f[5]))</td><td class='cmd'>$(Html $f[6])</td><td>$($started.ToString('MM-dd HH:mm'))</td><td>$(Duration ($started - $queued))</td><td>$(Duration ($ended - $started))</td><td class='$ok'>$($f[3])</td></tr>")
      if ($mdHistory.Length -lt 20000) { [void]$mdHistory.Append("| $($f[4]) | $(Who $f[5]) | ``$(MdText $f[6])`` | $($started.ToString('MM-dd HH:mm')) | $(Duration ($started - $queued)) | $(Duration ($ended - $started)) | $(if ($f[3] -eq '0') { '0' } else { '**' + $f[3] + '**' }) |`n") }
    }
    if ($lines.Count -eq 0) { [void]$history.Append("<tr><td colspan='7' class='none'>none yet</td></tr>") }
    $page = @"
<!doctype html>
<html><head><meta charset="utf-8"><meta http-equiv="refresh" content="5"><title>heavy slot</title>
<style>
:root { --bg: #fff; --fg: #1a1a1a; --muted: #666; --line: #ddd; --head: #f3f3f3; --ok: #1a7f37; --bad: #c62828; --ex: #8250df; --hv: #0969da; }
@media (prefers-color-scheme: dark) { :root { --bg: #161616; --fg: #e6e6e6; --muted: #999; --line: #333; --head: #222; --ok: #4ac26b; --bad: #ff6b6b; --ex: #b392f0; --hv: #58a6ff; } }
body { background: var(--bg); color: var(--fg); font: 14px/1.4 system-ui, sans-serif; margin: 16px; }
h1 { font-size: 18px; margin: 0 0 4px; } h2 { font-size: 15px; margin: 20px 0 6px; }
.muted, .none { color: var(--muted); } table { border-collapse: collapse; width: 100%; }
th, td { border-bottom: 1px solid var(--line); padding: 4px 8px; text-align: left; vertical-align: top; }
th { background: var(--head); font-weight: 600; } td.cmd { font-family: Consolas, monospace; font-size: 12px; word-break: break-all; }
.kind.exclusive { color: var(--ex); font-weight: 600; } .kind.heavy { color: var(--hv); } .ok { color: var(--ok); } .bad { color: var(--bad); font-weight: 600; }
</style></head><body>
<h1>PRACOVNA heavy slot</h1>
<div class="muted">One heavy job at a time (tools\heavy\heavy.cmd). Written $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'); reloads every 5 s.</div>
<h2>Running</h2>
<table><tr><th>kind</th><th>folder</th><th>command</th><th>started</th><th>running</th><th>pid</th></tr>$($rows.ToString())</table>
<h2>Waiting</h2>
<table><tr><th>#</th><th>kind</th><th>folder</th><th>command</th><th>queued</th><th>waiting</th></tr>$($queue.ToString())</table>
<h2>Finished (last 200)</h2>
<table><tr><th>kind</th><th>folder</th><th>command</th><th>started</th><th>waited</th><th>took</th><th>exit</th></tr>$($history.ToString())</table>
<script>
function d(s) { s = Math.max(0, Math.floor(s)); var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), x = s % 60;
  return h ? h + 'h ' + String(m).padStart(2, '0') + 'm' : m ? m + 'm ' + String(x).padStart(2, '0') + 's' : x + 's'; }
function tick() { document.querySelectorAll('[data-since]').forEach(function (e) { e.textContent = d((Date.now() - new Date(e.dataset.since)) / 1000); }); }
tick(); setInterval(tick, 1000);
</script>
</body></html>
"@
    $temp = "$pageFile.$PID.tmp"
    [IO.File]::WriteAllText($temp, $page, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -Force $temp $pageFile
    # The same as markdown (status.md), for reading in a terminal or an editor; publish.ps1 uploads the page.
    $noneQueue = if ($mdQueue.Length) { '' } else { "| | nobody waiting | | | | |`n" }
    $text = "# PRACOVNA heavy slot`n`nWritten $($now.ToString('yyyy-MM-dd HH:mm:ss')) (times are PRACOVNA's; durations as of then).`n`n" +
      "## Running`n`n| kind | folder | command | started | running |`n|---|---|---|---|---|`n" + $md.ToString() +
      "`n## Waiting`n`n| # | kind | folder | command | queued | waiting |`n|---|---|---|---|---|---|`n" + $mdQueue.ToString() + $noneQueue +
      "`n## Finished`n`n| kind | folder | command | started | waited | took | exit |`n|---|---|---|---|---|---|---|`n" + $mdHistory.ToString()
    $mdFile = Join-Path $root 'status.md'
    # And as JSON (status.json), which publish.ps1 posts to heavyboard on kovarex.com (board\README.md).
    $json = ([ordered]@{ written = (Iso $now); running = $jRunning; queue = @($jQueue); finished = @($jFinished) } | ConvertTo-Json -Depth 4 -Compress)
    $jsonFile = Join-Path $root 'status.json'
    [IO.File]::WriteAllText("$jsonFile.$PID.tmp", $json, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -Force "$jsonFile.$PID.tmp" $jsonFile
    [IO.File]::WriteAllText("$mdFile.$PID.tmp", $text, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -Force "$mdFile.$PID.tmp" $mdFile
    if (Test-Path (Join-Path $root 'publish.txt'))
    {
      Start-Process powershell -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\publish.ps1`"")
    }
  }
  catch { try { Set-Content -Path (Join-Path $root 'page-error.txt') -Value "$(Get-Date -Format s) $_ $($_.InvocationInfo.PositionMessage)" } catch { } } # (the page is a view: a failed write never stops a job)
}

if ($status) { Show-Status; Write-Page; exit 0 }
if (-not $line) { 'usage: heavy.cmd [--exclusive] [--name TEXT] COMMAND LINE...  |  heavy.cmd --status'; exit 2 }

function Run-Command
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $env:ComSpec
  $info.Arguments = '/d /s /c "' + $line + '"'
  $info.UseShellExecute = $false
  $info.WorkingDirectory = (Get-Location).Path
  $info.EnvironmentVariables['KPP_HEAVY_HELD'] = "$PID"
  $process = [System.Diagnostics.Process]::Start($info)
  $process.WaitForExit()
  return $process.ExitCode
}

# Inside a heavy job already (its own child): no second wait. CI and machines without C:\kpp-sandbox
# have no lock.
if ($env:KPP_HEAVY_HELD -or $env:GITHUB_ACTIONS -or -not (Test-Path 'C:\kpp-sandbox')) { exit (Run-Command) }

New-Item -ItemType Directory -Force $queueDir | Out-Null
$label = if ($name) { $name } else { $line }
if ($label.Length -gt 160) { $label = $label.Substring(0, 160) + '...' }
$kind = if ($exclusive) { 'exclusive' } else { 'heavy' }
$me = "pid $PID, $kind, since {0}, cwd {1}: {2}"
$ticket = Join-Path $queueDir ('{0:D20}-{1}.txt' -f [DateTime]::UtcNow.Ticks, $PID)
$queuedAt = Get-Date
Set-Content -Path $ticket -Value ($me -f $queuedAt.ToString('MM-dd HH:mm:ss'), (Get-Location).Path, $label) -Encoding UTF8
Write-Page

function First-In-Queue
{
  foreach ($w in @(Get-ChildItem $queueDir -File -ErrorAction SilentlyContinue | Sort-Object Name))
  {
    $id = [int](($w.BaseName -split '-')[1])
    if ($id -eq $PID) { return $true }
    if (Alive $id) { return $false }
    Remove-Item $w.FullName -Force -ErrorAction SilentlyContinue
  }
  return $true
}

function Machine-Held
{
  $first = Get-Content $schedule -TotalCount 1 -ErrorAction SilentlyContinue
  if ($first -and $first.Trim() -ne 'HOLD: no') { return "schedule.md line 1 is '$($first.Trim())'" }
  if (Test-Path $flag)
  {
    $text = Get-Content -Raw $flag -ErrorAction SilentlyContinue
    if ($text -match 'heavy pid (\d+)' -and -not (Alive ([int]$matches[1]))) { Remove-Item $flag -Force -ErrorAction SilentlyContinue }
    else { return 'quiet-window.flag exists' }
  }
  return $null
}

$mutex = New-Object System.Threading.Mutex($false, 'Global\kpp-heavy')
$held = $false
$said = ''
$code = 1
try
{
  while (-not $held)
  {
    $why = $null
    if (-not (First-In-Queue)) { $why = 'queued behind an earlier job' }
    if (-not $why) { $why = Machine-Held }
    if (-not $why)
    {
      try { $held = $mutex.WaitOne(2000) } catch [System.Threading.AbandonedMutexException] { $held = $true }
      if (-not $held) { $why = 'the heavy slot is taken' }
    }
    if ($held) { break }
    $holder = if (Test-Path $holderFile) { (Get-Content -Raw $holderFile -ErrorAction SilentlyContinue).Trim() } else { '' }
    $now = "heavy: waiting ($why). holder: $holder"
    if ($now -ne $said) { [Console]::Error.WriteLine($now); $said = $now; Write-Page }
    Start-Sleep -Seconds 3
  }
  Remove-Item $ticket -Force -ErrorAction SilentlyContinue
  $startedAt = Get-Date
  Set-Content -Path $holderFile -Value ($me -f $startedAt.ToString('MM-dd HH:mm:ss'), (Get-Location).Path, $label) -Encoding UTF8
  Write-Page

  if ($exclusive)
  {
    Set-Content -Path $flag -Value "heavy pid $PID ($label)" -Encoding UTF8
    # Otherwise idle: total CPU under 15% for 20 seconds in a row (the Claude app alone keeps it near 7%).
    $quiet = 0
    $said = ''
    while ($quiet -lt 20)
    {
      $load = (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average
      if ($load -lt 15) { $quiet += 2 } else
      {
        $quiet = 0
        $now = 'heavy: exclusive, waiting for the machine to go idle'
        if ($now -ne $said) { [Console]::Error.WriteLine("$now (load $load%)"); $said = $now }
      }
      Start-Sleep -Seconds 2
    }
  }
  $code = Run-Command
}
finally
{
  Remove-Item $ticket -Force -ErrorAction SilentlyContinue
  if ($held)
  {
    if ($exclusive) { Remove-Item $flag -Force -ErrorAction SilentlyContinue }
    Remove-Item $holderFile -Force -ErrorAction SilentlyContinue
    $mutex.ReleaseMutex()
    $clean = ($label -replace "[`t`r`n]", ' ')
    $record = @($queuedAt.ToString('s'), $startedAt.ToString('s'), (Get-Date).ToString('s'), "$code", $kind, (Get-Location).Path, $clean) -join "`t"
    try { Add-Content -Path $historyFile -Encoding UTF8 -Value $record } catch { }
  }
  $mutex.Dispose()
  Write-Page
}
exit $code
