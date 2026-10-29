# The compiler's output must not depend on the thread count or on timing (docs/compile-speed.md,
# "Parallel front end"). Builds a few test programs at -j 1, -j 8 and -j 32 and compares the IR
# byte for byte and the report (timings masked); then, when programs\factorio\kpp exists (after
# convert.cmd), the headline tree's front end at -j 1, -j 8 and twice at -j 32. run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\determinism'
New-Item -ItemType Directory -Force $work | Out-Null
$failed = 0

# The report without what may differ from run to run by design: timings and rates, the thread
# count, wait counts, the number of interned types (a cache: equivalent paths taken in another
# order intern different intermediate types), and C import temp files (named with the process id).
function Normalize([string[]]$lines)
{
  $lines | Where-Object { $_ -notmatch '^check:\s+[0-9]+ distinct types' } | ForEach-Object {
    $_ -replace '[0-9]+\.[0-9]+ s\b', '_ s' -replace '\([0-9.]+ MB/s[^)]*\)', '(_)' -replace '\([0-9.]+ M lines/s\)', '(_)' `
       -replace '^threads:.*', 'threads: _' -replace '\(([0-9]+) requests, ([0-9]+) waits\)', '(_ requests, _ waits)' `
       -replace 'kpp-imports\\[0-9a-f]+\.[0-9]+\.c', 'kpp-imports\_.c'
  }
}

function Run([string[]]$arguments, [string]$label)
{
  $out = Join-Path $work "$label.out.txt"
  $err = Join-Path $work "$label.err.txt"
  $p = Start-Process -FilePath $Kppc -ArgumentList $arguments -NoNewWindow -PassThru -Wait -RedirectStandardOutput $out -RedirectStandardError $err
  return [pscustomobject]@{ label = $label; out = @(Normalize (Get-Content $out)); err = @(Normalize (Get-Content $err)) }
}

function Compare-Runs($runs, [string]$what, [string[]]$irFiles)
{
  $ok = $true
  for ($k = 1; $k -lt $runs.Count; $k++)
  {
    foreach ($part in @('out', 'err'))
    {
      $d = Compare-Object $runs[0].$part $runs[$k].$part -SyncWindow 0
      if ($d)
      {
        $ok = $false
        Write-Host "FAIL determinism ${what}: $($runs[0].label) and $($runs[$k].label) differ ($part)"
        $d | Select-Object -First 4 | ForEach-Object { Write-Host "  $($_.SideIndicator) $($_.InputObject)" }
      }
    }
    if ($irFiles -and (Get-FileHash $irFiles[0]).Hash -ne (Get-FileHash $irFiles[$k]).Hash)
    {
      $ok = $false
      Write-Host "FAIL determinism ${what}: the IR of $($runs[0].label) and $($runs[$k].label) differs"
    }
  }
  return $ok
}

foreach ($name in @('containers', 'templates', 'format'))
{
  $src = Join-Path $PSScriptRoot "run\$name.kpp"
  $runs = @(); $irs = @()
  foreach ($j in @('1', '8', '32'))
  {
    $ir = Join-Path $work "$name.j$j.ll"
    $runs += Run @('-j', $j, '--emit-llvm', $ir, $src) "$name -j $j"
    $irs += $ir
  }
  if (Compare-Runs $runs $name $irs) { "ok   determinism $name" } else { $failed++ }
}

$tree = Join-Path $PSScriptRoot '..\..\programs\factorio\kpp'
if (Test-Path (Join-Path $tree 'src'))
{
  $factorio = Join-Path $PSScriptRoot '..\..\programs\factorio'
  $common = @('--std', "$PSScriptRoot\..\..\stdlib",
    '--cinclude', 'C:\Factorio\libraries\SDL3\include', '--cinclude', 'C:\Factorio\libraries\SDL_Mixer\include',
    '--cinclude', 'C:\Factorio\libraries\Lua', '--cinclude', 'C:\Factorio\libraries', '--cinclude', 'C:\Factorio\libraries\curl\include',
    '--cinclude', 'C:\Factorio\libraries\glad\include', '--cinclude', 'C:\Factorio\libraries\trio', '--cinclude', "$factorio\native\include",
    '--errors', '0', '--unresolved', '0', "$tree\CommonUtil", "$tree\Agui", "$tree\cxxopts", "$tree\StackWalker", "$tree\Lua", "$tree\src")
  $runs = @()
  $n = 0
  foreach ($j in @('1', '8', '32', '32'))
  {
    $n++
    $runs += Run (@('-j', $j) + $common) "headline -j $j (run $n)"
  }
  if (Compare-Runs $runs "headline front end" $null) { "ok   determinism headline front end" } else { $failed++ }
}
else { "skip determinism headline front end: programs\factorio\kpp missing (run convert.cmd)" }

if ($failed -gt 0) { exit 1 }
