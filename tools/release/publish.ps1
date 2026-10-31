# Publishes files as a release on the forge that hosts the repository, replacing a release of the same
# tag (docs/build.md, "Continuous integration"). It uses each forge's REST API, not a forge's own
# CLI or action, so the same step runs under GitHub Actions and Forgejo Actions:
#   -Forge github   https://api.github.com           (GitHub's API; assets go to uploads.github.com)
#   -Forge forgejo  https://<host>/api/v1            (Forgejo's and Gitea's API)
# In a workflow, pass -ApiUrl "${{ github.api_url }}" -Repository "${{ github.repository }}" and the
# job's token in the environment variable KPP_FORGE_TOKEN; -Forge defaults from the API URL.
#   pwsh -File tools\release\publish.ps1 -ApiUrl URL -Repository owner/name -Tag latest -Commit SHA
#        -Title T -Notes N -Prerelease -Files build\release\kpp-0.1.0-ci.1.zip
param(
  [Parameter(Mandatory)][string]$ApiUrl,
  [Parameter(Mandatory)][string]$Repository,
  [Parameter(Mandatory)][string]$Tag,
  [Parameter(Mandatory)][string]$Commit,
  [Parameter(Mandatory)][string]$Title,
  [string]$Notes = '',
  [switch]$Prerelease,
  [Parameter(Mandatory)][string[]]$Files,
  [ValidateSet('', 'github', 'forgejo')][string]$Forge = ''
)
$ErrorActionPreference = 'Stop'
$token = $env:KPP_FORGE_TOKEN
if (-not $token) { throw 'KPP_FORGE_TOKEN is not set' }
$ApiUrl = $ApiUrl.TrimEnd('/')
if (-not $Forge) { $Forge = if ($ApiUrl -match '/api/v1$') { 'forgejo' } else { 'github' } }
foreach ($f in $Files) { if (-not (Test-Path $f)) { throw "no file $f" } }

$headers = if ($Forge -eq 'github') {
  @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
} else {
  @{ Authorization = "token $token"; Accept = 'application/json' }
}
function Api([string]$method, [string]$path, $body = $null) {
  $arguments = @{ Method = $method; Uri = "$ApiUrl/repos/$Repository$path"; Headers = $headers }
  if ($null -ne $body) { $arguments.Body = ($body | ConvertTo-Json); $arguments.ContentType = 'application/json' }
  Invoke-RestMethod @arguments
}
function Status($exception) { if ($exception.Response) { [int]$exception.Response.StatusCode } else { 0 } }

# The old release and its tag, when there are any.
try { $old = Api GET "/releases/tags/$Tag" } catch { if ((Status $_.Exception) -ne 404) { throw } ; $old = $null }
if ($old) { Api DELETE "/releases/$($old.id)" | Out-Null; Write-Output "deleted release $($old.id)" }
$tagPath = if ($Forge -eq 'github') { "/git/refs/tags/$Tag" } else { "/tags/$Tag" }
try { Api DELETE $tagPath | Out-Null; Write-Output "deleted tag $Tag" } catch { if ((Status $_.Exception) -notin 404, 422) { throw } }

$release = Api POST '/releases' @{ tag_name = $Tag; target_commitish = $Commit; name = $Title; body = $Notes; prerelease = [bool]$Prerelease; draft = $false }
Write-Output "created release $($release.id) ($Tag at $Commit)"

foreach ($f in $Files) {
  $name = Split-Path -Leaf $f
  if ($Forge -eq 'github') {
    $upload = ($release.upload_url -replace '\{.*\}$', '') + "?name=$([uri]::EscapeDataString($name))"
    Invoke-RestMethod -Method POST -Uri $upload -Headers $headers -ContentType 'application/octet-stream' -InFile $f | Out-Null
  } else {
    # multipart/form-data with the file as "attachment", built by hand: Windows PowerShell 5.1 has no -Form.
    $boundary = [guid]::NewGuid().ToString('N')
    $encoding = [Text.Encoding]::UTF8
    $head = $encoding.GetBytes("--$boundary`r`nContent-Disposition: form-data; name=`"attachment`"; filename=`"$name`"`r`nContent-Type: application/octet-stream`r`n`r`n")
    $tail = $encoding.GetBytes("`r`n--$boundary--`r`n")
    $content = [IO.File]::ReadAllBytes((Resolve-Path $f).Path)
    $body = New-Object byte[] ($head.Length + $content.Length + $tail.Length)
    [Array]::Copy($head, 0, $body, 0, $head.Length)
    [Array]::Copy($content, 0, $body, $head.Length, $content.Length)
    [Array]::Copy($tail, 0, $body, $head.Length + $content.Length, $tail.Length)
    Invoke-RestMethod -Method POST -Uri "$ApiUrl/repos/$Repository/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($name))" -Headers $headers -ContentType "multipart/form-data; boundary=$boundary" -Body $body | Out-Null
  }
  Write-Output "uploaded $name"
}
