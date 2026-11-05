# The natvis of a --debug-info build (docs/debugging.md, "Values in Visual Studio"): builds
# natvis\values.kpp, runs it, and checks that kppc warned of no rule a library change broke, that
# the .natvis is valid by Visual Studio's natvis.xsd, that the link put it into the PDB (a named
# stream /src/files/PATH, PATH the .natvis's, lower case; llvm-pdbutil) and that its rules are the ones natvis\values.expected
# lists. run_tests.cmd runs it. -Update writes values.expected from this run.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [switch]$Update)
$ErrorActionPreference = 'Stop'
$Kppc = (Resolve-Path $Kppc).Path # (the project's build runs in its own directory)
$work = Join-Path $PSScriptRoot '..\build\tests\natvis'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$pdbutil = Join-Path $vs 'VC\Tools\Llvm\x64\bin\llvm-pdbutil.exe'
$xsd = Join-Path $vs 'Xml\Schemas\1033\natvis.xsd'
$exe = Join-Path $work 'values.exe'
$natvis = Join-Path $work 'values.natvis'
Remove-Item $natvis -ErrorAction SilentlyContinue

$ErrorActionPreference = 'Continue'
$log = & $Kppc --quiet --debug-info --exe $exe (Join-Path $PSScriptRoot 'natvis\values.kpp') 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0) { "FAIL natvis: compile (exit $code)"; $log | Select-Object -Last 5 | ForEach-Object { "  $_" }; exit 1 }
$warnings = @($log | Where-Object { $_ -match 'warning: natvis' })
if ($warnings) { "FAIL natvis: kppc left rules out"; $warnings | ForEach-Object { "  $_" }; exit 1 }
$out = & $exe
if ("$out" -ne '2 kpp 51 2 1 5 false 4 12 gear 40 -20 2 9 3 1 3 2 false 6') { "FAIL natvis: the program printed $out"; exit 1 }
if (-not (Test-Path $natvis)) { "FAIL natvis: no $natvis"; exit 1 }

# Visual Studio's schema.
$settings = New-Object System.Xml.XmlReaderSettings
$settings.Schemas.Add('http://schemas.microsoft.com/vstudio/debugger/natvis/2010', $xsd) | Out-Null
$settings.ValidationType = 'Schema'
$script:invalid = @()
$settings.add_ValidationEventHandler({ param($sender, $e) $script:invalid += "line $($e.Exception.LineNumber): $($e.Message)" })
try
{
  $reader = [System.Xml.XmlReader]::Create($natvis, $settings)
  while ($reader.Read()) { }
  $reader.Close()
}
catch { $script:invalid += $_.Exception.Message }
if ($script:invalid) { "FAIL natvis: not valid by natvis.xsd"; $script:invalid | Select-Object -First 5 | ForEach-Object { "  $_" }; exit 1 }

# In the PDB, at the file's size.
$size = (Get-Item $natvis).Length
$streams = (& $pdbutil dump --named-streams (Join-Path $work 'values.pdb')) -join "`n"
if ($streams -notmatch "/src/files/\S*values\.natvis\s+Index: \d+\s+Size in bytes: $size\b") { "FAIL natvis: the PDB has no /src/files/...values.natvis of $size bytes"; exit 1 }

# Its rules.
$names = @(([xml](Get-Content $natvis -Raw)).AutoVisualizer.Type | ForEach-Object { $_.Name })
$expectedPath = Join-Path $PSScriptRoot 'natvis\values.expected'
if ($Update) { [IO.File]::WriteAllLines($expectedPath, [string[]]$names); "natvis.ps1: wrote $expectedPath"; exit 0 }
$d = Compare-Object @(Get-Content $expectedPath) $names -SyncWindow 0
if ($d) { "FAIL natvis: its rules differ from natvis\values.expected"; $names | ForEach-Object { "  $_" }; exit 1 }

# A program's own .natvis (build.kpp's Executable::natvis, kppc's --natvis): natvis\project builds
# values.kpp with point.natvis; its debug build's PDB holds both that file and the one kppc writes.
$project = Join-Path $work 'project'
if (Test-Path $project) { cmd /c rd /s /q "$project" }
New-Item -ItemType Directory -Force $project | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'natvis\project\*') $project -Force
Copy-Item (Join-Path $PSScriptRoot 'natvis\values.kpp') $work
Push-Location $project
$ErrorActionPreference = 'Continue'
$log = & $Kppc values debug --debug-info --no-server 2>&1 # (no compile server left holding the directory) | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Pop-Location
if ($code -ne 0) { "FAIL natvis: the project with its own .natvis does not build (exit $code)"; $log | Select-Object -Last 5 | ForEach-Object { "  $_" }; exit 1 }
$pdb = Get-ChildItem $project -Recurse -Filter 'values.pdb' | Select-Object -First 1
$streams = if ($pdb) { (& $pdbutil dump --named-streams $pdb.FullName) -join "`n" } else { '' }
if ($streams -notmatch '/src/files/\S*point\.natvis' -or $streams -notmatch '/src/files/\S*values\.natvis')
{ "FAIL natvis: the project's PDB lacks point.natvis or values.natvis"; exit 1 }
"ok   natvis ($($names.Count) rules, $size bytes, in the PDB; a project's own .natvis beside it)"
exit 0
