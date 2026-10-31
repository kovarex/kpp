<#
The daily backup of kpp.net (RUNBOOK.md, step 6): pulls the bare repositories and the registry's
files from NFS to this machine, checks every archive's tree hash, and keeps 14 dated copies of the
index. NFS allows automated SSH once a day per site for backups; run this from a scheduled task, not
more often.

  .\pull-backup.ps1 -SshTarget kpp_kovarex@ssh.<host>.nearlyfreespeech.net -Destination D:\backup\kpp.net

Needs OpenSSH (Windows' own) and rsync (from Git for Windows' or MSYS2's usr\bin) on PATH, and the
backup key in ~\.ssh\kppnet-backup, authorized in NFS's member keychain.
#>
param(
  [Parameter(Mandatory = $true)][string]$SshTarget,
  [Parameter(Mandatory = $true)][string]$Destination,
  [string]$Key = "$HOME\.ssh\kppnet-backup"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force "$Destination\git", "$Destination\registry", "$Destination\index-history" | Out-Null
$ssh = "ssh -i `"$Key`" -o IdentitiesOnly=yes"

# One SSH session per rsync: the bare repositories, then the registry (append-only, so --ignore-existing
# for archives and a full copy of the small index).
rsync -a --delete -e $ssh "${SshTarget}:/home/private/git/" "$Destination\git\"
if ($LASTEXITCODE -ne 0) { throw "rsync of the repositories failed ($LASTEXITCODE)" }
rsync -a -e $ssh "${SshTarget}:/home/public/registry/" "$Destination\registry\"
if ($LASTEXITCODE -ne 0) { throw "rsync of the registry failed ($LASTEXITCODE)" }

$stamp = Get-Date -Format "yyyy-MM-dd"
Copy-Item -Recurse -Force "$Destination\registry\index" "$Destination\index-history\$stamp"
Get-ChildItem "$Destination\index-history" | Sort-Object Name -Descending | Select-Object -Skip 14 | Remove-Item -Recurse -Force

python -I "$PSScriptRoot\..\registry\tree_hash.py" verify "$Destination\registry"
if ($LASTEXITCODE -ne 0) { throw "an archive on kpp.net does not match its tree hash: investigate before anything else" }
Write-Host "kpp.net backup $stamp done"
