# Every file convert.cmd writes ends its lines with LF alone (convert.cmd's check): the converted
# files (*.kpp, CONVERSION.md) in the checkout and in kpp\, and the shims libraries\kpp-native has from
# native\. Not the files the checkout has from Factorio unchanged (resources.cmd's data\ and libraries,
# and the two kpp-native copies, Factorio.manifest and LogitechLEDLib.h, binary in Factorio's
# repository). A file with a CR fails it: Visual Studio calls a file of both "inconsistent".
param([Parameter(Mandatory = $true)][string]$Checkout, [string[]]$Also = @())
$skip = '\\(build|bin|data|shaders|config|saves|mods|\.git)(\\|$)'
$files = New-Object System.Collections.Generic.List[string]
foreach ($root in @($Checkout) + $Also)
{
  if (-not (Test-Path $root)) { continue }
  $root = (Resolve-Path $root).Path
  Get-ChildItem -Recurse -File $root -Include *.kpp, CONVERSION.md -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName.Substring($root.Length) -notmatch $skip } | ForEach-Object { $files.Add($_.FullName) }
}
$native = Join-Path $Checkout 'libraries\kpp-native'
if (Test-Path $native)
{
  Get-ChildItem -Recurse -File $native -Include *.c, *.cpp, *.h, *.rc |
    Where-Object { $_.FullName -notmatch '\\include\\LED\\' } | ForEach-Object { $files.Add($_.FullName) }
}
$bad = New-Object System.Collections.Generic.List[string]
foreach ($f in $files)
{
  $bytes = [IO.File]::ReadAllBytes($f)
  if ([Array]::IndexOf($bytes, [byte]13) -ge 0) { $bad.Add($f) }
}
if ($bad.Count -gt 0)
{
  "line ends: $($bad.Count) of $($files.Count) files have a CR (LF alone is the rule), first:"
  $bad | Select-Object -First 10 | ForEach-Object { "  $_" }
  exit 1
}
"line ends: LF alone in all $($files.Count) files"
exit 0
