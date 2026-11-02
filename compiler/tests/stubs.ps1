# --keep-going --stub-missing with an error the checker reported (tests\stubs\: broken's body names
# something declared nowhere): codegen leaves broken out, and the module that calls it (main's, another
# file's) gets a stub that names it and aborts. The build reports the checker's error alone (codegen's
# failure on the call holding it follows from it and is not shown) and exits 1, and the program runs
# up to the call. Such a failure, not shown, is a failure all the same: when it was the only one of
# codegen's, nothing declared what it left out, and clang rejected the calling module. In one module,
# in four, and in a release build (ThinLTO). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\stubs'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$Kppc = (Resolve-Path $Kppc).Path
$sources = @((Resolve-Path "$PSScriptRoot\stubs\broken.kpp").Path, (Resolve-Path "$PSScriptRoot\stubs\main.kpp").Path)
$failed = @()

# A process's exit code, stdout and stderr (not PowerShell's 2>&1, which makes stderr errors).
function Run([string]$file, [string]$arguments)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo $file, $arguments
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.WorkingDirectory = $work
  $p = [System.Diagnostics.Process]::Start($info)
  $errors = $p.StandardError.ReadToEndAsync()
  $output = $p.StandardOutput.ReadToEnd()
  $p.WaitForExit()
  return [pscustomobject]@{ code = $p.ExitCode; text = $output + $errors.Result; out = $output }
}

foreach ($mode in '--modules 1', '--modules 4', '--release --modules 4')
{
  $exe = "$work\stubs$($mode -replace '[- ]', '').exe"
  Remove-Item $exe -ErrorAction SilentlyContinue
  $build = Run $Kppc "--quiet --keep-going --stub-missing $mode --exe `"$exe`" `"$($sources[0])`" `"$($sources[1])`""
  $errorLines = @($build.text -split "`r?`n" | Where-Object { $_ -match ': error: ' })
  if ($build.code -ne 1) { $failed += "${mode}: the build exited $($build.code), not 1"; continue }
  if ($errorLines.Count -ne 1 -or -not $errorLines[0].Contains("'undefinedName' is not declared"))
  {
    $failed += "${mode}: the errors are not the checker's alone: $($errorLines -join ' | ')"
    continue
  }
  if ($build.text -notmatch 'stubs: 1 functions left out') { $failed += "${mode}: no stub for broken: $(($build.text -split "`r?`n" | Select-Object -First 6) -join ' | ')"; continue }
  if (-not (Test-Path $exe)) { $failed += "${mode}: no executable"; continue }
  $run = Run $exe ''
  if ($run.code -ne 0 -or $run.out -notmatch '^before\r?\nafter') { $failed += "${mode}: the program printed '$($run.text.Trim())', exit $($run.code)" }
  $stub = Run $exe 'call'
  if ($stub.code -eq 0 -or -not $stub.text.Contains('broken at') -or -not $stub.text.Contains('broken.kpp:3'))
  {
    $failed += "${mode}: calling broken did not abort with its stub's message: '$($stub.text.Trim())', exit $($stub.code)"
  }
}
foreach ($f in $failed) { "FAIL stubs: $f" }
if (-not $failed.Count) { "ok   stubs: a function the checker found an error in is a stub in the module that calls it (--keep-going --stub-missing), in one module, four, and a release build" }
exit $failed.Count
