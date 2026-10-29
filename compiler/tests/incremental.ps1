# The compile server must build exactly what a new kppc process builds (docs/compile-speed.md,
# "The compile server"). A copy of tests\incremental\ is edited step by step (a function body, a
# signature, a class's layout, a file added, the file removed); after each edit it is built through
# one long-lived server and by a new process, and the IR, stdout and stderr (timings masked) must
# be identical, and the program the server built must print what the step expects. run_tests.cmd
# runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\incremental'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$src = Join-Path $work 'src'
New-Item -ItemType Directory -Force $src | Out-Null
Copy-Item "$PSScriptRoot\incremental\*.kpp" $src
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0
$started = Get-Date

function Normalize([string[]]$lines)
{
  $lines | ForEach-Object { $_ -replace '[0-9]+\.[0-9]+ s\b', '_ s' -replace '\([0-9.]+ [MG]?B/s[^)]*\)', '(_)' -replace '\([0-9.]+ M lines/s\)', '(_)' -replace '\(([0-9]+) requests, ([0-9]+) waits\)', '(_ requests, _ waits)' }
}

function Build([string[]]$arguments, [string]$label)
{
  $out = Join-Path $work "$label.out.txt"
  $err = Join-Path $work "$label.err.txt"
  # Not Start-Process -Wait: it waits for the client's whole process tree, the server included.
  $ErrorActionPreference = 'Continue'
  & $Kppc @arguments > $out 2> $err
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  return [pscustomobject]@{ exit = $code; out = @(Normalize (Get-Content $out)); err = @(Normalize (Get-Content $err)) }
}

function Edit([string]$file, [string]$old, [string]$new)
{
  $path = Join-Path $src $file
  $text = [IO.File]::ReadAllText($path)
  if (-not $text.Contains($old)) { throw "incremental.ps1: '$old' not found in $file" }
  [IO.File]::WriteAllText($path, $text.Replace($old, $new))
}

$steps = @(
  @{ name = 'first build'; expect = '6 40 40 3 8'; edit = {} },
  @{ name = 'body edit'; expect = '6 44 44 3 8'; edit = { Edit 'shapes.kpp' 'return v * 10;' 'return v * 11;' } },
  @{ name = 'signature edit'; expect = '6 44 44 103 8'; edit = { Edit 'shapes.kpp' 'int combine(int a, int b) { return a + b; }' 'int combine(int a, int b, int c = 100) { return a + b + c; }' } },
  @{ name = 'layout edit'; expect = '6 44 44 103 12'; edit = { Edit 'shapes.kpp' '  int height = 3;' "  int height = 3;`n  int depth = 4;" } },
  @{ name = 'new file'; expect = '6 44 1 103 12'; edit = { Set-Content (Join-Path $src 'extra.kpp') 'int scale(double v) { return 1; }' } },
  @{ name = 'file removed'; expect = '6 44 44 103 12'; edit = { Remove-Item (Join-Path $src 'extra.kpp') } }
)
$n = 0
foreach ($step in $steps)
{
  $n++
  & $step.edit
  $ir = Join-Path $work "server.$n.ll"
  $fresh = Join-Path $work "fresh.$n.ll"
  $exe = Join-Path $work 'server.exe'
  # The server's IR is written to one path every step (its arguments, so its server, stay the same).
  $s = Build @('--server', '--emit-llvm', (Join-Path $work 'server.ll'), $src) "server.$n"
  Copy-Item (Join-Path $work 'server.ll') $ir
  $f = Build @('--emit-llvm', (Join-Path $work 'fresh.ll'), $src) "fresh.$n"
  Copy-Item (Join-Path $work 'fresh.ll') $fresh
  $ok = $true
  if ($s.exit -ne $f.exit) { $ok = $false; "FAIL incremental ${n} ($($step.name)): exit $($s.exit) through the server, $($f.exit) fresh" }
  foreach ($part in 'out', 'err')
  {
    $d = Compare-Object $f.$part $s.$part -SyncWindow 0
    if ($d) { $ok = $false; "FAIL incremental ${n} ($($step.name)): $part differs"; $d | Select-Object -First 4 | ForEach-Object { "  $($_.SideIndicator) $($_.InputObject)" } }
  }
  if ((Get-FileHash $ir).Hash -ne (Get-FileHash $fresh).Hash) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the IR differs" }
  # The same request again, nothing changed: the server sends the kept build's output again (step
  # 3a) and, with --verify-incremental, checks it against a build from scratch.
  $r = Build @('--server', '--verify-incremental', '--emit-llvm', (Join-Path $work 'server.ll'), $src) "replay.$n"
  if ($r.exit -ne $f.exit) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the replay's exit $($r.exit), fresh $($f.exit)" }
  foreach ($part in 'out', 'err')
  {
    $d = Compare-Object $f.$part $r.$part -SyncWindow 0
    if ($d) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the replay's $part differs"; $d | Select-Object -First 4 | ForEach-Object { "  $($_.SideIndicator) $($_.InputObject)" } }
  }
  if ((Get-FileHash (Join-Path $work 'server.ll')).Hash -ne (Get-FileHash $fresh).Hash) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the IR after the replay differs" }
  $b = Build @('--server', '--quiet', '--exe', $exe, $src) "exe.$n"
  $printed = if ($b.exit -eq 0) { (& $exe) -join ' ' } else { "(build exit $($b.exit))" }
  if ($printed -ne $step.expect) { $ok = $false; "FAIL incremental ${n} ($($step.name)): printed '$printed', expected '$($step.expect)'" }
  if ($ok) { "ok   incremental $n ($($step.name))" } else { $failed++ }
}
# Each step's repeat was a replay, checked against a build from scratch (the server's log says so).
$log = Get-ChildItem (Join-Path $env:TEMP 'kppc-server\*.log') | Where-Object { $_.LastWriteTime -ge $started } |
  ForEach-Object { Get-Content $_.FullName } | Where-Object { $_ -match 'verify-incremental: the same as from scratch' -and [datetime]::ParseExact($_.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff', $null) -ge $started }
if (@($log).Count -lt $steps.Count) { "FAIL incremental: $(@($log).Count) replays verified, $($steps.Count) expected (the server's log, %TEMP%\kppc-server)"; $failed++ }
else { "ok   incremental replays ($($steps.Count), each the same as from scratch)" }
& $Kppc --stop-server --emit-llvm (Join-Path $work 'server.ll') $src | Out-Null
& $Kppc --stop-server --quiet --exe $exe $src | Out-Null
if ($failed -gt 0) { exit 1 }
