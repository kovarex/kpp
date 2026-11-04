# --debug-info (docs/debugging.md): builds debuginfo\lines.kpp with line tables, runs it, and compares
# the lines of each of its functions in the PDB (llvm-pdbutil from Visual Studio's LLVM, in address
# order) with debuginfo\lines.expected, one function per row, the rows sorted; the compiler its
# objects name, kppc and kpp's version; and debuginfo\values.kpp's variables and types (below)
# with debuginfo\values.expected. run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\debuginfo'
New-Item -ItemType Directory -Force $work | Out-Null
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$pdbutil = Join-Path $vs 'VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe'
$exe = Join-Path $work 'lines.exe'
& $Kppc --quiet --debug-info --exe $exe (Join-Path $PSScriptRoot 'debuginfo\lines.kpp')
if ($LASTEXITCODE -ne 0) { "FAIL debug info: compile"; exit 1 }
$out = & $exe
if ("$out" -ne '26') { "FAIL debug info: the program printed $out"; exit 1 }
$rows = @()
$inFile = $false
foreach ($line in (& $pdbutil dump -l (Join-Path $work 'lines.pdb')))
{
  if ($line -match '^\S') { $inFile = $line -match 'debuginfo\\lines\.kpp' }
  elseif ($inFile -and $line -match '^  \S.*line/addr entries') { $rows += ,@() }
  elseif ($inFile -and $line -match '^\s+\d+ [0-9A-F]{8}') { $rows[-1] += @([regex]::Matches($line, '(\d+) [0-9A-F]{8}') | ForEach-Object { $_.Groups[1].Value }) }
}
$actual = @($rows | ForEach-Object { $_ -join ' ' } | Sort-Object)
$expected = @(Get-Content (Join-Path $PSScriptRoot 'debuginfo\lines.expected'))
$d = Compare-Object $expected $actual -SyncWindow 0
if ($d)
{
  "FAIL debug info: the line tables differ from tests\debuginfo\lines.expected"
  $actual | ForEach-Object { "  $_" }
  exit 1
}
# Each of the program's objects names its compiler (S_COMPILE3): kppc and the version `kppc --version`
# gives, without "+dirty" (docs/build.md, "Versions").
$version = if ((& $Kppc --version | Select-Object -First 1) -match '^kpp ([^,+]+)') { $Matches[1] } else { 'unknown' }
$compilers = @()
$inProgram = $false
foreach ($line in (& $pdbutil dump -symbols (Join-Path $work 'lines.pdb')))
{
  if ($line -match '^\s*Mod \d+ \| `(.*)`') { $inProgram = $Matches[1] -match 'lines\.exe\.\d+\.o$' }
  elseif ($inProgram -and $line -match 'Ver = ([^,]*), language') { $compilers += $Matches[1] }
}
$others = @($compilers | Where-Object { $_ -ne "kppc $version" })
if ($compilers.Count -eq 0 -or $others.Count -gt 0) { "FAIL debug info: the objects name their compiler '$(@($compilers | Sort-Object -Unique) -join "', '")', not 'kppc $version'"; exit 1 }

# Variables and types (docs/debugging.md, "Variables and types"): debuginfo\values.kpp built with
# --debug-info as one module and as 64 (its classes then defined in their home modules alone, every
# other module declaring them), run, and its PDB as llvm-pdbutil dumps it, summed up: inspect's
# parameters, locals and nested block with their types; each class's size, vtable pointer, bases and
# members (type, offset); the enum's underlying type and enumerators; kpp's integer typedefs (S_UDT
# int32, which a natvis rule's $T1 parses). Both builds must give debuginfo\values.expected.
$script:records = @{}
# A type as a record names it: llvm-pdbutil's name for it, or the record's (a class's, a pointer's to
# its referent's, a bit field's type and bits).
function TypeName([string]$text)
{
  if ($text -match '^(0x[0-9A-F]+) \((.+)\)$') { return $Matches[2] }
  if ($text -match '^(0x[0-9A-F]+)( \(\))?$') { $index = $Matches[1] } else { return $text }
  $lines = $script:records[$index]
  if (-not $lines) { return $text }
  $body = ($lines | Select-Object -Skip 1) -join ' '
  if ($lines[0] -match 'LF_(CLASS|STRUCTURE|UNION|ENUM) \[size = \d+\] `(.+)`') { return $Matches[2] }
  if ($lines[0] -match 'LF_POINTER' -and $body -match 'referent = (0x[0-9A-F]+(?: \([^)]*\))?), mode = (pointer|ref|rvalue ref)')
  {
    $referent = $Matches[1]
    $suffix = @{ 'pointer' = '*'; 'ref' = '&'; 'rvalue ref' = '&&' }[$Matches[2]]
    return (TypeName $referent) + $suffix
  }
  if ($lines[0] -match 'LF_MODIFIER' -and $body -match 'referent = (0x[0-9A-F]+(?: \([^)]*\))?), modifiers = (\w+)')
  {
    $modifier = $Matches[2]
    return "$modifier $(TypeName $Matches[1])"
  }
  if ($lines[0] -match 'LF_BITFIELD' -and $body -match 'type = (0x[0-9A-F]+(?: \([^)]*\))?), bit offset = (\d+), # bits = (\d+)')
  {
    $at = $Matches[2]; $bits = $Matches[3]
    return "$(TypeName $Matches[1]) bits $bits at $at"
  }
  if ($lines[0] -match 'LF_ARRAY' -and $body -match 'size: (\d+), index type: .*, element type: (0x[0-9A-F]+(?: \([^)]*\))?)')
  {
    $bytes = $Matches[1]
    return "$(TypeName $Matches[2]) array of $bytes bytes"
  }
  return $text
}
function Summary([string]$pdb)
{
  $script:records = @{}
  $current = $null
  foreach ($line in (& $pdbutil dump -types $pdb))
  {
    if ($line -match '^\s*(0x[0-9A-F]+) \| LF_') { $current = $Matches[1]; $script:records[$current] = New-Object System.Collections.Generic.List[string] }
    if ($current) { $script:records[$current].Add($line) }
  }
  $out = New-Object System.Collections.Generic.List[string]
  $symbols = @(& $pdbutil dump -symbols $pdb)
  $depth = 0
  for ($i = 0; $i -lt $symbols.Count; ++$i)
  {
    $line = $symbols[$i]
    if ($depth -eq 0)
    {
      if ($line -match 'S_GPROC32 \[size = \d+\] `(inspect\(.*\))`') { $out.Add("function $($Matches[1])"); $depth = 1 }
      continue
    }
    if ($line -match '\| S_LOCAL \[size = \d+\] `(.*)`')
    {
      $name = $Matches[1]
      if ($symbols[$i + 1] -match 'type=(0x[0-9A-F]+(?: \(.*\))?), flags = (\w+)')
      {
        $kind = if ($Matches[2] -eq 'param') { 'parameter' } else { 'local' }
        $out.Add(('  ' * $depth) + "$kind $name $(TypeName $Matches[1])")
      }
    }
    elseif ($line -match '\| S_BLOCK32') { $out.Add(('  ' * $depth) + 'block'); ++$depth }
    elseif ($line -match '\| S_END') { if (--$depth -eq 0) { break } }
  }
  foreach ($name in 'game::Point', 'game::Shape', 'game::Square', 'Flags', 'Value', 'Vector<int32>', 'game::Mode')
  {
    $pattern = 'LF_(CLASS|STRUCTURE|UNION|ENUM) \[size = \d+\] `' + [regex]::Escape($name) + '`'
    $record = @($script:records.Keys | Where-Object { $l = $script:records[$_]; $l[0] -match $pattern -and ($l -join ' ') -notmatch 'forward ref' } | Sort-Object) | Select-Object -First 1
    if (-not $record) { $out.Add("$name has no definition"); continue }
    $body = $script:records[$record] -join ' '
    if ($body -match 'underlying type: (0x[0-9A-F]+(?: \([^)]*\))?)') { $out.Add("enum $name, $(TypeName $Matches[1])") }
    elseif ($body -match 'sizeof (\d+)') { $out.Add("class $name, $($Matches[1]) bytes") }
    if ($body -notmatch 'field list: (0x[0-9A-F]+)') { continue }
    $fields = @($script:records[$Matches[1]])
    for ($k = 0; $k -lt $fields.Count; ++$k)
    {
      $f = $fields[$k]
      if ($f -match '- LF_VFUNCTAB') { $out.Add('  vtable pointer') }
      elseif ($f -match '- LF_BCLASS' -and $fields[$k + 1] -match 'type = (0x[0-9A-F]+), offset = (\d+)') { $offset = $Matches[2]; $out.Add("  base $(TypeName $Matches[1]) at $offset") }
      elseif ($f -match '- LF_MEMBER \[name = `(.*)`, Type = (0x[0-9A-F]+(?: \([^)]*\))?), offset = (\d+)') { $member = $Matches[1]; $offset = $Matches[3]; $out.Add("  member $member $(TypeName $Matches[2]) at $offset") }
      elseif ($f -match '- LF_ENUMERATE \[(.*) = (-?\d+)\]') { $out.Add("  $($Matches[1]) = $($Matches[2])") }
    }
  }
  $udts = @(& $pdbutil dump -globals $pdb | ForEach-Object { if ($_ -cmatch 'S_UDT \[size = \d+\] `(u?int(8|16|32|64))`') { $Matches[1] } } | Sort-Object -CaseSensitive -Unique)
  $out.Add("typedefs $($udts -join ' ')")
  return $out
}
$expectedValues = @(Get-Content (Join-Path $PSScriptRoot 'debuginfo\values.expected'))
foreach ($modules in '1', '64')
{
  $exe = Join-Path $work "values$modules.exe"
  # (No PDB of an earlier run: link.exe would add to it, and its records would answer for this build's.)
  Remove-Item "$work\values$modules.*" -ErrorAction SilentlyContinue
  & $Kppc --quiet --debug-info --modules $modules --exe $exe (Join-Path $PSScriptRoot 'debuginfo\values.kpp')
  if ($LASTEXITCODE -ne 0) { "FAIL debug info: values.kpp at $modules module(s): compile"; exit 1 }
  $printed = & $exe
  if ("$printed" -ne '23') { "FAIL debug info: values.kpp at $modules module(s) printed $printed"; exit 1 }
  $summary = @(Summary ([IO.Path]::ChangeExtension($exe, '.pdb')))
  if (Compare-Object $expectedValues $summary -SyncWindow 0)
  {
    "FAIL debug info: values.kpp's variables and types at $modules module(s) differ from tests\debuginfo\values.expected"
    $summary | ForEach-Object { "  $_" }
    exit 1
  }
}
"ok   debug info"
