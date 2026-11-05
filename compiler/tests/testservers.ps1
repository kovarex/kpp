# Dot-sourced by the checks that start compile servers in a directory of their own (project.ps1,
# generate.ps1): Stop-TestServers ROOT stops the servers whose KEY.args (in %TEMP%\kppc-server,
# server.cpp) starts with a working directory under ROOT, each through its process, KEY.exe. A check
# that failed partway left its servers running, holding ROOT, which the next run could not delete
# (thread B, 2026-11-04; D, generate.ps1, 2026-11-05); never kpp --stop-all-servers, which would stop
# other threads' servers too.
function Stop-TestServers([string]$root)
{
  $root = [IO.Path]::GetFullPath($root)
  $dir = Join-Path $env:TEMP 'kppc-server'
  foreach ($record in @(Get-ChildItem (Join-Path $dir '*.args') -ErrorAction SilentlyContinue))
  {
    # (one another thread's client is writing as its server starts cannot be read: not this test's,
    # whose own are written by then; it stopped the whole test once, the gate of 2026-11-05)
    try { $cwd = ([IO.File]::ReadAllText($record.FullName) -split "`0")[0] } catch { continue }
    if (-not $cwd.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { continue }
    $exe = Join-Path $dir ($record.BaseName + '.exe')
    Get-Process | Where-Object { $_.Path -and $_.Path -ieq $exe } | ForEach-Object { Stop-Process -Id $_.Id -Force; $_.WaitForExit(10000) | Out-Null }
  }
}
