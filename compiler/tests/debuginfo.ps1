# --debug-info (docs/debugging.md): builds debuginfo\lines.kpp with line tables, runs it, and compares
# the lines of each of its functions in the PDB (llvm-pdbutil from Visual Studio's LLVM, in address
# order) with debuginfo\lines.expected, one function per row, the rows sorted. run_tests.cmd runs it.
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
"ok   debug info"
