# The compile server must build exactly what a new kppc process builds (docs/compile-speed.md,
# "The compile server"). A copy of tests\incremental\ is edited step by step (a function body, a
# signature, a return type a data member is declared with, a class's layout, a file added, the file removed); after each edit it is built through
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
  # (and without the interned types' count: a cache, larger in a server that built earlier versions)
  $lines | Where-Object { $_ -notmatch '^check:\s+[0-9]+ distinct types' } | ForEach-Object { $_ -replace '[0-9]+\.[0-9]+ s\b', '_ s' -replace '\([0-9.]+ [MG]?B/s[^)]*\)', '(_)' -replace '\([0-9.]+ M lines/s\)', '(_)' -replace '\(([0-9]+) requests, ([0-9]+) waits\)', '(_ requests, _ waits)' }
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
  @{ name = 'first build'; expect = '6 40 40 3 8 10 6 5 8 40 3 8 6'; edit = {} },
  @{ name = 'body edit'; expect = '6 44 44 3 8 10 6 5 8 44 3 8 6'; edit = { Edit 'shapes.kpp' 'return v * 10;' 'return v * 11;' } },
  @{ name = 'trivial getter made two statements (callers in another file)'; expect = '7 44 44 3 8 10 6 5 8 44 3 8 6'; edit = { Edit 'shapes.kpp' 'int area() const { return width * height; }' 'int area() const { int a = height * width; return a + 1; }' } },
  @{ name = 'template body edit (its instance checked again)'; expect = '7 44 44 3 8 11 6 5 8 44 3 8 6'; edit = { Edit 'shapes.kpp' 'T r = v + v; return r;' 'T r = v + v; return r + 1;' } },
  @{ name = 'class template member body edit (its instance checked again)'; expect = '7 44 44 3 8 11 7 5 8 44 3 8 6'; edit = { Edit 'shapes.kpp' 'T r = v * 3; return r;' 'T r = v * 3; return r + 1;' } },
  @{ name = 'constexpr body edit (what the evaluator ran it for checked again)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6'; edit = { Edit 'shapes.kpp' 'int r = 5; return r;' 'int r = 6; return r;' } },
  @{ name = 'constexpr body edit failing a static_assert in another file'; expect = '(build exit 1)'; edit = { Edit 'shapes.kpp' 'int r = 6; return r;' 'int r = 12; return r;' } },
  @{ name = 'constexpr body edit back'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6'; edit = { Edit 'shapes.kpp' 'int r = 12; return r;' 'int r = 6; return r;' } },
  @{ name = 'signature edit'; expect = '7 44 44 103 8 11 7 6 8 44 3 8 6'; edit = { Edit 'shapes.kpp' 'int combine(int a, int b) { return a + b; }' 'int combine(int a, int b, int c = 100) { return a + b + c; }' } },
  @{ name = 'return type edit (a data member is declared with it)'; expect = '7 44 44 103 8 11 7 6 16 44 3 8 6'; edit = { Edit 'shapes.kpp' 'int measure()' 'long long measure()' } },
  @{ name = 'overload added'; expect = '7 44 7 103 8 11 7 6 16 7 3 8 6'; edit = { Edit 'shapes.kpp' 'int scale(int v) { return v * 11; }' "int scale(int v) { return v * 11; }`nint scale(double v) { return 7; }" } },
  @{ name = 'overload removed'; expect = '7 44 44 103 8 11 7 6 16 44 3 8 6'; edit = { Edit 'shapes.kpp' "`nint scale(double v) { return 7; }" '' } },
  @{ name = 'signature edit of a member another class shares the name of'; expect = '7 44 44 103 8 11 7 6 16 44 5 8 6'; edit = { Edit 'shapes.kpp' 'struct Gauge { int reading() const { return 3; } };' 'struct Gauge { int reading(int scale = 1) const { return 5 * scale; } };' } },
  @{ name = 'signature edit read by an instance codegen made (dropped, made anew)'; expect = '7 44 44 103 8 11 7 6 16 44 5 8 15'; edit = { Edit 'shapes.kpp' 'int base() const { return 2; }' 'int base(int k = 0) const { return 5 + k; }' } },
  @{ name = 'layout edit'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15'; edit = { Edit 'shapes.kpp' '  int height = 3;' "  int height = 3;`n  int depth = 4;" } },
  @{ name = 'new file'; expect = '7 44 1 103 12 11 7 6 16 1 5 8 15'; edit = { Set-Content (Join-Path $src 'extra.kpp') 'int scale(double v) { return 1; }' } },
  @{ name = 'file removed'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15'; edit = { Remove-Item (Join-Path $src 'extra.kpp') } }
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
  $s = Build @('--server', '--verify-incremental', '--emit-llvm', (Join-Path $work 'server.ll'), $src) "server.$n"
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
  $b = Build @('--server', '--verify-incremental', '--quiet', '--exe', $exe, $src) "exe.$n"
  $printed = if ($b.exit -eq 0) { (& $exe) -join ' ' } else { "(build exit $($b.exit))" }
  if ($printed -ne $step.expect) { $ok = $false; "FAIL incremental ${n} ($($step.name)): printed '$printed', expected '$($step.expect)'" }
  if ($ok) { "ok   incremental $n ($($step.name))" } else { $failed++ }
}
# Each step's repeat was a replay, checked against a build from scratch (the server's log says so).
# Only this run's servers' lines: other worktrees run this test at the same time, with servers of
# their own (their build directories differ), logging to the same directory.
$ours = Get-ChildItem (Join-Path $env:TEMP 'kppc-server\*.log') | Where-Object { $_.LastWriteTime -ge $started } |
  ForEach-Object { Get-Content $_.FullName } | Where-Object { $_.Length -gt 23 -and [datetime]::ParseExact($_.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff', $null) -ge $started }
$pids = @($ours | Where-Object { $_.Contains("the last $src") } | ForEach-Object { if ($_ -match '\[(\d+)\]') { $Matches[1] } } | Sort-Object -Unique)
$ours = @($ours | Where-Object { $_ -match '\[(\d+)\]' -and $pids -contains $Matches[1] })
$log = $ours | Where-Object { $_ -match 'verify-incremental: the same as from scratch' }
if (@($log).Count -lt $steps.Count) { "FAIL incremental: $(@($log).Count) replays verified, $($steps.Count) expected (the server's log, %TEMP%\kppc-server)"; $failed++ }
else { "ok   incremental replays ($($steps.Count), each the same as from scratch)" }
# The body edits (a plain function's, twice in one file, a template's, a class template member's, a
# constexpr function's) were built from the kept program, the changed bodies checked again, and verified.
$fast = $ours | Where-Object { $_ -match 'incremental: 1 file\(s\) changed \(0 with whole functions\)' }
# Codegen followed through its kept session (step 5b) on the executable's builds, verified.
$rebuilt = $ours | Where-Object { $_ -match 'codegen: [0-9]+ module\(s\) assembled again' }
if (@($rebuilt).Count -lt 3) { "FAIL incremental: codegen rebuilt from its kept session $(@($rebuilt).Count) times, expected at least 3 (the server's log)"; $failed++ }
else { "ok   incremental codegen rebuilt from its kept session ($(@($rebuilt).Count))" }
$whole = $ours | Where-Object { $_ -match 'with whole functions' -and $_ -notmatch '\(0 with whole functions\)' }
# Instances checked again because they read a change: the overload added and removed (viaScale<double>
# reads scale) and the edit of Panel::base (scaled<int>, made by codegen: dropped and made anew),
# through each of two servers; not readTwice<Meter> for the edit of Gauge::reading (lookups in a
# class are recorded with the class).
$instances = @($ours | Where-Object { $_ -match 'instance\(s\) that read a change checked again' })
if ($instances.Count -ne 6) { "FAIL incremental: $($instances.Count) builds checked instances that read a change again, 6 expected (the overload steps and Panel::base)"; $failed++ }
else { "ok   incremental instances checked again only where they read the change (6)" }
$dropped = @($ours | Where-Object { $_ -match 'made by codegen dropped' })
if ($dropped.Count -ne 2) { "FAIL incremental: $($dropped.Count) builds dropped instances codegen made, 2 expected (Panel::base through each server)"; $failed++ }
else { "ok   incremental instances codegen made dropped and made anew (2)" }
# (10: the signature edits and the overload added and removed, through each of two servers; the overload
# steps change what an instance, viaScale<double>, calls: it is checked again, not built from scratch)
if (@($whole).Count -lt 10) { "FAIL incremental: the signature edit and the overload added and removed were not built from the kept program ($(@($whole).Count) such builds, 10 expected)"; $failed++ }
else { "ok   incremental whole-function edits built from the kept program ($(@($whole).Count))" }
if (@($fast).Count -lt 10) { "FAIL incremental: $(@($fast).Count) body edits built from the kept program, 10 expected (5 through each of two servers) (the server's log, %TEMP%\kppc-server)"; $failed++ }
else { "ok   incremental body edits built from the kept program ($(@($fast).Count))" }
& $Kppc --stop-server --emit-llvm (Join-Path $work 'server.ll') $src | Out-Null
& $Kppc --stop-server --verify-incremental --quiet --exe $exe $src | Out-Null
if ($failed -gt 0) { exit 1 }
