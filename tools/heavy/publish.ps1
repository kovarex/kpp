# Publishes C:\kpp-sandbox\heavy\status.md (heavy.ps1 writes it) to the private repository
# kovarex/pracovna-status as its README.md, for the status from other places than PRACOVNA (kovarex,
# 2026-11-23). heavy.ps1 starts this, hidden, whenever the status changes, once
# C:\kpp-sandbox\heavy\publish is a clone of that repository. One publisher at a time
# (Global\kpp-heavy-publish); a change during a push is pushed after it. The repository keeps one
# commit, replaced on every push. A failed push only leaves the published status older.
$root = 'C:\kpp-sandbox\heavy'
$clone = Join-Path $root 'publish'
$dirty = Join-Path $root 'publish.dirty'
Set-Content -Path $dirty -Value '' -ErrorAction SilentlyContinue
$mutex = New-Object System.Threading.Mutex($false, 'Global\kpp-heavy-publish')
$held = $false
try { $held = $mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $held = $true }
if (-not $held) { exit 0 } # (the running publisher sees publish.dirty)
try
{
  $log = Join-Path $root 'publish.log'
  while (Test-Path $dirty)
  {
    Start-Sleep -Seconds 5 # (a burst of changes is one push)
    Remove-Item $dirty -Force -ErrorAction SilentlyContinue
    $text = Get-Content -Raw (Join-Path $root 'status.md') -ErrorAction SilentlyContinue
    if (-not $text) { continue }
    [IO.File]::WriteAllText((Join-Path $clone 'README.md'), $text, (New-Object System.Text.UTF8Encoding($false)))
    git -C $clone add README.md 2>&1 | Out-Null
    $has = git -C $clone rev-parse --verify -q HEAD 2>$null
    if ($has) { git -C $clone commit -q --amend -m 'PRACOVNA heavy slot status' 2>&1 | Out-Null }
    else { git -C $clone commit -q -m 'PRACOVNA heavy slot status' 2>&1 | Out-Null }
    $out = git -C $clone push -q -f origin HEAD:main 2>&1
    Set-Content -Path $log -Value "$(Get-Date -Format s) push exit $LASTEXITCODE $out" -ErrorAction SilentlyContinue
  }
}
finally { $mutex.ReleaseMutex(); $mutex.Dispose() }
