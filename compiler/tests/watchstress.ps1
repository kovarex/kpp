# The compile server's watched listing under stress (thread A's review, 2026-11-05): -Rounds times a
# source written and built through the server at once (KPP_WATCH=1, KPP_WATCH_CHECK=1: a listing on
# every build too, a difference an error), the program run and its output compared with what was
# written. With -Spin, a spinner per logical processor keeps the machine busy, at -SpinPriority
# (Normal by default: it competes with the watch's thread, which Idle spinners never do, thread A).
# One stale round fails it. Not part of run_tests (minutes); run it after a change to collectFilesWatched.
#   powershell -File compiler\tests\watchstress.ps1 [-Rounds 500] [-Spin] [-SpinPriority Normal]
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [int]$Rounds = 500, [switch]$Spin, [string]$SpinPriority = 'Normal')
$ErrorActionPreference = 'Stop'
$Kppc = (Resolve-Path $Kppc).Path
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\watchstress'))
function StopServer { $ErrorActionPreference = 'Continue'; & $Kppc --stop-server --quiet --exe "$work\w.exe" "$work\src" 2>&1 | Out-Null }
if (Test-Path $work) { StopServer; cmd.exe /c rd /s /q $work }
New-Item -ItemType Directory -Force "$work\src" | Out-Null
$env:KPP_WATCH_CHECK = '1'
$spinners = @()
if ($Spin)
{
  foreach ($i in 1..[Environment]::ProcessorCount)
  {
    # Each spinner ends by itself once this script's process is gone or after 15 minutes, so that a
    # run stopped from outside (no finally) leaves no processor spinning (2026-11-05: 32 were left).
    $spinCommand = "`$end = (Get-Date).AddMinutes(15); while ((Get-Date) -lt `$end -and (Get-Process -Id $PID -ErrorAction SilentlyContinue)) { for (`$i = 0; `$i -lt 3000000; `$i++) { } }"
    $p = Start-Process powershell -ArgumentList '-NoProfile', '-Command', $spinCommand -PassThru -WindowStyle Hidden
    try { $p.PriorityClass = $SpinPriority } catch { }
    $spinners += $p
  }
}
$failed = 0
$main = "$work\src\main.kpp"
try
{
  for ($r = 1; $r -le $Rounds; $r++)
  {
    # Another file of a different size or the same size each round (a stamp that differs only in time).
    [IO.File]::WriteAllText($main, "int main() { println($(1000 + $r)); return 0; }")
    $ErrorActionPreference = 'Continue'
    $out = & $Kppc --server --quiet --exe "$work\w.exe" "$work\src" 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $printed = if ($code -eq 0) { (& "$work\w.exe") -join ' ' } else { "exit $code" }
    if ($printed -ne "$(1000 + $r)")
    {
      $failed++
      "FAIL watchstress round ${r}: wrote $(1000 + $r), the server's build printed $printed`n$($out -join "`n")"
      if ($failed -ge 5) { break }
    }
  }
}
finally
{
  foreach ($p in $spinners) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  StopServer
}
if ($failed -eq 0) { "ok   watchstress: $Rounds rounds of a write and a server build at once$(if ($Spin) { ", every processor spinning ($SpinPriority priority)" }), none stale" }
exit $failed
