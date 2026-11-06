# publish's check that the artifacts it downloaded are the files their jobs made: -Sums is `NAME=SHA256 ...`,
# each NAME under -Dir, as the package and linux_host jobs wrote them. On 2026-11-06 (run 42236543487)
# download-artifact ended with no error after 50 MB of a 116 MB zip; a file cut short that still exists
# must not be released. Prints each file; exits 1 naming the ones that are missing or differ.
param([Parameter(Mandatory)][string]$Dir, [Parameter(Mandatory)][string]$Sums)
$ErrorActionPreference = 'Stop'
$entries = @($Sums -split '\s+' | Where-Object { $_ })
if (-not $entries.Count) { 'check-files: no sums given'; exit 1 }
$bad = @()
foreach ($entry in $entries)
{
  if ($entry -notmatch '^(.+)=([0-9a-f]{64})$') { "check-files: not NAME=SHA256: $entry"; exit 1 }
  $name = $Matches[1]
  $want = $Matches[2]
  $path = Join-Path $Dir $name
  if (-not (Test-Path -LiteralPath $path)) { "missing $name"; $bad += $name; continue }
  $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
  $size = (Get-Item -LiteralPath $path).Length
  if ($got -ne $want) { "differs $name ($size bytes, SHA-256 $got, not $want)"; $bad += $name; continue }
  "ok $name ($size bytes)"
}
if ($bad.Count) { "check-files: $($bad.Count) of $($entries.Count) files are not the ones built: $($bad -join ', ')"; exit 1 }
"check-files: ALL PASSED ($($entries.Count) files)"
exit 0
