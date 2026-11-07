# kpp --run's build cache, %TEMP%\kpp-run (src/run.cpp): a run builds with four modules, keeps the
# 100 most recently used directories, and never deletes one a run holds (its lock file). In a TEMP of
# its own, build\tests\runcache\temp, planted first: 102 directories used in turn; two older still,
# one whose lock file is held open as a running build holds it and one from before the lock file;
# and one from before the lock file changed now. One run then leaves its own directory, the most
# recent 99 others (the one changed now among them), the held one, and no other: the old one without
# a lock file goes, as it has been unchanged for over an hour. Then a run waits while its
# directory's lock file is held alone, as the cleanup holds it, and builds once it is let go. And
# --no-run builds without running; a program for another --target needs it. run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\runcache'))
$KppExe = (Resolve-Path $KppExe).Path
$failed = 0
function Fail([string]$what) { "FAIL runcache: $what"; $script:failed++ }

if (Test-Path $work) { cmd /c rd /s /q $work }
$temp = Join-Path $work 'temp'
$cache = Join-Path $temp 'kpp-run'
New-Item -ItemType Directory -Force $cache | Out-Null
Set-Content (Join-Path $work 'hello.kpp') "int main()`n{`n  println(`"hello `", 6 * 7);`n  return 0;`n}"

function Plant([string]$name, [datetime]$used, [bool]$lock)
{
  $dir = New-Item -ItemType Directory -Force (Join-Path $cache $name)
  Set-Content (Join-Path $dir 'left.o') ''
  if ($lock)
  {
    Set-Content (Join-Path $dir 'lock') ''
    (Get-Item (Join-Path $dir 'lock')).LastWriteTime = $used
  }
  $dir.LastWriteTime = $used
}
$start = (Get-Date).AddDays(-3)
for ($i = 1; $i -le 102; ++$i) { Plant ('used-{0:d3}' -f $i) $start.AddMinutes($i) $true }
Plant 'held' $start.AddDays(-30) $true
Plant 'before-lock-recent' (Get-Date) $false
Plant 'before-lock-old' $start.AddMinutes(-10) $false
$held = [IO.File]::Open((Join-Path $cache 'held\lock'), 'Open', 'ReadWrite', 'ReadWrite')

$savedTemp = $env:TEMP; $savedTmp = $env:TMP
$env:TEMP = $temp; $env:TMP = $temp
try
{
  $ErrorActionPreference = 'Continue'
  $output = & $KppExe --no-server --run (Join-Path $work 'hello.kpp') 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($code -ne 0 -or ($output -join "`n") -ne 'hello 42') { Fail "kpp --run hello.kpp: exit $code, '$($output -join ' | ')'" }
  $run = @(Get-ChildItem -Directory $cache -Filter 'hello-*')
  if ($run.Count -ne 1) { Fail "$($run.Count) directories hello-* in the cache, not 1" }
  else
  {
    $files = @(Get-ChildItem -Recurse -File $run[0].FullName).Count
    if ($files -gt 30) { Fail "the run's directory holds $files files (four modules: at most 30)" }
  }
  $left = @(Get-ChildItem -Directory $cache | ForEach-Object Name)
  $expected = @($run | ForEach-Object Name) + @('before-lock-recent', 'held') + @(5..102 | ForEach-Object { 'used-{0:d3}' -f $_ })
  $missing = @($expected | Where-Object { $left -notcontains $_ })
  $extra = @($left | Where-Object { $expected -notcontains $_ })
  if ($missing.Count -or $extra.Count) { Fail "the cache after the run: missing [$($missing -join ', ')], left [$($extra -join ', ')]" }
  if (-not (Test-Path (Join-Path $cache 'held\left.o'))) { Fail "the held directory's contents were deleted" }

  if ($run.Count -eq 1)
  {
    # The cleanup's hold: the lock file alone. The run waits for it, then builds.
    $lockPath = Join-Path $run[0].FullName 'lock'
    $alone = [IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'None')
    $process = Start-Process -FilePath $KppExe -ArgumentList '--no-server', '--run', (Join-Path $work 'hello.kpp') -NoNewWindow -PassThru `
      -RedirectStandardOutput (Join-Path $work 'waiting.txt') -RedirectStandardError (Join-Path $work 'waiting.errors.txt')
    $null = $process.Handle
    Start-Sleep -Milliseconds 1500
    $waited = -not $process.HasExited
    $alone.Close()
    $process.WaitForExit()
    $printed = (Get-Content (Join-Path $work 'waiting.txt')) -join ' | '
    if (-not $waited) { Fail "a run did not wait while its lock file was held alone (exit $($process.ExitCode), '$printed')" }
    if ($process.ExitCode -ne 0 -or $printed -ne 'hello 42') { Fail "a run once its lock file was let go: exit $($process.ExitCode), '$printed'" }
  }

  # --no-run builds and names the executable without running it; a program for another --target is
  # an error without it, before any build (2026-11-04: CI builds the libraries' tests for FreeBSD).
  $ErrorActionPreference = 'Continue'
  $output = & $KppExe --no-server --no-run --run (Join-Path $work 'hello.kpp') 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $built = if (($output -join "`n") -match 'kpp --run: built (.*hello\.exe) \(not run: --no-run\)') { $Matches[1] } else { '' }
  if ($code -ne 0 -or -not $built -or -not (Test-Path $built) -or ($output -join "`n") -match 'hello 42') { Fail "kpp --no-run --run: exit $code, '$($output -join ' | ')'" }
  $output = & $KppExe --no-server --target freebsd --run (Join-Path $work 'hello.kpp') 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($code -ne 2 -or ($output -join "`n") -notmatch 'a freebsd program does not run on this machine; --no-run builds it') { Fail "kpp --target freebsd --run without --no-run: exit $code, '$($output -join ' | ')'" }
}
finally
{
  $env:TEMP = $savedTemp; $env:TMP = $savedTmp
  $held.Close()
}

if ($failed) { exit 1 }
cmd /c rd /s /q $work
"ok   runcache (the 100 most recent kept, a held one and a recent one from before the lock too; a run waits for the cleanup's hold; --no-run)"
