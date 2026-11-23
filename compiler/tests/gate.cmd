@echo off
rem One heavy job at a time on PRACOVNA: this script runs under tools\heavy\heavy.cmd (it says how).
if defined KPP_HEAVY_HELD goto :heavy_held
call "%~dp0..\..\tools\heavy\heavy.cmd" "%~f0" %*
exit /b %errorlevel%
:heavy_held
rem The quick gate before a push: run_tests --quick and the spec side by side (tests\gate.ps1, which
rem says what it prints). --full: the whole of run_tests beside the spec. The last line says whether
rem it passed, and the exit code agrees with it.
set "FULL="
if "%~1"=="--full" set "FULL=-Full"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gate.ps1" %FULL%
exit /b %errorlevel%
