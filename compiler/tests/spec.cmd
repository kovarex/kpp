@echo off
rem One heavy job at a time on PRACOVNA: this script runs under tools\heavy\heavy.cmd (it says how).
if defined KPP_HEAVY_HELD goto :heavy_held
call "%~dp0..\..\tools\heavy\heavy.cmd" "%~f0" %*
exit /b %errorlevel%
:heavy_held
rem The feature specification suite: compiler\tests\spec\<area>\*.kpp, each compiled, run and compared.
rem Writes docs\spec-status.md. Options: --filter TEXT, --jobs N, --no-doc.
python -X utf8 -I "%~dp0spec\run_spec.py" %*
