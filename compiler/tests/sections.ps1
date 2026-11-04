# A debug build's objects without a section per function where its link is link.exe's incremental
# one (--incremental-link, which keeps every function: /OPT:NOREF), so that the edit's link does not
# carry a section contribution per function (docs/debugging.md, "The edit's link"); with one per
# function where the link drops what nobody calls (a debug build linked whole). The same program at 2
# modules (and kppc's own pieces) both ways: the --incremental-link build's objects hold a few .text
# sections, the other's one per function, and both programs print the same. run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\sections'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path

# kppc's output and exit code (its stderr is no error of PowerShell's).
function Native([string[]]$arguments)
{
  $ErrorActionPreference = 'Continue'
  $output = & $Kppc @arguments 2>&1 | ForEach-Object { "$_" }
  $script:code = $LASTEXITCODE
  return $output
}

# The number of sections named .text in a COFF object (its own functions' code).
function TextSections([string]$object)
{
  $b = [IO.File]::ReadAllBytes($object)
  if ($b[0] -eq 0 -and $b[1] -eq 0 -and $b[2] -eq 0xFF -and $b[3] -eq 0xFF) { throw "$object is a /bigobj object" }
  $count = [BitConverter]::ToUInt16($b, 2)
  $optional = [BitConverter]::ToUInt16($b, 16)
  $n = 0
  for ($i = 0; $i -lt $count; $i++)
  {
    if ([Text.Encoding]::ASCII.GetString($b, 20 + $optional + 40 * $i, 8).TrimEnd([char]0) -eq '.text') { $n++ }
  }
  return $n
}

$functions = 40
$source = Join-Path $work 'functions.kpp'
$text = ''
for ($i = 0; $i -lt $functions; $i++) { $text += "int f$i(int x) { return x + $i; }`n" }
$text += "int main()`n{`n  int sum = 0;`n"
for ($i = 0; $i -lt $functions; $i++) { $text += "  sum = f$i(sum);`n" }
$text += "  println(`"sum `", sum);`n  return 0;`n}`n"
[IO.File]::WriteAllText($source, $text)
$expected = 'sum ' + (($functions - 1) * $functions / 2)

$failed = 0
$sections = @{}
foreach ($mode in 'incremental', 'whole')
{
  $exe = Join-Path $work "$mode.exe"
  Remove-Item "$exe*" -ErrorAction SilentlyContinue
  $arguments = @('--quiet', '--toolchain', 'msvc', '--modules', '2')
  if ($mode -eq 'incremental') { $arguments += '--incremental-link' }
  $output = Native ($arguments + @('--exe', $exe, $source))
  if ($code -ne 0) { "FAIL sections: the $mode build: $($output -join "`n")"; exit 1 }
  $printed = & $exe
  if ("$printed" -ne $expected) { "FAIL sections: the $mode build printed '$printed', not '$expected'"; $failed++ }
  $objects = @(Get-ChildItem "$exe.*.o")
  if ($objects.Count -lt 2) { "FAIL sections: the $mode build has $($objects.Count) objects, not several"; exit 1 }
  $sections[$mode] = ($objects | ForEach-Object { TextSections $_.FullName } | Measure-Object -Sum).Sum
}
if ($sections['whole'] -lt $functions) { "FAIL sections: a build linked whole has $($sections['whole']) .text sections for $functions functions (one per function)"; $failed++ }
if ($sections['incremental'] * 4 -gt $sections['whole']) { "FAIL sections: an --incremental-link build has $($sections['incremental']) .text sections, the build linked whole $($sections['whole']): a section per function"; $failed++ }
# The edit loop on the --incremental-link build: f5's body changed, then grown well past its old size
# (a loop of calls), each built again into the same executable (link.exe relinks the module whose
# object changed, all its functions in one section) and run: the program is the edited one.
$exe = Join-Path $work 'incremental.exe'
$edits = @(
  @("int f5(int x) { return x + 1005; }", (($functions - 1) * $functions / 2 + 1000)),
  @("int f5(int x)`n{`n  int y = x;`n  for (int i = 0; i < 1000; ++i)`n    y = f0(y) + f1(0) - f1(0) + 1;`n  return y + 6;`n}", (($functions - 1) * $functions / 2 + 1001)))
foreach ($edit in $edits)
{
  [IO.File]::WriteAllText($source, $text.Replace("int f5(int x) { return x + 5; }", $edit[0]))
  $output = Native @('--quiet', '--toolchain', 'msvc', '--modules', '2', '--incremental-link', '--exe', $exe, $source)
  $printed = if ($code -eq 0) { & $exe } else { '' }
  if ($code -ne 0 -or "$printed" -ne "sum $($edit[1])") { "FAIL sections: the --incremental-link build edited ($($edit[0].Split("`n")[0])...) printed '$printed', not 'sum $($edit[1])': $($output -join "`n")"; $failed++ }
}
[IO.File]::WriteAllText($source, $text)
if ($failed) { exit 1 }
"ok   sections: an --incremental-link build's objects $($sections['incremental']) .text sections, linked whole $($sections['whole']) ($functions functions)"
