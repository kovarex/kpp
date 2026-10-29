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
rem The output must not depend on the thread count (tests\determinism.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\determinism.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL determinism
  set /a FAIL+=1
)
if %FAIL% neq 0 exit /b 1
