@echo off
rem kpp's heavy jobs run under the machine's heavy-job lock where it has one (C:\kpp-sandbox\heavy.cmd,
rem github.com/kovarex/heavy-queue), elsewhere at once: heavy.ps1 here says how it goes.
setlocal
set "KPP_HEAVY_COMMAND=%*"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0heavy.ps1"
exit /b %errorlevel%
