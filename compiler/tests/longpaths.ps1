# Paths near Windows' MAX_PATH (docs/build.md, "Long paths"): Windows' tools open no path of 260
# characters or more, kpp's own writes and link.exe's output even as a \\?\ path, whatever
# LongPathsEnabled says.
# - kpp: an executable's path of at most 238 characters builds and runs (kpp writes files beside it
#   with up to 21 more, NAME.failed-functions.txt); one character more is a command-line error at once
#   (exit 2, naming MAX_PATH), where it was "cannot write" for each module's file.
# - The spec runner: thread K's snippet of an 80-character name, built with debug information under an
#   output directory of 140 characters. Its build directory and program were <area>.<name> twice over,
#   328 characters; now a name of more than 40 characters is cut to 32 and a hash, the program
#   program.exe (run_spec.py's work_name).
# run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$KppExe = (Resolve-Path $KppExe).Path
$work = Join-Path $PSScriptRoot '..\build\tests\longpaths'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$hello = Join-Path $work 'hello.kpp'
[IO.File]::WriteAllText($hello, "int main() { println(`"long paths`"); return 0; }`n")

# A directory of $length characters under $work (each part at most 100), not created.
function DeepPath([int]$length)
{
  $path = $work
  $i = 0
  while ($path.Length -lt $length)
  {
    $path += '\' + ([string][char](97 + $i % 26)) * [Math]::Min(100, $length - $path.Length - 1)
    $i++
  }
  return $path
}

# The longest executable's path kpp takes: 238 characters.
$dir = DeepPath (238 - '\h.exe'.Length)
New-Item -ItemType Directory -Force $dir | Out-Null
$exe = "$dir\h.exe"
if ($exe.Length -ne 238) { "FAIL long paths: the test's path is $($exe.Length) characters, not 238"; exit 1 }
$ErrorActionPreference = 'Continue'
$log = & $KppExe --quiet --debug-info --exe $exe $hello 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0) { "FAIL long paths: kpp --exe of 238 characters, exit $code"; $log | Select-Object -Last 5 | ForEach-Object { "  $_" }; exit 1 }
$out = & $exe
if ("$out" -ne 'long paths') { "FAIL long paths: the program of a 238-character path printed '$out'"; exit 1 }

# One character more: the error before any work, no file written.
$dir = DeepPath (239 - '\h.exe'.Length)
New-Item -ItemType Directory -Force $dir | Out-Null
$exe = "$dir\h.exe"
$ErrorActionPreference = 'Continue'
$log = (& $KppExe --quiet --exe $exe $hello 2>&1 | ForEach-Object { "$_" }) -join "`n"
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 2 -or $log -notmatch 'its path is 239 characters' -or $log -notmatch 'MAX_PATH' -or $log -notmatch 'at most 238 characters') { "FAIL long paths: kpp --exe of 239 characters: exit $code`n$log"; exit 1 }
if (@(Get-ChildItem $dir).Count -ne 0) { "FAIL long paths: kpp --exe of 239 characters wrote $(@(Get-ChildItem $dir | ForEach-Object Name) -join ', ')"; exit 1 }

# The spec runner under a deep output directory (run_spec.py appends "debug-info" to it).
$out = DeepPath (140 - 'debug-info'.Length)
$ErrorActionPreference = 'Continue'
$log = & python -X utf8 -I (Join-Path $PSScriptRoot 'spec\run_spec.py') --no-doc --debug-info --jobs 1 --out $out `
  --only 'templates/qualified_pattern_of_a_deduced_parameter_never_matches_a_function_type.kpp' 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0 -or $log[-1] -notmatch '^spec: ALL PASSED \(1 pass') { "FAIL long paths: the spec runner under a 140-character directory, exit $code"; $log | Select-Object -Last 8 | ForEach-Object { "  $_" }; exit 1 }

cmd /c rd /s /q "$work"
"long paths: ok"
exit 0
