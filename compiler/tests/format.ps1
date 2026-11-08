# kpp format (docs/format.md): each tests\format\*.kpp is in kpp's style, so formatting it changes
# nothing; each tests\format\fix\NAME.kpp formats to fix\NAME.expected, which formatting leaves as it
# is (formatting twice is formatting once); --check says which files are not in the style.
# run_tests.cmd runs it. -Update writes the .expected files from this run.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe", [switch]$Update)
$ErrorActionPreference = 'Stop'
$KppExe = (Resolve-Path $KppExe).Path
$dir = Join-Path $PSScriptRoot 'format'
$failed = @()

function Format([string]$path)
{
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $KppExe
  $psi.Arguments = "format --stdout `"$path`""
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
  $p = [System.Diagnostics.Process]::Start($psi)
  $out = $p.StandardOutput.ReadToEnd()
  $err = $p.StandardError.ReadToEnd()
  $p.WaitForExit()
  return @{ Code = $p.ExitCode; Text = $out; Error = $err }
}

function Read([string]$path) { return [IO.File]::ReadAllText($path, (New-Object System.Text.UTF8Encoding($false))) }

foreach ($f in Get-ChildItem (Join-Path $dir '*.kpp') | Sort-Object Name)
{
  $r = Format $f.FullName
  if ($r.Code -ne 0) { $failed += "$($f.Name): not formatted: $($r.Error.Trim())"; continue }
  if ($r.Text -ne (Read $f.FullName)) { $failed += "$($f.Name): formatting changes it (kpp format --stdout shows how)" }
}
foreach ($f in Get-ChildItem (Join-Path $dir 'fix\*.kpp') | Sort-Object Name)
{
  $expected = [IO.Path]::ChangeExtension($f.FullName, '.expected')
  $r = Format $f.FullName
  if ($r.Code -ne 0) { $failed += "fix\$($f.Name): not formatted: $($r.Error.Trim())"; continue }
  if ($Update) { [IO.File]::WriteAllText($expected, $r.Text, (New-Object System.Text.UTF8Encoding($false))) }
  if (-not (Test-Path $expected)) { $failed += "fix\$($f.Name): no $([IO.Path]::GetFileName($expected))"; continue }
  if ($r.Text -ne (Read $expected)) { $failed += "fix\$($f.Name): the output is not $([IO.Path]::GetFileName($expected))"; continue }
  # formatting the output again changes nothing
  $copy = Join-Path $PSScriptRoot "..\build\tests\format\$($f.BaseName).kpp"
  New-Item -ItemType Directory -Force (Split-Path $copy) | Out-Null
  [IO.File]::WriteAllText($copy, $r.Text, (New-Object System.Text.UTF8Encoding($false)))
  $again = Format $copy
  if ($again.Text -ne $r.Text) { $failed += "fix\$($f.Name): formatting the output again changes it" }
}
# --check: 0 for files in the style, 1 when one is not
& $KppExe format --check (Join-Path $dir 'canonical.kpp') | Out-Null
if ($LASTEXITCODE -ne 0) { $failed += "--check: exit $LASTEXITCODE for canonical.kpp" }
& $KppExe format --check (Join-Path $dir 'fix') | Out-Null
if ($LASTEXITCODE -ne 1) { $failed += "--check: exit $LASTEXITCODE for fix\ (1 expected)" }

if ($failed) { "FAIL format:"; $failed | ForEach-Object { "  $_" }; exit 1 }
"format: OK"
exit 0
