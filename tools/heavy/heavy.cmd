@echo off
rem PRACOVNA's heavy-job lock: runs COMMAND LINE while holding the machine's one heavy slot (heavy.ps1
rem says how). heavy.cmd [--exclusive] [--name TEXT] COMMAND LINE...   heavy.cmd --status
setlocal
set "KPP_HEAVY_COMMAND=%*"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0heavy.ps1"
exit /b %errorlevel%
