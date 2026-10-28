# Measures kppc on the converted Factorio sources for docs/compile-speed.md. Run convert.cmd first
# (it writes kpp\). Reports, for the whole converted src: the wall time at which each driver line
# appears (the phase split), the total wall time and the peak working set; then, for the converted
# CommonUtil + Agui program (the largest converted code that compiles to an executable today), the
# debug and release builds and clang's time per module run one at a time at -O0 and -O2, fitted to
# a fixed cost per module plus a cost per KB of IR.
#   powershell -ExecutionPolicy Bypass -File programs\factorio\measure-speed.ps1 [-Runs 3]
param([int]$Runs = 3, [string]$Tree = 'kpp') # -Tree: a copy of kpp\, so that a concurrent convert.cmd does not change the input mid-run
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$kppc = (Resolve-Path ..\..\compiler\build\kppc.exe).Path
$clang = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang.exe'
$out = Join-Path $env:TEMP 'kpp-measure-speed'
New-Item -ItemType Directory -Force $out | Out-Null
$cinc = '--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\SDL_Mixer\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include'

function Run-Timed([string]$arguments, [switch]$Echo)
{
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = $kppc; $psi.Arguments = $arguments; $psi.WorkingDirectory = $PSScriptRoot
  $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $err = $p.StandardError.ReadToEndAsync()
  $peak = 0
  $next = $p.StandardOutput.ReadLineAsync()
  while ($true)
  {
    # Poll the working set while waiting for the next line of output.
    while (-not $next.Wait(20)) { try { $p.Refresh(); $peak = [math]::Max($peak, $p.PeakWorkingSet64) } catch {} }
    if ($next.Result -eq $null) { break }
    if ($Echo) { '{0,6:N2} s  {1}' -f $sw.Elapsed.TotalSeconds, $next.Result }
    try { $p.Refresh(); $peak = [math]::Max($peak, $p.PeakWorkingSet64) } catch {}
    $next = $p.StandardOutput.ReadLineAsync()
  }
  $p.WaitForExit()
  $errors = ($err.Result -split "`n" | Where-Object { $_ -match '^\S' }).Count
  '  wall {0:N2} s, peak working set {1:N0} MB, exit {2}, {3} error lines' -f $sw.Elapsed.TotalSeconds, ($peak / 1MB), $p.ExitCode, $errors
}

"== whole converted src ($Runs runs; the first prints the phase split)"
for ($i = 1; $i -le $Runs; ++$i)
{
  $script:peak = 0
  Run-Timed "--std ..\..\stdlib $cinc --errors 0 --unresolved 0 --exe $out\full.exe $Tree" -Echo:($i -eq 1)
}

$files = (Get-ChildItem $Tree\CommonUtil\*.kpp | Where-Object BaseName -ne SystemClipboard | ForEach-Object { $_.FullName }) -join ' '
$agui = "--std ..\..\stdlib $cinc --unresolved 0 {0} --exe $out\agui.exe $files tests\stubs\SystemClipboard.kpp $Tree\Agui tests\agui_smoke.kpp"
foreach ($mode in @('', '--release'))
{
  "== CommonUtil + Agui, $(if ($mode) { 'release' } else { 'debug' }) ($Runs runs, no cached objects)"
  for ($i = 1; $i -le $Runs; ++$i)
  {
    Remove-Item "$out\agui.exe*" -ErrorAction SilentlyContinue
    $script:peak = 0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $lines = & $kppc ($agui -f $mode).Split(' ', [StringSplitOptions]::RemoveEmptyEntries) 2>&1
    '  wall {0:N2} s' -f $sw.Elapsed.TotalSeconds
    $lines | Where-Object { "$_" -match '^(files|lines|codegen|clang):' } | ForEach-Object { "    $_" }
  }
}

"== clang per module, one at a time (the release build's .ll files)"
$rows = foreach ($f in Get-ChildItem "$out\agui.exe.*.ll")
{
  $o0 = (Measure-Command { & $clang -O0 -w -ffunction-sections -fdata-sections -c $f.FullName -o "$out\t0.o" }).TotalSeconds
  $o2 = (Measure-Command { & $clang -O2 -w -ffunction-sections -fdata-sections -c $f.FullName -o "$out\t2.o" }).TotalSeconds
  [pscustomobject]@{ kb = $f.Length / 1KB; o0 = $o0; o2 = $o2; defs = (Select-String -Path $f.FullName -Pattern '^define ').Count }
}
function Fit($xs, $ys)
{
  $mx = ($xs | Measure-Object -Average).Average; $my = ($ys | Measure-Object -Average).Average
  $sxy = 0; $sxx = 0
  for ($i = 0; $i -lt $xs.Count; ++$i) { $sxy += ($xs[$i] - $mx) * ($ys[$i] - $my); $sxx += ($xs[$i] - $mx) * ($xs[$i] - $mx) }
  $slope = $sxy / $sxx
  '{0:N4} s per module + {1:N3} ms per KB of IR' -f ($my - $slope * $mx), ($slope * 1000)
}
'  {0} modules, {1:N0} KB of IR, {2} function definitions (linkonce copies counted per module)' -f $rows.Count, ($rows | Measure-Object kb -Sum).Sum, ($rows | Measure-Object defs -Sum).Sum
'  -O0: {0:N2} s in all; {1}' -f ($rows | Measure-Object o0 -Sum).Sum, (Fit $rows.kb $rows.o0)
'  -O2: {0:N2} s in all; {1}' -f ($rows | Measure-Object o2 -Sum).Sum, (Fit $rows.kb $rows.o2)
