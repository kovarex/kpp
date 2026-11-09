@echo off
rem kpp format's round trip on the converted game (kovarex 2026-11-08, through the coordinator): the game's
rem src\ and tests\ in a factorio-in-kpp checkout (the first argument, else FACTORIO_IN_KPP), each file first
rem formatted in kpp's default style in a copy, then compiler\tests\format\run_format.py's checks in the
rem default and two others: combination kr (braces, indentation) and combination narrow (wrapping at 80
rem columns). Not libraries\: third-party code in its own styles. Not in CI: the game is private; thread H's
rem batches on master run it. It only reads the checkout. Its copies go in the second argument, else
rem DIR\build\format-check, deleted when it passes; C:\factorio-in-kpp, which every thread reads, needs the
rem second argument. A file with a line splice between tokens is never formatted (docs/format.md): listed,
rem counted and no failure (2 in the game, 2026-11-09). The last line is `format: ALL PASSED (...)` or `format: N FAILED`, the exit code 0 or 1.
setlocal
set "HERE=%~dp0"
set "CHECKOUT=%FACTORIO_IN_KPP%"
if not "%~1"=="" set "CHECKOUT=%~f1"
if "%CHECKOUT%"=="" (echo format-check: name the factorio-in-kpp checkout: format-check.cmd DIR [OUT], or set FACTORIO_IN_KPP=DIR& exit /b 2)
if not exist "%CHECKOUT%\src\" (echo format-check: %CHECKOUT% has no src\& exit /b 2)
set "OUT=%~f2"
if "%OUT%"=="" (
  if /i "%CHECKOUT%"=="C:\factorio-in-kpp" (echo format-check: every thread reads C:\factorio-in-kpp: name a directory of your own, format-check.cmd DIR OUT& exit /b 2)
  set "OUT=%CHECKOUT%\build\format-check"
)
python -I "%HERE%..\..\compiler\tests\format\run_format.py" --corpus "%CHECKOUT%\src" --corpus "%CHECKOUT%\tests" --configuration "combination kr" --configuration "combination narrow" --allow-refused "a line splice between tokens" --jobs 16 --out "%OUT%"
exit /b %errorlevel%
