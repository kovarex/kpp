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
rem   given), clang-cl with the library's own flags (so it runs before the library's swap: after it,
rem   build.kpp builds no NAME.lib);
rem - converted by kppc --convert together with the library's sources (FACTORIO\libraries\NAME),
rem   and compiled by kppc. A C library has a directory c-libraries\NAME: it is converted by C's
rem   rules (--c-sources), with c-libraries\NAME as the conversion's overrides (its
rem   expand-macros.txt names the headers whose macros the converted code expands), and its headers
rem   stay C imports, found in the directories c-libraries\NAME\includes.txt names (below
rem   FACTORIO\libraries\NAME). Its inputs.txt names the C interfaces it uses, parsed with it (zlib.h for
rem   minizip), and its links.txt the C libraries both builds link (build\clib\NAME.lib: the C zlib for
rem   minizip, so that the builds differ in minizip alone).
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
rem (build.kpp's name of the library: c-libraries\NAME\clib.txt where it is not NAME, stb_image's stb)
set CLIB=%NAME%
if exist c-libraries\%NAME%\clib.txt for /f "usebackq eol=# delims=" %%n in ("c-libraries\%NAME%\clib.txt") do set CLIB=%%n
set CLIBRARY=%CONVERSION%\build\clib\%CLIB%.lib
if not exist "%CLIBRARY%" (echo %CLIBRARY% is missing: build it with `kpp factorio` in %CONVERSION%& exit /b 2)
set OUT=kpp\harness\%NAME%
set CMODE=
set CINC=
set CLINC=
set CINPUTS=
set CLINKS=
set KLINKS=
if exist c-libraries\%NAME%\links.txt for /f "usebackq eol=# delims=" %%i in ("c-libraries\%NAME%\links.txt") do call set CLINKS=%%CLINKS%% "%CONVERSION%\build\clib\%%i.lib"& call set KLINKS=%%KLINKS%% --lib "%CONVERSION%\build\clib\%%i.lib"
if exist c-libraries\%NAME%\inputs.txt for /f "usebackq eol=# delims=" %%i in ("c-libraries\%NAME%\inputs.txt") do call set CINPUTS=%%CINPUTS%% "%FACTORIO%\libraries\%%i"
if exist c-libraries\%NAME% set CMODE=--c-sources --overrides c-libraries\%NAME%
rem (the library's sources: its directory, or the files its sources.txt names, stb_image's two headers)
set CSOURCES=%FACTORIO%\libraries\%NAME%
if exist c-libraries\%NAME%\sources.txt set CSOURCES=
if exist c-libraries\%NAME%\sources.txt for /f "usebackq eol=# delims=" %%i in ("c-libraries\%NAME%\sources.txt") do call set CSOURCES=%%CSOURCES%% "%FACTORIO%\libraries\%NAME%\%%i"
rem (and the conversion's own headers: the configured imports c-headers.txt names, kpp_*.h)
if exist c-libraries\%NAME% set CINC=%CINC% --cinclude %OUT%\converted\%NAME%
if exist c-libraries\%NAME%\includes.txt for /f "usebackq delims=" %%i in ("c-libraries\%NAME%\includes.txt") do call set CINC=%%CINC%% --cinclude "%FACTORIO%\libraries\%NAME%\%%i"& call set CLINC=%%CLINC%% /I"%CONVERSION%\libraries\%NAME%\%%i"
if exist %OUT% rd /s /q %OUT%
mkdir %OUT%\driver

rem The inputs: one list of files, the same for both builds.
if exist %OUT%\inputs.txt del %OUT%\inputs.txt
type nul > %OUT%\inputs.txt
if exist harness\%NAME%.inputs for /f "usebackq delims=" %%p in ("harness\%NAME%.inputs") do dir /s /b /a-d /on "%CONVERSION%\%%p" >> %OUT%\inputs.txt 2>nul
rem (System32's find: a Unix find earlier on the PATH, Git's, would walk the disk instead)
for /f %%n in ('%SystemRoot%\System32\find.exe /c /v "" ^< %OUT%\inputs.txt') do echo == %NAME%: %%n input files

rem The C or C++ build: the driver compiled with the library's flags (build.kpp's, as kpp recorded
rem them in flags.txt), linked with the library build.kpp built.
rem Without its /c, for one command that compiles and links (with the same /vctoolsdir).
set /p FLAGS=<"%CONVERSION%\build\clib\%CLIB%\flags.txt"
set FLAGS=%FLAGS:/c =%
rem (a driver in C: the library's C++ standard flag is no C one)
if /i "%DRIVER:~-2%"==".c" set FLAGS=%FLAGS:/std:c++20=%
"%CLANGCL%" %FLAGS% /I"%CONVERSION%\libraries" %CLINC% -fuse-ld=lld /Fo%OUT%\ /Fe%OUT%\c.exe %DRIVER% "%CLIBRARY%" %CLINKS% > %OUT%\c-build.txt 2>&1 || (type %OUT%\c-build.txt& exit /b 1)

rem The kpp build: the driver and the library converted together, then compiled. A scoped library
rem (c-libraries\NAME\interface.txt, design.md 16) is converted on its own, as convert.cmd converts it
rem for the game, and the driver with --convert-library, which writes its uses qualified (zstd::...):
rem the library is compiled as the library NAME (--library-root), the driver as the program.
copy /y %DRIVER% %OUT%\driver\ > nul
if exist c-libraries\%NAME%\interface.txt goto :scoped
%KPPC% --no-std --quiet %CMODE% --convert %OUT%\converted --ext .cpp,.hpp,.h,.kpp,.c %OUT%\driver %CSOURCES% %CINPUTS% > %OUT%\convert.txt 2>&1 || (type %OUT%\convert.txt& exit /b 1)
%KPPC% --std ..\..\stdlib --quiet %CINC% %KLINKS% --exe %OUT%\kpp.exe %OUT%\converted > %OUT%\kpp-build.txt 2>&1 || (type %OUT%\kpp-build.txt& exit /b 1)
goto :run
:scoped
%KPPC% --no-std --quiet %CMODE% --convert %OUT%\library --ext .c,.h,.hpp %CSOURCES% %CINPUTS% > %OUT%\convert.txt 2>&1 || (type %OUT%\convert.txt& exit /b 1)
%KPPC% --no-std --quiet --c-sources --convert-library %NAME% %OUT%\library\%NAME% --convert %OUT%\converted --ext .cpp,.c %OUT%\driver >> %OUT%\convert.txt 2>&1 || (type %OUT%\convert.txt& exit /b 1)
%KPPC% --std ..\..\stdlib --quiet %CINC% %KLINKS% --library-root %NAME% %OUT%\library\%NAME% --exe %OUT%\kpp.exe %OUT%\converted > %OUT%\kpp-build.txt 2>&1 || (type %OUT%\kpp-build.txt& exit /b 1)
:run

%OUT%\c.exe %OUT%\inputs.txt > %OUT%\c-output.txt 2> %OUT%\c-errors.txt
set CEXIT=%errorlevel%
%OUT%\kpp.exe %OUT%\inputs.txt > %OUT%\kpp-output.txt 2> %OUT%\kpp-errors.txt
set KEXIT=%errorlevel%
for /f %%n in ('%SystemRoot%\System32\find.exe /c /v "" ^< %OUT%\c-output.txt') do set LINES=%%n
fc /b %OUT%\c-output.txt %OUT%\kpp-output.txt > nul
if errorlevel 1 (
  echo == %NAME%: OUTPUTS DIFFER ^(exit %CEXIT% and %KEXIT%^): %OUT%\c-output.txt, %OUT%\kpp-output.txt
  fc /lb10 %OUT%\c-output.txt %OUT%\kpp-output.txt
  exit /b 1
)
if not "%CEXIT%"=="%KEXIT%" (echo == %NAME%: the same output, but exit %CEXIT% and %KEXIT%& exit /b 1)
echo == %NAME%: identical, %LINES% lines (exit %CEXIT%)
exit /b 0
