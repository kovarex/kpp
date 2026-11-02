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
#   powershell -File tools\release\settle.ps1 -ApiUrl URL -Repository owner/name -Keep 20
# The token is KPP_FORGE_TOKEN, as publish.ps1's. On Forgejo (an -ApiUrl ending in /api/v1) there is
# no "Latest" to set; old releases are deleted the same way.
param(
  [Parameter(Mandatory)][string]$ApiUrl,
  [Parameter(Mandatory)][string]$Repository,
  [int]$Keep = 20
)
$ErrorActionPreference = 'Stop'
$apiUrl = $ApiUrl.TrimEnd('/')
$github = $apiUrl -notmatch '/api/v1$'
$headers = if ($github) {
  @{ Authorization = "Bearer $env:KPP_FORGE_TOKEN"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
} else {
  @{ Authorization = "token $env:KPP_FORGE_TOKEN"; Accept = 'application/json' }
}
function Api([string]$method, [string]$path, $body = $null) {
  $arguments = @{ Method = $method; Uri = "$apiUrl/repos/$Repository$path"; Headers = $headers }
  if ($null -ne $body) { $arguments.Body = ($body | ConvertTo-Json); $arguments.ContentType = 'application/json' }
  Invoke-RestMethod @arguments
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
foreach ($old in ($pushes | Select-Object -Skip $Keep)) {
  $tag = $old.Release.tag_name
  Api DELETE "/releases/$($old.Release.id)" | Out-Null
  try {
    if ($github) { Api DELETE "/git/refs/tags/$tag" | Out-Null } else { Api DELETE "/tags/$tag" | Out-Null }
  } catch { "settle: no tag $tag" }
  "settle: deleted $tag ($($old.Version))"
}
"settle: $([Math]::Min($pushes.Count, $Keep)) push releases kept, $(@($runs).Count - $pushes.Count) other"
