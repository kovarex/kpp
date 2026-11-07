# LLVM's back end in kpp (src\backend.cpp), when this kpp has it (kpp --version): what LLVM rejects
# fails its module with LLVM's message and fails the build (never a crash, an exit from a worker
# thread, or an object), for both targets it compiles: windows (Visual Studio's) and windows-gnu
# (llvm-mingw's, tools\sysroot\llvm-mingw.ps1). KPP_TEST_BROKEN_MODULE=0 gives module 0 an IR bug the
# verifier rejects; KPP_TEST_BROKEN_ASM=0 an instruction only the back end's assembler rejects (LLVM's
# diagnostics). run_tests.cmd runs it. Exit 0: passed; 1: failed; 3: skipped (no LLVM back end).
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
if (-not ((& $KppExe --version) -match "^back end: LLVM's")) { "skipped: this kpp has no LLVM back end"; exit 3 }
$work = Join-Path $PSScriptRoot '..\build\tests\backend'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$mingw = (& "$PSScriptRoot\..\..\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1)
$source = (Resolve-Path "$PSScriptRoot\debuginfo\lines.kpp").Path
$cases = @(
  @{ name = 'verifier'; variable = 'KPP_TEST_BROKEN_MODULE'; says = 'Instruction does not dominate all uses' },
  @{ name = 'diagnostics'; variable = 'KPP_TEST_BROKEN_ASM'; says = "invalid instruction mnemonic 'kpp_no_such_instruction'" })
$failed = @()
$clang = $env:KPP_CLANG
foreach ($target in 'windows', 'windows-gnu')
{
  if ($target -eq 'windows-gnu') { $env:KPP_CLANG = "$mingw\bin\clang.exe" }
  foreach ($c in $cases)
  {
    $errors = "$work\$target-$($c.name).errors.txt"
    Set-Item "Env:$($c.variable)" '0'
    cmd /c "`"$KppExe`" --quiet --keep-going --target $target --exe `"$work\$target-$($c.name).exe`" `"$source`" > nul 2> `"$errors`""
    $rc = $LASTEXITCODE
    Remove-Item "Env:$($c.variable)"
    $text = Get-Content -Raw $errors
    if ($rc -eq 0) { $failed += "$target $($c.name): the build exited 0" }
    elseif (-not $text.Contains("error: LLVM's back end rejected module 0") -or -not $text.Contains($c.says))
    {
      $failed += "$target $($c.name): $errors does not report module 0 with LLVM's message"
    }
  }
  if ($clang) { $env:KPP_CLANG = $clang } else { Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue }
}
foreach ($f in $failed) { "FAIL LLVM back end: $f" }
if (-not $failed.Count) { "ok   LLVM back end: the verifier and LLVM's diagnostics fail module 0, windows and windows-gnu" }
exit [int]($failed.Count -ne 0)
