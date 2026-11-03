@echo off
rem Copies what the converted game takes from the Factorio source unchanged into the factorio-in-kpp
rem checkout (the directory given as the first argument, else FACTORIO_IN_KPP; C:\factorio-in-kpp for a
rem push, committed there by hand), at the paths it has in Factorio's repository: data\, shaders\, version.txt, libraries\
rem (the C and C++ libraries the game imports and links, as Factorio has them) and tests\ (the saves,
rem images and mods the tests read). The converted libraries' and the tests' own C and C++ sources
rem are left out: convert.cmd writes their kpp conversion there. Run after convert.cmd when the
rem Factorio source changes. The files are git archive's of FACTORIO's HEAD, with its checkout's line
rem endings (core.autocrlf), as the game's working tree has them; FACTORIO must be a git checkout.
setlocal
if not defined FACTORIO set FACTORIO=C:\Factorio
rem (the checkout: the first argument, else FACTORIO_IN_KPP; never a thread's copy, a sparse checkout
rem without data\ and shaders\ whose game reads C:\factorio-in-kpp's: setup-checkout.cmd)
set WHOLE=%FACTORIO_IN_KPP%
if not "%~1"=="" set WHOLE=%~1
if "%WHOLE%"=="" (echo resources: name the factorio-in-kpp checkout: resources.cmd DIR, or set FACTORIO_IN_KPP=DIR& exit /b 2)
set ATTRIBUTES=-
if exist "%WHOLE%\data" for %%a in ("%WHOLE%\data") do set ATTRIBUTES=%%~aa
if not "%ATTRIBUTES:l=%"=="%ATTRIBUTES%" (echo resources: %WHOLE%\data is a junction: a copy shares C:\factorio-in-kpp's resources, update those there ^(and run setup-checkout.cmd --unlink on it^)& exit /b 2)
for /f %%s in ('git -C "%WHOLE%" config --get core.sparseCheckout 2^>nul') do if "%%s"=="true" (echo resources: %WHOLE% is a thread's copy ^(a sparse checkout^): its game reads C:\factorio-in-kpp's resources, update those there& exit /b 2)
rem The libraries convert.cmd converts, and the shims it copies (libraries\kpp-native). Of a C library
rem converted to kpp (c-libraries.cmd) only the .c and .cpp files are left out: its headers stay C.
call "%~dp0c-libraries.cmd"
set CONVERTED=CommonUtil Agui cxxopts StackWalker Lua crc32fast UnitTest++ %CLIBRARIES%
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
rem (The converter's own C headers stay too: a C library's configured imports, kpp_*.h.)
for %%d in (%CONVERTED%) do if exist "%WHOLE%\libraries\%%d" for /f "delims=" %%f in ('dir /s /b /a-d "%WHOLE%\libraries\%%d"') do if /i not "%%~xf"==".kpp" call :keep "%%f"
if exist "%WHOLE%\tests" for /r "%WHOLE%\tests" %%f in (*) do if /i not "%%~xf"==".kpp" del /q "%%f"
endlocal
rem A pathspec's * also matches /: libraries/Lua/*.c is every .c file below libraries/Lua.
set EXCLUDE=
for %%d in (libraries/CommonUtil libraries/Agui libraries/cxxopts libraries/StackWalker libraries/Lua libraries/crc32fast libraries/UnitTest++ tests) do for %%e in (c h cpp hpp) do call set EXCLUDE=%%EXCLUDE%% ":(exclude)%%d/*.%%e"
for %%d in (%CLIBRARIES%) do call set EXCLUDE=%%EXCLUDE%% ":(exclude)libraries/%%d/*.c" ":(exclude)libraries/%%d/*.cpp"
if not exist "%WHOLE%" mkdir "%WHOLE%"
git -C %FACTORIO% archive HEAD -- data shaders libraries tests version.txt %EXCLUDE% | %SystemRoot%\System32\tar.exe -x -f - -C "%WHOLE%" || exit /b 1
for /f %%h in ('git -C %FACTORIO% rev-parse --short HEAD') do echo resources: %FACTORIO% %%h into %WHOLE%
goto :eof

rem A file of a converted library's directory: deleted, unless the converter wrote it (kpp_*.h).
:keep
set KEEPNAME=%~nx1
if /i "%KEEPNAME:~0,4%"=="kpp_" goto :eof
del /q %1
goto :eof
