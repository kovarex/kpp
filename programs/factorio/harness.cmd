@echo off
rem A library's differential harness (docs/path-to-factorio.md, "Converting the C libraries"): the
rem gate a converted library passes before factorio-in-kpp's build.kpp links it instead of the C one.
rem
rem   harness.cmd NAME [CONVERSION]
rem
rem harness\NAME.cpp (or .c) is a driver that feeds the library inputs and prints what it gets back.
rem It is built twice:
rem - with the C or C++ library as the game links it today: CONVERSION\build\clib\NAME.lib, which
rem   `kpp factorio` builds in the factorio-in-kpp checkout CONVERSION (C:\factorio-in-kpp unless
rem   given), clang-cl with the library's own flags;
rem - converted by kppc --convert together with the library's sources (FACTORIO\libraries\NAME),
rem   and compiled by kppc.
rem Both run on the same inputs, the files under CONVERSION that harness\NAME.inputs names (one
rem `dir` pattern per line, such as data\*.ogg), and their outputs must be byte-identical.
rem Everything is written to kpp\harness\NAME. Exit 0 only when the outputs match.
setlocal
if "%~1"=="" (echo usage: harness.cmd NAME [CONVERSION]& exit /b 2)
set NAME=%~1
set CONVERSION=C:\factorio-in-kpp
if not "%~2"=="" set CONVERSION=%~f2
if not defined FACTORIO set FACTORIO=C:\Factorio
cd /d "%~dp0"
set KPPC=..\..\compiler\build\kppc.exe
set CLANGCL=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe
set DRIVER=harness\%NAME%.cpp
if not exist %DRIVER% set DRIVER=harness\%NAME%.c
if not exist %DRIVER% (echo no harness\%NAME%.cpp or .c& exit /b 2)
set CLIBRARY=%CONVERSION%\build\clib\%NAME%.lib
if not exist "%CLIBRARY%" (echo %CLIBRARY% is missing: build it with `kpp factorio` in %CONVERSION%& exit /b 2)
set OUT=kpp\harness\%NAME%
if exist %OUT% rd /s /q %OUT%
mkdir %OUT%\driver

rem The inputs: one list of files, the same for both builds.
if exist %OUT%\inputs.txt del %OUT%\inputs.txt
type nul > %OUT%\inputs.txt
if exist harness\%NAME%.inputs for /f "usebackq delims=" %%p in ("harness\%NAME%.inputs") do dir /s /b /a-d /on "%CONVERSION%\%%p" >> %OUT%\inputs.txt 2>nul
for /f %%n in ('find /c /v "" ^< %OUT%\inputs.txt') do echo == %NAME%: %%n input files

rem The C or C++ build: the driver compiled with the library's flags (build.kpp's, as kpp recorded
rem them in flags.txt), linked with the library build.kpp built.
rem Without its /c, for one command that compiles and links (with the same /vctoolsdir).
set /p FLAGS=<"%CONVERSION%\build\clib\%NAME%\flags.txt"
set FLAGS=%FLAGS:/c =%
"%CLANGCL%" %FLAGS% /I"%CONVERSION%\libraries" -fuse-ld=lld /Fo%OUT%\ /Fe%OUT%\c.exe %DRIVER% "%CLIBRARY%" > %OUT%\c-build.txt 2>&1 || (type %OUT%\c-build.txt& exit /b 1)

rem The Kpp build: the driver and the library converted together, then compiled.
copy /y %DRIVER% %OUT%\driver\ > nul
%KPPC% --no-std --quiet --convert %OUT%\converted --ext .cpp,.hpp,.h,.kpp,.c %OUT%\driver %FACTORIO%\libraries\%NAME% > %OUT%\convert.txt 2>&1 || (type %OUT%\convert.txt& exit /b 1)
%KPPC% --std ..\..\stdlib --quiet --exe %OUT%\kpp.exe %OUT%\converted > %OUT%\kpp-build.txt 2>&1 || (type %OUT%\kpp-build.txt& exit /b 1)

%OUT%\c.exe %OUT%\inputs.txt > %OUT%\c-output.txt 2> %OUT%\c-errors.txt
set CEXIT=%errorlevel%
%OUT%\kpp.exe %OUT%\inputs.txt > %OUT%\kpp-output.txt 2> %OUT%\kpp-errors.txt
set KEXIT=%errorlevel%
for /f %%n in ('find /c /v "" ^< %OUT%\c-output.txt') do set LINES=%%n
fc /b %OUT%\c-output.txt %OUT%\kpp-output.txt > nul
if errorlevel 1 (
  echo == %NAME%: OUTPUTS DIFFER ^(exit %CEXIT% and %KEXIT%^): %OUT%\c-output.txt, %OUT%\kpp-output.txt
  fc /lb10 %OUT%\c-output.txt %OUT%\kpp-output.txt
  exit /b 1
)
if not "%CEXIT%"=="%KEXIT%" (echo == %NAME%: the same output, but exit %CEXIT% and %KEXIT%& exit /b 1)
echo == %NAME%: identical, %LINES% lines (exit %CEXIT%)
exit /b 0
