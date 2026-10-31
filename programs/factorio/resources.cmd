@echo off
rem Copies what the converted game takes from the Factorio source unchanged into the factorio-in-kpp
rem checkout (C:\factorio-in-kpp, or the directory given as the first argument; commit there by
rem hand), at the paths it has in Factorio's repository: data\, shaders\, version.txt, libraries\
rem (the C and C++ libraries the game imports and links, as Factorio has them) and tests\ (the saves,
rem images and mods the tests read). The converted libraries' and the tests' own C and C++ sources
rem are left out: convert.cmd writes their Kpp conversion there. Run after convert.cmd when the
rem Factorio source changes. The files are git archive's of FACTORIO's HEAD, with its checkout's line
rem endings (core.autocrlf), as the game's working tree has them; FACTORIO must be a git checkout.
setlocal
if not defined FACTORIO set FACTORIO=C:\Factorio
set WHOLE=C:\factorio-in-kpp
if not "%~1"=="" set WHOLE=%~1
rem The libraries convert.cmd converts, and the shims it copies (libraries\kpp-native).
set CONVERTED=CommonUtil Agui cxxopts StackWalker Lua crc32fast UnitTest++
rem Fresh every run: a file Factorio no longer has would stay. Every file of these directories but
rem the conversion's (the .kpp files, and libraries\kpp-native) is removed.
for %%d in (data shaders) do if exist "%WHOLE%\%%d" rd /s /q "%WHOLE%\%%d"
if exist "%WHOLE%\version.txt" del /q "%WHOLE%\version.txt"
setlocal EnableDelayedExpansion
if exist "%WHOLE%\libraries" for /d %%d in ("%WHOLE%\libraries\*") do (
  set KEEP=0
  for %%c in (%CONVERTED% kpp-native) do if /i "%%~nxd"=="%%c" set KEEP=1
  if !KEEP!==0 rd /s /q "%%d"
)
rem (dir, not for /r: for /r's directory cannot be another for's variable, and then it lists nothing.)
for %%d in (%CONVERTED%) do if exist "%WHOLE%\libraries\%%d" for /f "delims=" %%f in ('dir /s /b /a-d "%WHOLE%\libraries\%%d"') do if /i not "%%~xf"==".kpp" del /q "%%f"
if exist "%WHOLE%\tests" for /r "%WHOLE%\tests" %%f in (*) do if /i not "%%~xf"==".kpp" del /q "%%f"
endlocal
rem A pathspec's * also matches /: libraries/Lua/*.c is every .c file below libraries/Lua.
set EXCLUDE=
for %%d in (libraries/CommonUtil libraries/Agui libraries/cxxopts libraries/StackWalker libraries/Lua libraries/crc32fast libraries/UnitTest++ tests) do for %%e in (c h cpp hpp) do call set EXCLUDE=%%EXCLUDE%% ":(exclude)%%d/*.%%e"
if not exist "%WHOLE%" mkdir "%WHOLE%"
git -C %FACTORIO% archive HEAD -- data shaders libraries tests version.txt %EXCLUDE% | %SystemRoot%\System32\tar.exe -x -f - -C "%WHOLE%" || exit /b 1
for /f %%h in ('git -C %FACTORIO% rev-parse --short HEAD') do echo resources: %FACTORIO% %%h into %WHOLE%
