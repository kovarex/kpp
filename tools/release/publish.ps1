# Publishes kpp's packages (docs/build.md, "Continuous integration"; the store's layout:
# kppnet/registry/layout.md). Two destinations, each taken when its credential is set, so CI works
# before either is set up:
# - The forge's release (KPP_FORGE_TOKEN, -ApiUrl): the release of -Tag is replaced through the
#   forge's REST API, not a forge's own CLI or action, so the same step runs under GitHub Actions and
#   Forgejo Actions. -Forge github is GitHub's API (assets go to uploads.github.com), -Forge forgejo
#   Forgejo's and Gitea's (https://<host>/api/v1); it defaults from the URL. On GitHub a release
#   that exists is updated in place, so a step that fails leaves the old release whole: its tag is
#   moved to -Commit first, the new files are uploaded under temporary names, and only then are the
#   old files deleted and the new ones renamed. (A failed delete-then-create once left no release.)
# - kpp.net's registry store (KPP_NET_SSH_KEY, -KppNet user@host): the files go to
#   kpp/releases/<-Version>/, files already there are left alone (rsync --ignore-existing's rule), and
#   kpp/latest.json ({version, commit, zip, sha256}, zip relative to kpp/) is written last, as a
#   temporary file renamed into place. With -Mirror, each file goes to mirror/<its sha256>/<name>
#   instead and nothing else is written. Over OpenSSH's ssh and scp, which Windows has (rsync it has
#   not); KPP_NET_KNOWN_HOSTS holds the server's host key line.
#   powershell -File tools\release\publish.ps1 -Files kpp-0.1.1.zip -Commit SHA -Version 0.1.1
#     -ApiUrl URL -Repository owner/name -Tag v0.1.1 -Title T -Notes N -KppNet user@host
# -MakeLatest true makes a full release GitHub's "Latest" (releases/latest, the repository's sidebar);
# false keeps it from becoming that. A prerelease never is.
#   powershell -File tools\release\publish.ps1 -Mirror -KppNet user@host -Files SDL3-devel-3.4.18-VC.zip
param(
  [Parameter(Mandatory)][string[]]$Files,
  [string]$Commit = '',
  [string]$Version = '',
  [string]$ApiUrl = '',
  [string]$Repository = '',
  [string]$Tag = '',
  [string]$Title = '',
  [string]$Notes = '',
  [switch]$Prerelease,
  [ValidateSet('', 'true', 'false')][string]$MakeLatest = '',
  [ValidateSet('', 'github', 'forgejo')][string]$Forge = '',
  [string]$KppNet = '',
  [string]$Registry = '/home/public/registry',
  [switch]$Mirror,
  # The forge's calls are tried again after a failure GitHub's side may clear (no response, a 5xx; an
  # upload after any but 401/403/404), up to 4 times, waiting RetryWait, 3x and 6x that many seconds
  # between: 2026-11-05 two runs in a row lost their release to one 500 and one upload error
  # ("Error updating policy"). tools/release/publish-test.ps1 passes 0.
  [int]$RetryWait = 10
)
$ErrorActionPreference = 'Stop'
foreach ($f in $Files) { if (-not (Test-Path $f)) { throw "no file $f" } }

function PublishForge {
  $token = $env:KPP_FORGE_TOKEN
  $apiUrl = $ApiUrl.TrimEnd('/')
  $forge = if ($Forge) { $Forge } elseif ($apiUrl -match '/api/v1$') { 'forgejo' } else { 'github' }
  $headers = if ($forge -eq 'github') {
    @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
  } else {
    @{ Authorization = "token $token"; Accept = 'application/json' }
  }
  function Status($exception) { if ($exception.Response) { [int]$exception.Response.StatusCode } else { 0 } }
  # call, tried again after a failure (-Transient: only after no response or a 5xx; else after any but
  # 401, 403 and 404), before running first (a partial upload deleted). The attempts are said with
  # Write-Host: what a function writes to its output is its value.
  function Again([string]$what, [scriptblock]$call, [scriptblock]$before = $null, [switch]$Transient) {
    for ($attempt = 1; ; $attempt++) {
      try { return & $call }
      catch {
        $status = Status $_.Exception
        $again = if ($Transient) { $status -eq 0 -or $status -ge 500 } else { $status -notin 401, 403, 404 }
        if ($attempt -ge 4 -or -not $again) { throw }
        $wait = $RetryWait * @(1, 3, 6)[$attempt - 1]
        Write-Host "forge: $what failed ($(if ($status) { "HTTP $status" } else { 'no response' }): $($_.Exception.Message)); attempt $($attempt + 1) of 4 in $wait s"
        Start-Sleep -Seconds $wait
        if ($before) { try { & $before } catch { Write-Host "forge: before attempt $($attempt + 1) of $what`: $($_.Exception.Message)" } }
      }
    }
  }
  function ApiOnce([string]$method, [string]$path, $body) {
    $arguments = @{ Method = $method; Uri = "$apiUrl/repos/$Repository$path"; Headers = $headers }
    if ($null -ne $body) { $arguments.Body = ($body | ConvertTo-Json); $arguments.ContentType = 'application/json' }
    Invoke-RestMethod @arguments
  }
  # A DELETE whose first attempt failed may have been done: its 404 on a later attempt is success.
  function Api([string]$method, [string]$path, $body = $null) {
    $tries = @{ n = 0 }
    Again "$method $path" -Transient {
      $tries.n++
      try { ApiOnce $method $path $body }
      catch { if ($tries.n -gt 1 -and $method -eq 'DELETE' -and (Status $_.Exception) -eq 404) { return }; throw }
    }
  }
  # GitHub refuses the job's token a tag on a commit whose .github/workflows differ from the default
  # branch's now (403 "Resource not accessible by integration"): a run whose commit is older than a
  # workflow change pushed meanwhile cannot publish. The release stays as it was; the next run publishes.
  function Refused($exception, [string]$what) {
    if ((Status $exception) -eq 403) { throw "forge: $what refused (403): GitHub lets the job's token tag only a commit whose .github/workflows match the default branch's; a workflow change was pushed after $Commit. The release is unchanged; the next run publishes." }
    throw $exception
  }

  # An upload tried again (Again) after the release's asset of that name, which a failed upload may
  # leave behind half made, is deleted.
  function Upload($release, [string]$file, [string]$name) {
    Again "uploading $name" { UploadOnce $release $file $name } {
      foreach ($a in (Api GET "/releases/$($release.id)/assets")) {
        if ($a.name -eq $name) { Api DELETE "/releases/assets/$($a.id)" | Out-Null; Write-Host "forge: deleted the failed upload's $name" }
      }
    }
  }
  function UploadOnce($release, [string]$file, [string]$name) {
    if ($forge -eq 'github') {
      $upload = ($release.upload_url -replace '\{.*\}$', '') + "?name=$([uri]::EscapeDataString($name))"
      return Invoke-RestMethod -Method POST -Uri $upload -Headers $headers -ContentType 'application/octet-stream' -InFile $file
    }
    # multipart/form-data with the file as "attachment", built by hand: Windows PowerShell 5.1 has no -Form.
    $boundary = [guid]::NewGuid().ToString('N')
    $encoding = [Text.Encoding]::UTF8
    $head = $encoding.GetBytes("--$boundary`r`nContent-Disposition: form-data; name=`"attachment`"; filename=`"$name`"`r`nContent-Type: application/octet-stream`r`n`r`n")
    $tail = $encoding.GetBytes("`r`n--$boundary--`r`n")
    $content = [IO.File]::ReadAllBytes((Resolve-Path $file).Path)
    $body = New-Object byte[] ($head.Length + $content.Length + $tail.Length)
    [Array]::Copy($head, 0, $body, 0, $head.Length)
    [Array]::Copy($content, 0, $body, $head.Length, $content.Length)
    [Array]::Copy($tail, 0, $body, $head.Length + $content.Length, $tail.Length)
    return Invoke-RestMethod -Method POST -Uri "$apiUrl/repos/$Repository/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($name))" -Headers $headers -ContentType "multipart/form-data; boundary=$boundary" -Body $body
  }

  try { $old = Api GET "/releases/tags/$Tag" } catch { if ((Status $_.Exception) -ne 404) { throw } ; $old = $null }

  if ($old -and $forge -eq 'github') {
    # In place: the tag to the new commit (refused, it throws and the old release stays as it was),
    # the new files beside the old ones under temporary names, then the old ones out and the new ones
    # renamed, and last the title and notes.
    try { Api PATCH "/git/refs/tags/$Tag" @{ sha = $Commit; force = $true } | Out-Null } catch { Refused $_.Exception "moving tag $Tag" }
    Write-Output "forge: moved tag $Tag to $Commit"
    $suffix = '.new-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $added = @()
    foreach ($f in $Files) {
      $name = Split-Path -Leaf $f
      $added += [pscustomobject]@{ name = $name; asset = (Upload $old $f ($name + $suffix)) }
      Write-Output "forge: uploaded $name (as $name$suffix)"
    }
    foreach ($a in $old.assets) { Api DELETE "/releases/assets/$($a.id)" | Out-Null; Write-Output "forge: deleted old $($a.name)" }
    foreach ($a in $added) { Api PATCH "/releases/assets/$($a.asset.id)" @{ name = $a.name } | Out-Null }
    $edit = @{ name = $Title; body = $Notes; prerelease = [bool]$Prerelease }
    if ($MakeLatest) { $edit.make_latest = $MakeLatest }
    Api PATCH "/releases/$($old.id)" $edit | Out-Null
    Write-Output "forge: updated release $($old.id) ($Tag at $Commit)"
    return
  }

  # A new release (and, on Forgejo, the old one replaced: its API moves no tag).
  if ($old) { Api DELETE "/releases/$($old.id)" | Out-Null; Write-Output "forge: deleted release $($old.id)" }
  $tagPath = if ($forge -eq 'github') { "/git/refs/tags/$Tag" } else { "/tags/$Tag" }
  try { Api DELETE $tagPath | Out-Null; Write-Output "forge: deleted tag $Tag" } catch { if ((Status $_.Exception) -notin 404, 422) { throw } }

  $create = @{ tag_name = $Tag; target_commitish = $Commit; name = $Title; body = $Notes; prerelease = [bool]$Prerelease; draft = $false }
  if ($MakeLatest -and $forge -eq 'github') { $create.make_latest = $MakeLatest }
  # Not Api: a POST whose response was lost may have made the release, so an attempt after the first
  # takes the release of the tag when there is one.
  $tries = @{ n = 0 }
  try {
    $release = Again "creating release $Tag" -Transient {
      $tries.n++
      if ($tries.n -gt 1) {
        try { return ApiOnce GET "/releases/tags/$Tag" $null } catch { if ((Status $_.Exception) -ne 404) { throw } }
      }
      ApiOnce POST '/releases' $create
    }
  }
  catch { Refused $_.Exception "creating release $Tag" }
  Write-Output "forge: created release $($release.id) ($Tag at $Commit)"
  foreach ($f in $Files) {
    Upload $release $f (Split-Path -Leaf $f) | Out-Null
    Write-Output "forge: uploaded $(Split-Path -Leaf $f)"
  }
}

function PublishKppNet {
  # ssh and scp are KPP_NET_SSH_EXE and KPP_NET_SCP_EXE when set (the test's mock), else OpenSSH's.
  $ssh = if ($env:KPP_NET_SSH_EXE) { $env:KPP_NET_SSH_EXE } else { 'ssh' }
  $scp = if ($env:KPP_NET_SCP_EXE) { $env:KPP_NET_SCP_EXE } else { 'scp' }
  if (-not $env:KPP_NET_KNOWN_HOSTS) { throw 'KPP_NET_SSH_KEY is set but KPP_NET_KNOWN_HOSTS (the server''s host key line) is not' }
  $work = Join-Path ([IO.Path]::GetTempPath()) ("kppnet-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory $work | Out-Null
  try {
    $key = Join-Path $work 'key'
    $known = Join-Path $work 'known_hosts'
    [IO.File]::WriteAllText($key, ($env:KPP_NET_SSH_KEY.Trim() -replace "`r", '') + "`n")
    [IO.File]::WriteAllText($known, ($env:KPP_NET_KNOWN_HOSTS.Trim() -replace "`r", '') + "`n")
    # OpenSSH refuses a key others can read.
    icacls $key /inheritance:r /grant:r "$($env:USERNAME):R" | Out-Null
    $options = @('-i', $key, '-o', 'IdentitiesOnly=yes', '-o', "UserKnownHostsFile=$known", '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')
    function Remote([string]$command) {
      $output = & $ssh @options $KppNet $command
      if ($LASTEXITCODE -ne 0) { throw "ssh $KppNet failed ($LASTEXITCODE): $command" }
      $output
    }
    function Quote([string]$s) { "'" + $s.Replace("'", "'\''") + "'" }
    # Sends a file under a temporary name, then renames it into place: a reader never sees half a file.
    function Send([string]$local, [string]$remote, [switch]$Replace) {
      $part = "$remote.part-" + [guid]::NewGuid().ToString('N').Substring(0, 8)
      Remote "mkdir -p $(Quote (Split-Path -Parent $remote).Replace('\', '/'))" | Out-Null
      & $scp @options -q $local "${KppNet}:$part"
      if ($LASTEXITCODE -ne 0) { throw "scp $local failed ($LASTEXITCODE)" }
      $move = if ($Replace) { 'mv -f' } else { 'mv -n' }
      Remote "$move $(Quote $part) $(Quote $remote) && rm -f $(Quote $part)" | Out-Null
    }
    # rsync --ignore-existing: a file already there (a release or a mirrored file never changes) is kept.
    function SendNew([string]$local, [string]$remote) {
      if ((Remote "test -e $(Quote $remote) && echo exists || echo new") -match 'exists') { Write-Output "kpp.net: kept $remote"; return }
      Send $local $remote
      Write-Output "kpp.net: uploaded $remote"
    }
    function Sha256([string]$f) { (Get-FileHash -Algorithm SHA256 $f).Hash.ToLowerInvariant() }

    if ($Mirror) {
      foreach ($f in $Files) { SendNew $f "$Registry/mirror/$(Sha256 $f)/$(Split-Path -Leaf $f)" }
      return
    }
    if (-not $Version) { throw '-Version is needed for kpp.net' }
    foreach ($f in $Files) { SendNew $f "$Registry/kpp/releases/$Version/$(Split-Path -Leaf $f)" }
    $zip = $Files | Where-Object { $_ -like '*.zip' } | Select-Object -First 1
    if (-not $zip) { throw 'no .zip among -Files for kpp/latest.json' }
    $latest = Join-Path $work 'latest.json'
    $json = [ordered]@{ version = $Version; commit = $Commit; zip = "releases/$Version/$(Split-Path -Leaf $zip)"; sha256 = (Sha256 $zip) } | ConvertTo-Json
    [IO.File]::WriteAllText($latest, $json + "`n")
    Send $latest "$Registry/kpp/latest.json" -Replace
    Write-Output "kpp.net: latest.json is $Version"
  }
  finally { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}

if ($Mirror) {
  if (-not $env:KPP_NET_SSH_KEY) { Write-Output 'kpp.net: skipped (no KPP_NET_SSH_KEY)'; exit 0 }
  if (-not $KppNet) { throw '-KppNet user@host is needed' }
  PublishKppNet
  exit 0
}
if ($ApiUrl -and $env:KPP_FORGE_TOKEN) {
  foreach ($p in 'Repository', 'Tag', 'Commit', 'Title') { if (-not (Get-Variable $p -ValueOnly)) { throw "-$p is needed for the forge's release" } }
  PublishForge
} else { Write-Output 'forge: skipped (no -ApiUrl or no KPP_FORGE_TOKEN)' }
if ($env:KPP_NET_SSH_KEY) {
  if (-not $KppNet) { throw 'KPP_NET_SSH_KEY is set but -KppNet user@host is not' }
  PublishKppNet
} else { Write-Output 'kpp.net: skipped (no KPP_NET_SSH_KEY)' }
