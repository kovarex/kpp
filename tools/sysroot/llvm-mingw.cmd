@echo off
rem Runs llvm-mingw.ps1 (the bundled toolchain, design.md 17) with its arguments, where Windows' default
rem policy forbids running scripts: tools\sysroot\llvm-mingw.cmd [-Out DIR]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0llvm-mingw.ps1" %*
exit /b %errorlevel%
