# The converted files read as the program's own (convert.cmd's check, kovarex 2026-11-04):
# - no file starts with a line naming its sources (`// Converted from Entity.hpp Entity.cpp by kpp
#   --convert`): CONVERSION.md says what each file was converted from;
# - no forward declaration of a class the program defines (`class DrawingFlags;` beside
#   DrawingFlags.kpp's `class DrawingFlags : public BitFieldBase`): kpp needs none. A class declared
#   and defined nowhere stays (the tags only template arguments name, `class ElectricityTag;`), and so
#   does a template's primary that only its specializations define.
# Namespace scope only, as the converter writes it: a declaration or definition at the line's start
# (the game's namespaces do not indent their contents), the class's own name (the namespace is not
# compared, a name is defined once in the game).
param([Parameter(Mandatory = $true)][string]$Checkout)
$skip = '\\(build|bin|data|shaders|config|saves|mods|\.git)(\\|$)'
$root = (Resolve-Path $Checkout).Path
$files = Get-ChildItem -Recurse -File $root -Include *.kpp -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName.Substring($root.Length) -notmatch $skip }
$headed = New-Object System.Collections.Generic.List[string]
# (case-sensitive: Car.hpp's `class energySource;` is no EnergySource)
$declared = New-Object 'System.Collections.Generic.Dictionary[string,string]'
$defined = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($f in $files)
{
  $lines = [IO.File]::ReadAllLines($f.FullName)
  if ($lines.Count -gt 0 -and $lines[0].StartsWith('// Converted from')) { $headed.Add($f.FullName) }
  for ($i = 0; $i -lt $lines.Count; ++$i)
  {
    $line = $lines[$i]
    $template = $i -gt 0 -and $lines[$i - 1] -match '^template\s*<'
    if ($line -match '^(class|struct|union) ([A-Za-z_]\w*);\s*$')
    {
      if (-not $template) { $declared[$Matches[2]] = "$($f.FullName.Substring($root.Length + 1)):$($i + 1)" }
    }
    elseif ($line -match '^(class|struct|union) ([A-Za-z_]\w*)\b(?!.*;\s*$)')
    {
      [void]$defined.Add($Matches[2])
    }
  }
}
$forward = @($declared.Keys | Where-Object { $defined.Contains($_) } | Sort-Object)
$failed = $false
if ($headed.Count -gt 0)
{
  "converted text: $($headed.Count) of $($files.Count) files start with a `"// Converted from`" line, first:"
  $headed | Select-Object -First 5 | ForEach-Object { "  $_" }
  $failed = $true
}
if ($forward.Count -gt 0)
{
  "converted text: $($forward.Count) forward declarations of classes the program defines, first:"
  $forward | Select-Object -First 10 | ForEach-Object { "  $($declared[$_]): $_" }
  $failed = $true
}
if ($failed) { exit 1 }
"converted text: no source line and no forward declaration of a defined class in $($files.Count) files"
exit 0
