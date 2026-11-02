@echo off
rem The feature specification suite: compiler\tests\spec\<area>\*.kpp, each compiled, run and compared.
rem Writes docs\spec-status.md. Options: --filter TEXT, --jobs N, --no-doc.
setlocal
rem Streaming's barrier check in codegen (KPP_CHECK_BARRIER, docs/codegen-speed.md), on for every
rem build here until program pieces rely on it.
set KPP_CHECK_BARRIER=1
python -X utf8 -I "%~dp0spec\run_spec.py" %*
