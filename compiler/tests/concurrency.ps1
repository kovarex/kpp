# The compiler's output must not depend on timing under load either: builds a few programs heavy in
# virtual calls, vtables and exceptions at -j 32 many times, 16 processes at once, and compares every
# IR byte for byte with the program's -j 1 build. The thread-count check (determinism.ps1) runs each
# build once; a race in codegen's sharing between threads that shows in 1 or 2 builds of 100 (a
# layout published before its vtables were made: a virtual call emitted as a direct one,
# docs/compile-speed.md, "Codegen's scaling") needs hundreds. run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [int]$Copies = 160)
$ErrorActionPreference = 'Stop'
$Kppc = (Resolve-Path $Kppc).Path # (the processes start in .NET's current directory, not PowerShell's)
$work = Join-Path $PSScriptRoot '..\build\tests\concurrency'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
$programs = @('run\exceptions.kpp', 'run\downcast.kpp', 'run\containers.kpp', 'spec\classes\catch_by_second_base.kpp',
  'spec\library\enable_shared_from_this.kpp', 'spec\classes\brace_init_assign.kpp')

function Start-Build([string]$src, [string]$ir, [string]$j)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $Kppc
  $info.Arguments = "--quiet -j $j --emit-llvm `"$ir`" `"$src`""
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $false
  $info.RedirectStandardError = $false
  return [System.Diagnostics.Process]::Start($info)
}

$failed = 0
$reference = @{}
$n = 0
foreach ($p in $programs)
{
  $n++
  $ir = Join-Path $work "p$n.reference.ll"
  $proc = Start-Build (Join-Path $PSScriptRoot $p) $ir '1'
  $proc.WaitForExit()
  if ($proc.ExitCode -ne 0 -or -not (Test-Path $ir)) { Write-Host "FAIL concurrency: $p does not build (exit $($proc.ExitCode))"; exit 1 }
  $reference[$n] = (Get-FileHash $ir).Hash
}

# Every program's copies interleaved, 16 processes running at a time.
$queue = New-Object System.Collections.Generic.Queue[object]
for ($c = 1; $c -le $Copies; $c++)
{
  for ($k = 1; $k -le $programs.Count; $k++) { $queue.Enqueue(@($k, $c)) }
}
$running = New-Object System.Collections.Generic.List[object]
$done = New-Object System.Collections.Generic.List[object]
while ($queue.Count -gt 0 -or $running.Count -gt 0)
{
  while ($queue.Count -gt 0 -and $running.Count -lt 16)
  {
    $k, $c = $queue.Dequeue()
    $ir = Join-Path $work "p$k.c$c.ll"
    $running.Add(@((Start-Build (Join-Path $PSScriptRoot $programs[$k - 1]) $ir '32'), $k, $ir))
  }
  $finished = @($running | Where-Object { $_[0].HasExited })
  if ($finished.Count -eq 0) { Start-Sleep -Milliseconds 5; continue }
  foreach ($r in $finished) { [void]$running.Remove($r); $done.Add($r) }
}
$differ = @{}
foreach ($r in $done)
{
  $proc, $k, $ir = $r
  if ($proc.ExitCode -ne 0 -or -not (Test-Path $ir) -or (Get-FileHash $ir).Hash -ne $reference[$k])
  {
    $differ[$k] = 1 + [int]$differ[$k]
  }
  else { Remove-Item $ir }
}
for ($k = 1; $k -le $programs.Count; $k++)
{
  if ($differ[$k])
  {
    $failed++
    Write-Host "FAIL concurrency $($programs[$k - 1]): $($differ[$k]) of $Copies builds at -j 32 under load differ from -j 1 (kept in $work)"
  }
}
if ($failed -gt 0) { exit 1 }
"ok   concurrency: $($programs.Count) programs built $Copies times each at -j 32 under load, the IR always the -j 1 build's"
