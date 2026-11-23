@echo off
rem One heavy job at a time on PRACOVNA: this script runs under tools\heavy\heavy.cmd (it says how).
if not defined KPP_HEAVY_HELD call "%~dp0..\..\tools\heavy\heavy.cmd" "%~f0" %* & goto :eof
rem The feature specification suite: compiler\tests\spec\<area>\*.kpp, each compiled, run and compared.
rem Writes docs\spec-status.md. Options: --filter TEXT, --jobs N, --no-doc.
python -X utf8 -I "%~dp0spec\run_spec.py" %*
