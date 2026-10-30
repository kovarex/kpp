@echo off
rem Makes Visual Studio colour .kpp files (tools/vs/README.md): links tools\vs\kpp, a TextMate grammar,
rem into %USERPROFILE%\.vs\Extensions\kpp, where Visual Studio reads TextMate grammars at its start.
rem A junction, so that a pull updates the grammar. Run once; restart Visual Studio.
setlocal
set TARGET=%USERPROFILE%\.vs\Extensions\kpp
if not exist "%USERPROFILE%\.vs\Extensions" mkdir "%USERPROFILE%\.vs\Extensions"
if exist "%TARGET%" rmdir "%TARGET%"
mklink /J "%TARGET%" "%~dp0kpp" || exit /b 1
