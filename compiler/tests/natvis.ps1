# The natvis of a --debug-info build (docs/debugging.md, "Values in Visual Studio"): builds
# natvis\values.kpp, runs it, and checks that kpp warned of no rule a library change broke, that
# the .natvis is valid by Visual Studio's natvis.xsd, that the link put it into the PDB (a named
# stream /src/files/PATH, PATH the .natvis's, lower case; llvm-pdbutil) and that its rules are the ones natvis\values.expected
# lists. run_tests.cmd runs it. -Update writes values.expected from this run.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe", [switch]$Update)
$ErrorActionPreference = 'Stop'
$KppExe = (Resolve-Path $KppExe).Path # (the project's build runs in its own directory)
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
$log = & $KppExe --quiet --debug-info --exe $exe (Join-Path $PSScriptRoot 'natvis\values.kpp') 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0) { "FAIL natvis: compile (exit $code)"; $log | Select-Object -Last 5 | ForEach-Object { "  $_" }; exit 1 }
$warnings = @($log | Where-Object { $_ -match 'warning: natvis' })
if ($warnings) { "FAIL natvis: kpp left rules out"; $warnings | ForEach-Object { "  $_" }; exit 1 }
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

# debugValue() (docs/debugging.md, "debugValue"): Fixed<8>'s rule computes its value with its own
# scale, Item's shows its name and expands to it; a body the debugger cannot compute (a call, a
# non-const debugValue) is the build's error, naming the file and line.
$types = @(([xml](Get-Content $natvis -Raw)).AutoVisualizer.Type)
$fixed = @($types | Where-Object { $_.Name -eq 'Fixed<8>' })[0]
$item = @($types | Where-Object { $_.Name -eq 'Item' })[0]
if (-not $fixed -or $fixed.DisplayString -ne '{(double)(value) * 0.00390625,g}') { "FAIL natvis: Fixed<8>'s debugValue rule is '$($fixed.DisplayString)'"; exit 1 }
if (-not $item -or $item.DisplayString -ne '{name}' -or $item.Expand.ExpandedItem -ne 'name') { "FAIL natvis: Item's debugValue rule is '$($item.DisplayString)'"; exit 1 }
$load = @($types | Where-Object { $_.Name -eq 'Load' })[0]
$shown = @($load.ChildNodes | Where-Object { $_.LocalName -eq 'DisplayString' } | ForEach-Object { "$($_.GetAttribute('Condition'))|$($_.InnerText)" }) -join ' '
if ($shown -ne 'empty|{0,g} |{((double)(weight.value) * 0.000244140625) * 2,g}') { "FAIL natvis: Load's debugValue rule is '$shown'"; exit 1 }
$buffer = @($types | Where-Object { $_.Name -eq 'Buffer' })[0]
if ($buffer.DisplayString -ne '{{length={size}}}' -or $buffer.Expand.ArrayItems.Size -ne 'size' -or $buffer.Expand.ArrayItems.ValuePointer -ne 'data')
{ "FAIL natvis: Buffer's debugValue rule is '$($buffer.DisplayString)'"; exit 1 }
$bad = Join-Path $work 'debugvalue-errors.kpp'
[IO.File]::WriteAllText($bad, "struct Counted`n{`n  i32 count() const { return this.n; }`n  i32 debugValue() const { return this.count(); }`n  i32 n = 0;`n};`n`n" +
  "struct Moving`n{`n  i32 debugValue() { return this.speed; }`n  i32 speed = 0;`n};`n`nint main()`n{`n  Counted c;`n  Moving m;`n  return c.n + m.speed;`n}`n")
$ErrorActionPreference = 'Continue'
$log = & $KppExe --quiet --debug-info --exe (Join-Path $work 'debugvalue-errors.exe') $bad 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$log = $log -join "`n"
if ($code -eq 0 -or $log -notmatch 'debugvalue-errors\.kpp:4:\d+: error: debugValue of Counted cannot be shown by the debugger: a call' -or
    $log -notmatch 'debugvalue-errors\.kpp:10:\d+: error: debugValue, which the debugger shows Moving as, is a const member function without parameters')
{ "FAIL natvis: debugValue bodies the debugger cannot compute: exit $code`n$log"; exit 1 }

# A program's own .natvis (build.kpp's Executable::natvis, kpp's --natvis): natvis\project builds
# values.kpp with point.natvis; its debug build's PDB holds both that file and the one kpp writes.
$project = Join-Path $work 'project'
if (Test-Path $project) { cmd /c rd /s /q "$project" }
New-Item -ItemType Directory -Force $project | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'natvis\project\*') $project -Force
Copy-Item (Join-Path $PSScriptRoot 'natvis\values.kpp') $work
Push-Location $project
$ErrorActionPreference = 'Continue'
$log = & $KppExe values debug --debug-info --no-server 2>&1 # (no compile server left holding the directory) | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Pop-Location
if ($code -ne 0) { "FAIL natvis: the project with its own .natvis does not build (exit $code)"; $log | Select-Object -Last 5 | ForEach-Object { "  $_" }; exit 1 }
$pdb = Get-ChildItem $project -Recurse -Filter 'values.pdb' | Select-Object -First 1
$streams = if ($pdb) { (& $pdbutil dump --named-streams $pdb.FullName) -join "`n" } else { '' }
if ($streams -notmatch '/src/files/\S*point\.natvis' -or $streams -notmatch '/src/files/\S*values\.natvis')
{ "FAIL natvis: the project's PDB lacks point.natvis or values.natvis"; exit 1 }
# Through a compile server: an edit of that .natvis alone is a new build, not the last one replayed
# (its size and time are in the replay's fingerprint, Driver/Compile.cpp): the PDB takes the new rules.
$served = Join-Path $work 'served'
New-Item -ItemType Directory -Force $served | Out-Null
$own = Join-Path $served 'point.natvis'
Copy-Item (Join-Path $PSScriptRoot 'natvis\project\point.natvis') $own -Force
$serverArgs = @('--quiet', '--debug-info', '--natvis', $own, '--exe', (Join-Path $served 'values.exe'), (Join-Path $PSScriptRoot 'natvis\values.kpp'))
$sizes = @()
foreach ($round in 1, 2)
{
  if ($round -eq 2) { Add-Content $own '<!-- edited: the rule file grows, nothing else changes -->' }
  $ErrorActionPreference = 'Continue'
  & $KppExe --server @serverArgs 2>&1 | Out-Null
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($code -ne 0) { & $KppExe --stop-server @serverArgs 2>&1 | Out-Null; "FAIL natvis: the build through a compile server failed (exit $code)"; exit 1 }
  $streams = (& $pdbutil dump --named-streams (Join-Path $served 'values.pdb')) -join "`n"
  $sizes += if ($streams -match '/src/files/\S*point\.natvis\s+Index: \d+\s+Size in bytes: (\d+)') { [int]$Matches[1] } else { 0 }
}
& $KppExe --stop-server @serverArgs 2>&1 | Out-Null
if ($sizes[0] -le 0 -or $sizes[1] -ne (Get-Item $own).Length)
{ "FAIL natvis: through a compile server the PDB's point.natvis is $($sizes -join ' then ') bytes, not the edited file's $((Get-Item $own).Length)"; exit 1 }
"ok   natvis ($($names.Count) rules, $size bytes, in the PDB; a project's own .natvis beside it, taken again when edited through a compile server)"
exit 0
