@echo off
rem overrides\implicit-dropped.txt: the `implicit` marks the converter writes that no build of the game asks
rem about (kovarex 2026-11-09: drop the unneeded `implicit`; design.md "Explicit by default"). Builds the converted checkout (the
rem first argument, else FACTORIO_IN_KPP; convert.cmd's output, with this script's kpp\ beside it) with kpp's
rem --implicit-uses in each of its 15 builds: Windows, Linux and FreeBSD, debug, release and final, with the
rem tests and without, but final with the tests, which the C++ does not compile either (its tests use what
rem PACKAGED_BUILD leaves out). Code a build leaves out by a false `when` asks nothing there. Each through
rem codegen, where the instances' bodies are walked. Then implicit-dropped.py keeps a mark any build asks about and
rem writes the others. The next convert.cmd writes them without the word; its builds and the game's tests
rem show nothing changed. A mark the list took out cannot be asked about, so the list is made from a conversion
rem without it: delete overrides\implicit-dropped.txt, convert.cmd, this, convert.cmd. Uses files in kpp\implicit-uses\ (about 10 MB each), IR in kpp\implicit-uses.ll.
setlocal
if not defined FACTORIO set FACTORIO=C:\Factorio
cd /d "%~dp0"
set KPP=..\..\compiler\build\kpp.exe
set WHOLE=%FACTORIO_IN_KPP%
if not "%~1"=="" set WHOLE=%~1
if "%WHOLE%"=="" (
  echo implicit-dropped: give the checkout, or set FACTORIO_IN_KPP
  exit /b 1
)
if exist overrides\implicit-dropped.txt (
  echo implicit-dropped: delete overrides\implicit-dropped.txt and run convert.cmd first: a mark the list took out cannot be asked about
  exit /b 1
)
rem The C libraries and the import paths as convert.cmd's builds take them.
for /f "tokens=2 delims==" %%a in ('findstr /b /c:"set CLIBRARIES=" c-libraries.cmd') do set CLIBRARIES=%%a
set CLIBWHOLE=
set CLIBINC=
for %%l in (%CLIBRARIES%) do for /f "usebackq delims=" %%i in ("c-libraries\%%l\includes.txt") do call set CLIBINC=%%CLIBINC%% --cinclude %FACTORIO%\libraries\%%l\%%i
for %%l in (%CLIBRARIES%) do (
  if exist c-libraries\%%l\interface.txt (
    call set CLIBWHOLE=%%CLIBWHOLE%% --library-root %%l %WHOLE%\libraries\%%l --exclude %WHOLE%\libraries\%%l\build.kpp
  ) else (
    call set CLIBWHOLE=%%CLIBWHOLE%% %WHOLE%\libraries\%%l
  )
)
set CINC=--cinclude %FACTORIO%\libraries\SDL3\include --cinclude %FACTORIO%\libraries\SDL_Mixer\include --cinclude %FACTORIO%\libraries\Lua --cinclude %FACTORIO%\libraries --cinclude %FACTORIO%\libraries\curl\include --cinclude %FACTORIO%\libraries\glad\include --cinclude %FACTORIO%\libraries\trio --cinclude native\include %CLIBINC%
for %%l in (%CLIBRARIES%) do call set CINC=%%CINC%% --cinclude kpp\%%l
set L=%WHOLE%\libraries
set BASE=--std ..\..\stdlib %CINC% --ext .kpp --errors 0 --unresolved 0 --emit-llvm kpp\implicit-uses.ll
set WINDOWS=
set LINUX=--cinclude %FACTORIO%\libraries\libbacktrace --target linux --sysroot ..\..\compiler\build\sysroots\linux-x86_64-factorio
set FREEBSD=--cinclude %FACTORIO%\libraries\libbacktrace --target freebsd --sysroot ..\..\compiler\build\sysroots\freebsd-x86_64
set ROOTS_WINDOWS=%L%\CommonUtil %L%\Agui %L%\cxxopts %L%\StackWalker %L%\Lua %L%\crc32fast %CLIBWHOLE% %WHOLE%\src
set ROOTS_LINUX=%L%\CommonUtil %L%\Agui %L%\cxxopts %L%\Lua %L%\crc32fast %CLIBWHOLE% %WHOLE%\src
set ROOTS_FREEBSD=%ROOTS_LINUX%
set TESTS=--tests %L%\UnitTest++ %WHOLE%\tests
set CONFIG_debug=
set CONFIG_release=--release
set CONFIG_final=--release --config finalRelease=true
if exist kpp\implicit-uses rd /s /q kpp\implicit-uses
mkdir kpp\implicit-uses
set USES=
set FAILED=
for %%p in (WINDOWS LINUX FREEBSD) do for %%c in (debug release final) do for %%t in (tests main) do call :build %%p %%c %%t
if exist kpp\implicit-uses.ll del kpp\implicit-uses.ll
if defined FAILED (
  echo implicit-dropped: FAILED:%FAILED% ^(kpp\implicit-uses\*.errors.txt^)
  exit /b 1
)
python -I implicit-dropped.py "%WHOLE%" overrides\implicit-dropped.txt %USES% || exit /b 1
exit /b 0

:build
if "%2-%3"=="final-tests" exit /b 0
set NAME=%1-%2-%3
call set PLATFORM=%%%1%%
call set ROOTS=%%ROOTS_%1%%
call set CONFIG=%%CONFIG_%2%%
set WITH=
if "%3"=="tests" set WITH=%TESTS%
echo == %NAME%
%KPP% %BASE% %PLATFORM% %CONFIG% --implicit-uses kpp\implicit-uses\%NAME%.txt %ROOTS% %WITH% > nul 2> kpp\implicit-uses\%NAME%.errors.txt
if errorlevel 1 set FAILED=%FAILED% %NAME%
for %%s in (kpp\implicit-uses\%NAME%.errors.txt) do if %%~zs gtr 0 set FAILED=%FAILED% %NAME%
set USES=%USES% kpp\implicit-uses\%NAME%.txt
exit /b 0
