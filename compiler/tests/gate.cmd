@echo off
rem The quick gate before a push: run_tests --quick and the spec side by side (tests\gate.ps1, which
rem says what it prints). --full: the whole of run_tests beside the spec. The last line says whether
rem it passed, and the exit code agrees with it.
set "FULL="
if "%~1"=="--full" set "FULL=-Full"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gate.ps1" %FULL%
exit /b %errorlevel%
