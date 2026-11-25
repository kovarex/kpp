# After ci.yml has published a run's release (docs/build.md, "Continuous integration"): makes the
# release of the highest version GitHub's "Latest", and deletes the push releases beyond the newest
# -Keep, each with its tag. A release's version is the one its title names ("kpp 0.2.5"; older releases capitalise it, which is read too; kpp's
# version: docs/build.md, "Versions"); prereleases and drafts (release.yml's build-<N>) are left alone.
# - Latest goes by version, compared as numbers (0.2.0 above 0.1.148), not by which run finished
#   last: runs of different triggers run at once, and an older commit's run that publishes after a
#   newer one leaves the newer one Latest. Of a push and a nightly release of one version, the push
#   one (v<version>) is Latest.
# - A push release is tagged v<major>.<minor>.<patch>; a nightly one nightly-<date> (and, before
#   kpp's versions, v0.1.<date>: a patch of 8 digits), which is never deleted: one durable release
#   per day.
# - A push release keep.txt names (one a known project's CI pins) is never deleted.
# - A call the forge's side fails (no response, a 5xx) is tried again, as publish.ps1's are
#   (-RetryWait). An old release it still has not deleted then is left to the next run, said in a
#   line, and this run is not failed for it; any other failure fails it.
#   powershell -File tools\release\settle.ps1 -ApiUrl URL -Repository owner/name -Keep 50
# The token is KPP_FORGE_TOKEN, as publish.ps1's. On Forgejo (an -ApiUrl ending in /api/v1) there is
# no "Latest" to set; old releases are deleted the same way.
param(
  [Parameter(Mandatory)][string]$ApiUrl,
  [Parameter(Mandatory)][string]$Repository,
  [int]$Keep = 20,
  # A call that fails on the forge's side (no response, a 5xx) is tried again, up to 4 times, waiting
  # RetryWait, 3x and 6x that many seconds between, as publish.ps1's calls are. Run 45644587269 went red
  # on one "Server Error" to the DELETE of the 51st release (2026-11-25, an outage of GitHub's), its own
  # release published and Latest. tools/release/settle-test.ps1 passes 0.
  [int]$RetryWait = 10
)
$ErrorActionPreference = 'Stop'
$apiUrl = $ApiUrl.TrimEnd('/')
$github = $apiUrl -notmatch '/api/v1$'
$headers = if ($github) {
  @{ Authorization = "Bearer $env:KPP_FORGE_TOKEN"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
} else {
  @{ Authorization = "token $env:KPP_FORGE_TOKEN"; Accept = 'application/json' }
}
# A failed call's HTTP status; 0 when there was no response.
function Status($exception) {
  if ($exception.Response) { [int]$exception.Response.StatusCode } else { 0 }
}
function ApiOnce([string]$method, [string]$path, $body) {
  $arguments = @{ Method = $method; Uri = "$apiUrl/repos/$Repository$path"; Headers = $headers }
  if ($null -ne $body) { $arguments.Body = ($body | ConvertTo-Json); $arguments.ContentType = 'application/json' }
  Invoke-RestMethod @arguments
}
# A call, tried again after no response or a 5xx (the attempts are said with Write-Host: what a
# function writes to its output is its value). A DELETE whose earlier attempt failed may have been
# done: its 404 on a later attempt is success.
function Api([string]$method, [string]$path, $body = $null) {
  for ($attempt = 1; ; $attempt++) {
    try { return ApiOnce $method $path $body }
    catch {
      $status = Status $_.Exception
      if ($attempt -gt 1 -and $method -eq 'DELETE' -and $status -eq 404) { return }
      if ($attempt -ge 4 -or -not ($status -eq 0 -or $status -ge 500)) { throw }
      $wait = $RetryWait * @(1, 3, 6)[$attempt - 1]
      Write-Host "settle: $method $path failed ($(if ($status) { "HTTP $status" } else { 'no response' }): $($_.Exception.Message)); attempt $($attempt + 1) of 4 in $wait s"
      Start-Sleep -Seconds $wait
    }
  }
}

$releases = @()
for ($page = 1; ; ++$page) {
  # Windows PowerShell's Invoke-RestMethod returns a JSON array as one object; ForEach-Object unrolls it.
  $batch = @(Api GET "/releases?per_page=100&page=$page&limit=50" | ForEach-Object { $_ })
  $releases += $batch
  if ($batch.Count -lt 50) { break }
}
$runs = @($releases | Where-Object { -not $_.draft -and -not $_.prerelease -and $_.name -match '^kpp (\d+\.\d+\.\d+)\b' } |
  ForEach-Object { [pscustomobject]@{ Version = [version]([regex]::Match($_.name, '(?i)^kpp (\d+\.\d+\.\d+)').Groups[1].Value); Push = $_.tag_name -like 'v*'; Release = $_ } } |
  Sort-Object Version, Push -Descending)
if (-not $runs) { 'settle: no release with a version'; exit 0 }

$newest = $runs[0].Release
if ($github) {
  try { $latest = Api GET '/releases/latest' } catch { $latest = $null }
  if ($latest -and $latest.id -eq $newest.id) {
    "settle: Latest is $($newest.tag_name) ($($runs[0].Version))"
  } else {
    Api PATCH "/releases/$($newest.id)" @{ make_latest = 'true' } | Out-Null
    "settle: made $($newest.tag_name) ($($runs[0].Version)) Latest$(if ($latest) { ", was $($latest.tag_name)" })"
  }
}

$pushes = @($runs | Where-Object { $_.Release.tag_name -match '^v\d+\.\d+\.\d{1,7}$' })
# keep.txt's tags (the releases known projects' CI pins: gorating's broke on 2026-11-05 when its pin,
# v0.2.242, was deleted) are never deleted, and do not count in -Keep.
# (a keep.txt that is not there stops it before anything is deleted: it would pin nothing)
$keepFile = Join-Path $PSScriptRoot 'keep.txt'
if (-not (Test-Path $keepFile)) { throw "settle: no $keepFile, so nothing is deleted (its releases would go)" }
$pinned = @(Get-Content $keepFile | ForEach-Object { ($_ -replace '#.*', '').Trim() } | Where-Object { $_ })
foreach ($tag in $pinned) { if (-not ($pushes | Where-Object { $_.Release.tag_name -eq $tag })) { "settle: keep.txt names $tag, which is not a push release here" } }
$pinnedHere = @($pushes | Where-Object { $pinned -contains $_.Release.tag_name }).Count
$pushes = @($pushes | Where-Object { $pinned -notcontains $_.Release.tag_name })
$left = 0
foreach ($old in ($pushes | Select-Object -Skip $Keep)) {
  $tag = $old.Release.tag_name
  try { Api DELETE "/releases/$($old.Release.id)" | Out-Null }
  catch {
    # An old release the forge's side did not delete in four attempts (no response, a 5xx) stays for
    # the next run, which deletes whatever is beyond -Keep then: this run's own release is published
    # and Latest by now, and its run is not failed for another release's cleanup. Any other answer
    # (a 403: the token may not delete) is this script's failure.
    $status = Status $_.Exception
    if ($status -ne 0 -and $status -lt 500) { throw }
    "settle: $tag ($($old.Version)) not deleted ($(if ($status) { "HTTP $status" } else { 'no response' }), 4 attempts): left to the next run"
    $left++
    continue
  }
  try {
    if ($github) { Api DELETE "/git/refs/tags/$tag" | Out-Null } else { Api DELETE "/tags/$tag" | Out-Null }
  } catch { "settle: no tag $tag" }
  "settle: deleted $tag ($($old.Version))"
}
"settle: $([Math]::Min($pushes.Count, $Keep)) push releases kept and $pinnedHere of keep.txt, $(@($runs).Count - $pushes.Count - $pinnedHere) other$(if ($left) { ", $left left to the next run" })"
