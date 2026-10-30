@echo off
rem Compiles every tests\run\*.kpp with kppc, runs it, and compares stdout with the .expected file.
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
set KPPC=build\kppc.exe
if not exist build\tests mkdir build\tests
set FAIL=0
set PASS=0
for %%f in (tests\run\*.kpp) do (
  set NAME=%%~nf
  %KPPC% --quiet --exe build\tests\!NAME!.exe %%f > build\tests\!NAME!.compile.txt 2>&1
  if errorlevel 1 (
    echo FAIL !NAME!: compile
    type build\tests\!NAME!.compile.txt
    set /a FAIL+=1
  ) else (
    build\tests\!NAME!.exe > build\tests\!NAME!.out.txt
    fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected > nul
    if errorlevel 1 (
      echo FAIL !NAME!: output differs
      fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected
      set /a FAIL+=1
    ) else (
      echo ok   !NAME!
      set /a PASS+=1
    )
  )
)
echo %PASS% passed, %FAIL% failed
rem --access-warnings (docs/design.md, "struct and class"): the warnings and notes on tests\access\access.kpp.
%KPPC% --quiet --access-warnings --exe build\tests\access.exe tests\access\access.kpp 2> build\tests\access.all.txt > nul
%SystemRoot%\System32\findstr.exe /b /c:"tests\access\access.kpp" build\tests\access.all.txt > build\tests\access.warnings.txt
fc /w build\tests\access.warnings.txt tests\access\access.expected > nul
if errorlevel 1 (
  echo FAIL access warnings: build\tests\access.warnings.txt differs from tests\access\access.expected
  fc /w build\tests\access.warnings.txt tests\access\access.expected
  set /a FAIL+=1
) else (
  echo ok   access warnings
)
rem --msvc-diagnostics (docs/build.md, "Visual Studio"): errors as path(line): error: ..., which Visual Studio reads.
%KPPC% --quiet --msvc-diagnostics --exe build\tests\msvc.exe tests\diagnostics\msvc.kpp 2> build\tests\msvc.errors.txt > nul
fc /w build\tests\msvc.errors.txt tests\diagnostics\msvc.expected > nul
if errorlevel 1 (
  echo FAIL msvc diagnostics: build\tests\msvc.errors.txt differs from tests\diagnostics\msvc.expected
  fc /w build\tests\msvc.errors.txt tests\diagnostics\msvc.expected
  set /a FAIL+=1
) else (
  echo ok   msvc diagnostics
)
rem --debug-info: the line tables in the PDB (tests\debuginfo.ps1, docs/debugging.md).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\debuginfo.ps1 -Kppc %KPPC%
if errorlevel 1 set /a FAIL+=1
rem The output must not depend on the thread count (tests\determinism.ps1, docs/compile-speed.md).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\determinism.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL determinism: the output depends on the thread count
  set /a FAIL+=1
)
rem The compile server must build what a new process builds, edit after edit (tests\incremental.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\incremental.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL incremental: the compile server's build differs from a new process's
  set /a FAIL+=1
)
if %FAIL% neq 0 exit /b 1
rem (Explicitly: the batch file's exit code would otherwise be the determinism check's.)
exit /b 0
