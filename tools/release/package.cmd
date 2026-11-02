@echo off
rem Runs package.ps1 (docs/build.md, "Installing") with its arguments, where Windows' default policy
rem forbids running scripts: tools\release\package.cmd -Version 0.0.0
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" %*
exit /b %errorlevel%
