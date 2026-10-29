# Builds the synthetic UniquePtr and plain-pointer programs (synth_up.py) with kppc, a fresh output
# directory each run (so no object file is reused), and prints kppc's phase lines and the wall time.
# Usage: powershell -ExecutionPolicy Bypass -File run_synth.ps1 [-N 1000] [-Runs 3] [-Out DIR] [-Extra "kppc args"]
param([int]$N = 1000, [int]$Runs = 3, [string]$Out = "$env:TEMP\kpp-uniqueptr-bench", [string]$Extra = '')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kppc = Join-Path $here '..\..\build\kppc.exe'
$std = Join-Path $here '..\..\..\stdlib'
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($mode in 'unique', 'raw') {
  $dir = "$Out\synth-$mode-$N"
  if (-not (Test-Path $dir)) { python -X utf8 -I "$here\synth_up.py" $N $mode $dir }
  for ($r = 1; $r -le $Runs; $r++) {
    $build = "$Out\build-$mode-$N-$r"
    if (Test-Path $build) { Remove-Item -Recurse -Force $build }
    New-Item -ItemType Directory -Force $build | Out-Null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $arguments = @('--std', $std, '--exe', "$build\p.exe") + ($Extra -split ' ' | Where-Object { $_ }) + @($dir)
    $text = & $kppc @arguments 2>&1 | Out-String
    $wall = $sw.Elapsed.TotalSeconds
    "== $mode N=$N run $r wall {0:N3} s" -f $wall
    $text -split "`n" | Where-Object { $_ -match '^(resolve|instantiate|codegen|clang)' } | ForEach-Object { '   ' + $_.Trim() }
    if (Test-Path "$build\p.exe") { '   output: ' + (& "$build\p.exe") } else { '   NO EXE'; $text -split "`n" | Select-Object -Last 5 }
  }
}
