# Posts C:\kpp-sandbox\heavy\status.json (heavy.ps1 writes it) to heavyboard on kovarex.com
# (board\README.md), for the status from other places than PRACOVNA (kovarex, 2026-11-23). Its two
# lines are in C:\kpp-sandbox\heavy\publish.txt: heavyboard's address (https://.../index.cgi) and its
# password; without that file nothing is posted. heavy.ps1 starts this, hidden, whenever the status
# changes. One publisher at a time (Global\kpp-heavy-publish): a change during a post is posted after
# it, a burst of changes once. A failed post only leaves the board older (publish.log says why).
$root = 'C:\kpp-sandbox\heavy'
$config = Join-Path $root 'publish.txt'
if (-not (Test-Path $config)) { exit 0 }
$lines = @(Get-Content $config -TotalCount 2)
if ($lines.Count -lt 2 -or -not $lines[0].Trim() -or -not $lines[1].Trim()) { exit 0 }
$address = $lines[0].Trim()
$password = $lines[1].Trim()
$dirty = Join-Path $root 'publish.dirty'
Set-Content -Path $dirty -Value '' -ErrorAction SilentlyContinue
$mutex = New-Object System.Threading.Mutex($false, 'Global\kpp-heavy-publish')
$held = $false
try { $held = $mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $held = $true }
if (-not $held) { exit 0 } # (the running publisher sees publish.dirty)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
try
{
  $log = Join-Path $root 'publish.log'
  while (Test-Path $dirty)
  {
    Start-Sleep -Seconds 3
    Remove-Item $dirty -Force -ErrorAction SilentlyContinue
    $file = Join-Path $root 'status.json'
    if (-not (Test-Path $file)) { continue }
    $body = [IO.File]::ReadAllBytes($file)
    try
    {
      $answer = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$address`?update" -Headers @{ 'X-Heavy-Password' = $password } -ContentType 'application/json' -Body $body -TimeoutSec 30
      $result = "status $($answer.StatusCode)"
    }
    catch { $result = "failed: $($_.Exception.Message)" }
    Set-Content -Path $log -Value "$(Get-Date -Format s) post to $address $result" -ErrorAction SilentlyContinue
  }
}
finally { $mutex.ReleaseMutex(); $mutex.Dispose() }
