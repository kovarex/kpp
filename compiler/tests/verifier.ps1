# LLVM's verifier on clang's path (main.cpp's -fverify-intermediate-code): an IR bug of kpp's
# (KPP_TEST_BROKEN_MODULE=0: module 0 gets a use its definition does not dominate) fails the build
# with the verifier's message for module 0, in a release build and in a debug build through clang
# (KPP_BACKEND=clang), for windows (Visual Studio's clang) and windows-gnu (llvm-mingw's,
# tools\sysroot\llvm-mingw.ps1). Without the flag a release clang (-disable-llvm-verifier) compiled
# such a module at -O0 and hung on it at -O2, so each build has a time limit. The back end inside
# kpp has its own check (tests\backend.ps1). run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\verifier'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$KppExe = (Resolve-Path $KppExe).Path
$source = (Resolve-Path "$PSScriptRoot\debuginfo\lines.kpp").Path
$mingw = (& "$PSScriptRoot\..\..\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1)
$failed = @()
$savedClang = $env:KPP_CLANG
$env:KPP_TEST_BROKEN_MODULE = '0'
$env:KPP_BACKEND = 'clang'
try
{
  foreach ($target in 'windows', 'windows-gnu')
  {
    if ($target -eq 'windows-gnu') { $env:KPP_CLANG = "$mingw\bin\clang.exe" }
    elseif ($savedClang) { $env:KPP_CLANG = $savedClang }
    else { Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue }
    foreach ($mode in '--release', '--no-optimize')
    {
      $label = "$target $mode"
      $errors = "$work\$target$mode.errors.txt"
      $info = New-Object System.Diagnostics.ProcessStartInfo
      $info.FileName = 'cmd.exe'
      $info.Arguments = "/c `"`"$KppExe`" --quiet $mode --target $target --exe `"$work\$target$mode.exe`" `"$source`" > nul 2> `"$errors`"`""
      $info.UseShellExecute = $false
      $p = [System.Diagnostics.Process]::Start($info)
      if (-not $p.WaitForExit(120000))
      {
        # (the build and its clangs: what is still running under this test's directory)
        Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($work) } |
          ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        $failed += "${label}: the build did not end within 120 s (clang hung on the broken module?)"
        continue
      }
      $text = Get-Content -Raw $errors
      if ($p.ExitCode -eq 0) { $failed += "${label}: the build exited 0" }
      elseif (-not $text.Contains('error: clang rejected module 0') -or -not $text.Contains('Instruction does not dominate all uses'))
      {
        $failed += "${label}: $errors does not report module 0 with the verifier's message"
      }
    }
  }
}
finally
{
  Remove-Item Env:KPP_TEST_BROKEN_MODULE, Env:KPP_BACKEND -ErrorAction SilentlyContinue
  if ($savedClang) { $env:KPP_CLANG = $savedClang } else { Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue }
}
foreach ($f in $failed) { "FAIL verifier: $f" }
if (-not $failed.Count) { "ok   verifier: clang rejects a broken module with the verifier's message, release and debug, windows and windows-gnu" }
exit $failed.Count
