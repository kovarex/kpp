# After ci.yml has published a run's release (docs/build.md, "Continuous integration"): makes the
# release of the newest ci run GitHub's "Latest", and deletes the push releases beyond the newest
# -Keep, each with its tag. A release's run is the "ci run <N>" its notes name; releases without one
# (release.yml's build-<N>, ones made by hand) are left alone.
# - Latest goes by run number, not by which run finished last: runs of different triggers run at
#   once, and an older run that publishes after a newer one leaves the newer one Latest.
# - A push release is tagged v<major>.<minor>.<run> (a run number, at most 7 digits); a nightly one
#   v<major>.<minor>.<date> (8 digits), which is never deleted: one durable release per day.
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
$runs = @($releases | Where-Object { -not $_.draft -and -not $_.prerelease -and $_.body -match 'ci run (\d+)' } |
  ForEach-Object { [pscustomobject]@{ Run = [int]([regex]::Match($_.body, 'ci run (\d+)').Groups[1].Value); Release = $_ } } |
  Sort-Object Run -Descending)
if (-not $runs) { 'settle: no release of a ci run'; exit 0 }

$newest = $runs[0].Release
if ($github) {
  try { $latest = Api GET '/releases/latest' } catch { $latest = $null }
  if ($latest -and $latest.id -eq $newest.id) {
    "settle: Latest is $($newest.tag_name) (run $($runs[0].Run))"
  } else {
    Api PATCH "/releases/$($newest.id)" @{ make_latest = 'true' } | Out-Null
    "settle: made $($newest.tag_name) (run $($runs[0].Run)) Latest$(if ($latest) { ", was $($latest.tag_name)" })"
  }
}

$pushes = @($runs | Where-Object { $_.Release.tag_name -match '^v\d+\.\d+\.\d{1,7}$' })
foreach ($old in ($pushes | Select-Object -Skip $Keep)) {
  $tag = $old.Release.tag_name
  Api DELETE "/releases/$($old.Release.id)" | Out-Null
  try {
    if ($github) { Api DELETE "/git/refs/tags/$tag" | Out-Null } else { Api DELETE "/tags/$tag" | Out-Null }
  } catch { "settle: no tag $tag" }
  "settle: deleted $tag (run $($old.Run))"
}
"settle: $([Math]::Min($pushes.Count, $Keep)) push releases kept, $(@($runs).Count - $pushes.Count) other"
