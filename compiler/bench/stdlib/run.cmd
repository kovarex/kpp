@echo off
rem Runs the standard library benchmarks (run.py) in the Visual Studio environment and prints the
rem markdown table that docs/stdlib-performance.md quotes. Takes a few minutes.
setlocal
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
python -X utf8 "%~dp0run.py" %*
