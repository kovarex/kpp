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

if ($status) { Show-Status; exit 0 }
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
Set-Content -Path $ticket -Value ($me -f (Get-Date -Format 'MM-dd HH:mm:ss'), (Get-Location).Path, $label) -Encoding UTF8

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
    if ($now -ne $said) { [Console]::Error.WriteLine($now); $said = $now }
    Start-Sleep -Seconds 3
  }
  Remove-Item $ticket -Force -ErrorAction SilentlyContinue
  Set-Content -Path $holderFile -Value ($me -f (Get-Date -Format 'MM-dd HH:mm:ss'), (Get-Location).Path, $label) -Encoding UTF8

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
  }
  $mutex.Dispose()
}
exit $code
