# What an edit costs through the compile server against the program's first build (docs/compile-speed.md,
# "The compile server"): an edit of a function that writes many lambdas, each one's closure an instance
# of a function template, in a program whose codegen answered many functions, the error typed into it
# and taken out, and a clean edit of it, all with --check (an editor's check, kpp --lsp). Each check
# must take well under the first build: rebuildLLVM once forgot each released closure instance's name
# through every emitter's answers one name at a time, so that each such check took longer than the
# first build (the game's fmain: an error typed checked in 7 to 19 s, F's Visual Studio test,
# 2026-11-03). The median of the checks must be under 3/4 of the first build's time (it was 1.1 to 1.25
# times it; 0.21 to 0.47 since). run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\editcost'
function StopLeftovers()
{
  $root = [IO.Path]::GetFullPath($work)
  # (a process's Path is asked of the process each time it is read: one that ends between two reads gives
  # null the second time, which made this line fail the check, 2026-11-07)
  Get-Process | ForEach-Object { $path = $_.Path; if ($path -and $path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue } }
}
if (Test-Path $work) { StopLeftovers; cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force "$work\src", "$work\tmp" | Out-Null
$env:TMP = "$work\tmp"
$env:TEMP = "$work\tmp"
$std = (Resolve-Path "$PSScriptRoot\..\..\stdlib").Path
$KppExe = (Resolve-Path $KppExe).Path

# 3000 lambdas in main, each passed to call<F>; 20000 functions, each with a lambda of its own.
$helpers = New-Object Text.StringBuilder
[void]$helpers.Append("template<typename F> int call(F f) { return f() + 1; }`n")
for ($i = 0; $i -lt 20000; $i++) { [void]$helpers.Append("int helper$i(int v) { return v * $($i % 7 + 1) + call([v]() { return v + $i; }); }`n") }
$lambdas = New-Object Text.StringBuilder
for ($i = 0; $i -lt 3000; $i++) { [void]$lambdas.Append("  total += call([&total]() { return total % $($i % 13 + 2) + $i; });`n") }
$main = "int main()`n{`n  int total = 0;`n$lambdas  total += helper0(1);`n  std::println(total);`n  return 0;`n}`n"
[IO.File]::WriteAllText("$work\src\helpers.kpp", $helpers.ToString())
[IO.File]::WriteAllText("$work\src\main.kpp", $main)

$arguments = @('--check', '--std', $std, "$work\src")
function Check([string]$label, [int]$expectExit)
{
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $ErrorActionPreference = 'Continue'
  & $KppExe --server @arguments > "$work\out.txt" 2> "$work\err.txt"
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  $seconds = $watch.Elapsed.TotalSeconds
  if ($code -ne $expectExit) { throw "editcost.ps1: $label exited $code, $expectExit expected: $(Get-Content "$work\err.txt" -TotalCount 3)" }
  return $seconds
}
function Write-Main([string]$text) { [IO.File]::WriteAllText("$work\src\main.kpp", $text) }

# One measurement: a server started anew, the first build, then the eight checks; their median and
# the first build's time.
function Measure-EditCost()
{
  $ErrorActionPreference = 'Continue'
  & $KppExe --stop-server @arguments > $null 2> $null
  $ErrorActionPreference = 'Stop'
  Write-Main $main
  $first = Check 'the first build' 0
  $checks = @()
  for ($round = 0; $round -lt 2; $round++)
  {
    Write-Main $main.Replace('  total += helper0(1);', "  total += helper0(1);`n  undeclaredThing();")
    $checks += Check 'an error typed' 1
    Write-Main $main
    $checks += Check 'the error taken out' 0
    Write-Main $main.Replace('helper0(1)', 'helper0(2)')
    $checks += Check 'a clean edit' 0
    Write-Main $main
    $checks += Check 'the clean edit undone' 0
  }
  $sorted = @($checks | Sort-Object)
  return [pscustomobject]@{ First = $first; Median = ($sorted[3] + $sorted[4]) / 2; Shown = ($checks | ForEach-Object { $_.ToString('0.00') }) -join ' ' }
}

$failed = 0
try
{
  # Measured again when the first measurement fails: one first build against eight checks is at the
  # mercy of other work on the machine (a gate of another thread starting between them failed it,
  # 2026-11-06), while the slowness this check is for repeats (1.1 to 1.25 times the first build in
  # every measurement); it fails only when both measurements do.
  $m = Measure-EditCost
  if ($m.Median -ge 0.75 * $m.First)
  {
    "editcost: the checks took $($m.Median.ToString('0.00')) s (median of $($m.Shown)), the first build $($m.First.ToString('0.00')) s: measured again"
    $m = Measure-EditCost
  }
  if ($m.Median -ge 0.75 * $m.First) { "FAIL editcost: the checks after an edit take $($m.Median.ToString('0.00')) s (median of $($m.Shown)), the first build $($m.First.ToString('0.00')) s: 3/4 of it at most"; $failed++ }
  else { "ok   editcost: the checks after an edit take $($m.Median.ToString('0.00')) s (median of $($m.Shown)), the first build $($m.First.ToString('0.00')) s" }
}
finally
{
  $ErrorActionPreference = 'Continue'
  & $KppExe --stop-server @arguments > $null 2> $null
  $ErrorActionPreference = 'Stop'
  StopLeftovers
}
if ($failed) { exit 1 }
cmd /c rd /s /q "$work"
exit 0
