@echo off
rem Converts the whole Factorio source (CommonUtil, Agui, src, and the tests with UnitTest++, the
rem unit-test library they are written in) to kpp into a factorio-in-kpp checkout (the directory given
rem as the first argument, else FACTORIO_IN_KPP: a thread's own copy, setup-checkout.cmd; C:\factorio-in-kpp
rem only for a factorio-in-kpp push, committed there by hand), each file at its path in
rem Factorio's repository (kpp's --convert-base): src\, tests\ and libraries\CommonUtil\ ... With it,
rem the C and C++ shims the game links (native\) into libraries\kpp-native\. What the game takes from
rem Factorio unchanged (data\, shaders\, the other libraries, the tests' saves) resources.cmd copies. Then converts the CommonUtil and Agui libraries and the game's Util module again on their
rem own (into kpp\, with the hand-written files kept there), checks that kpp compiles them, and runs
rem the smoke tests against that result: the stubs in tests\stubs\ stand in for the modules a whole
rem conversion pulls in (FloatCast.cpp merged into FloatCast.kpp needs global and Logging, for example).
rem Then imgui frames through the generated imgui interface (native\generate_imgui.py), the glm port
rem against glm, the converted Lua against the C++ build's (tests\lua_compat.lua, the gate of Lua's
rem conversion), and the GDI+ classes (overrides\added\src\Graphics\GdiplusClasses.kpp) against the
rem SDK's gdiplus.h, and RE2 through the RegexUtil override (tests\regex_smoke.kpp) with the game's re2.lib.
rem Last, the converter's own tests: the sizes a serialiser writes (tests\convert_sizes\sizes.cpp),
rem std::variant as typed unions (tests\convert_variant\variant.cpp) and Factorio's own printf
rem mismatches made explicit (tests\convert_formats\formats.cpp), whose conversions print what their
rem C++ builds print, and the rules where C and C++ differ (tests\convert_c\rules.c, pending), whose
rem conversion as C prints what its C build prints.
rem Hand-written parts are in overrides\.
rem The last line sums it up, and the exit code agrees with it: `convert: ALL PASSED (headline 0, release
rem errors 0, final errors 0, tests errors 0, smoke tests 16/16, C rules pending)` and 0; or `convert:
rem FAILED: STEP` (the first step that failed; the run stops there) or `convert: FAILED: headline 3,
rem release errors 0, ...` and 1.
setlocal
rem The Factorio source tree read (src, libraries, tests, data): C:\Factorio unless FACTORIO names another,
rem as a snapshot of an older commit taken with git archive. Its prebuilt libraries and executables
rem (vs2022\, bin\) are always C:\Factorio's.
if not defined FACTORIO set FACTORIO=C:\Factorio
cd /d "%~dp0"
set KPP=..\..\compiler\build\kpp.exe
set SMOKES=0
set STEP=the whole conversion
set SRC=%FACTORIO%\src
rem The checkout written: the first argument, else FACTORIO_IN_KPP; none is an error. Each thread
rem writes its own copy (setup-checkout.cmd); C:\factorio-in-kpp, which every thread reads, only for
rem a factorio-in-kpp push.
set WHOLE=%FACTORIO_IN_KPP%
if not "%~1"=="" set WHOLE=%~1
if "%WHOLE%"=="" goto :nocheckout
rem Factorio's src\Info\BuildVersion.hpp is generated (FASTBuild runs scripts\build\generate_build_version.bat
rem and git ignores the file): a fresh clone has none, and its VERSION_STRING would be 42 unresolved names.
if not exist "%FACTORIO%\src\Info\BuildVersion.hpp" goto :nobuildversion
rem Lua (libraries\Lua): its .c files as the C++ the game compiles them as, with the game's C++
rem overloads of its API (LuaCPPUtilities.hpp), into kpp\Lua\ (docs/path-to-factorio.md, "Converting
rem Lua"). No other input has .c files.
set LUA=%FACTORIO%\libraries\Lua
set EXT=--ext .cpp,.hpp,.h,.kpp,.c
rem The C libraries (c-libraries.cmd), first: each converted on its own by C's rules into
rem kpp\clib\NAME\ (its report there), then copied into the checkout's libraries\NAME\ and into
rem kpp\NAME\. Their headers stay C, but a scoped library's interface (CLIBSCOPED, design.md 16): the
rem checkout has them from resources.cmd, and the builds import them (CLIBINC). A scoped library is a
rem library of the builds (--library-root, and its build.kpp, c-libraries\NAME\build.kpp, in the
rem checkout), and the game's conversions write its names qualified (--convert-library, CLIBUSE).
set STEP=the C libraries
call "%~dp0c-libraries.cmd" || goto :failed
set CLIBKPP=
set CLIBWHOLE=
set CLIBUSE=
rem Each converted for Windows, Linux and FreeBSD (unless CONVERT_LINUX=0), Linux's and FreeBSD's made one by
rem config::platform, then that with Windows' (--convert-merge-platform, design.md 12), the marks taken out by
rem --convert-merge of that text with itself.
set LINUXCONVERT=--convert-platform linux --convert-predefines linux-predefines.txt
set FREEBSDCONVERT=--convert-platform freebsd --convert-predefines freebsd-predefines.txt
for %%d in (clib clib-w clib-l clib-f clib-p clib-b) do if exist kpp\%%d rd /s /q kpp\%%d
for %%l in (%CLIBRARIES%) do (
  call :libraryUses %%l
  call %KPP% --no-std --quiet --c-sources --convert-marks --overrides c-libraries\%%l %%LIBUSE%% --convert kpp\clib-w\%%l --convert-base %FACTORIO% --ext .c,.h,.hpp %%CLIBSRC_%%l%% %%CLIBIN_%%l%% || goto :clibfailed
  if "%CONVERT_LINUX%"=="0" (xcopy /s /y /q /i kpp\clib-w\%%l kpp\clib-b\%%l >nul) else (
    call %KPP% --no-std --quiet --c-sources --convert-marks %LINUXCONVERT% --overrides c-libraries\%%l %%LIBUSE%% --convert kpp\clib-l\%%l --convert-base %FACTORIO% --ext .c,.h,.hpp %%CLIBSRC_%%l%% %%CLIBIN_%%l%% || goto :clibfailed
    call %KPP% --no-std --quiet --c-sources --convert-marks %FREEBSDCONVERT% --overrides c-libraries\%%l %%LIBUSE%% --convert kpp\clib-f\%%l --convert-base %FACTORIO% --ext .c,.h,.hpp %%CLIBSRC_%%l%% %%CLIBIN_%%l%% || goto :clibfailed
    %KPP% --convert-merge-platform kpp\clib-l\%%l kpp\clib-f\%%l kpp\clib-p\%%l Linux FreeBSD > kpp\clib-p-%%l.log 2>&1 || goto :clibmergefailed
    %KPP% --convert-merge-platform kpp\clib-w\%%l kpp\clib-p\%%l kpp\clib-b\%%l Windows Linux,FreeBSD > kpp\clib-b-%%l.log 2>&1 || goto :clibmergefailed
  )
  %KPP% --convert-merge kpp\clib-b\%%l kpp\clib-b\%%l kpp\clib\%%l > kpp\clib-%%l.log 2>&1 || goto :clibmergefailed
  if exist "%WHOLE%\libraries\%%l" del /s /q "%WHOLE%\libraries\%%l\*.kpp" >nul
  xcopy /s /y /q /i kpp\clib\%%l\libraries\%%l "%WHOLE%\libraries\%%l" >nul
  if exist kpp\%%l rd /s /q kpp\%%l
  xcopy /s /y /q /i kpp\clib\%%l\libraries\%%l kpp\%%l >nul
  if exist c-libraries\%%l\interface.txt (
    copy /y c-libraries\%%l\build.kpp "%WHOLE%\libraries\%%l" >nul
    call set CLIBKPP=%%CLIBKPP%% --library-root %%l kpp\%%l
    call set CLIBWHOLE=%%CLIBWHOLE%% --library-root %%l %WHOLE%\libraries\%%l --exclude %WHOLE%\libraries\%%l\build.kpp
    call set CLIBUSE=%%CLIBUSE%% --convert-library %%l kpp\clib\%%l\libraries\%%l
  ) else (
    call set CLIBKPP=%%CLIBKPP%% kpp\%%l
    call set CLIBWHOLE=%%CLIBWHOLE%% %WHOLE%\libraries\%%l
  )
)
echo C libraries converted: %CLIBRARIES% (scoped:%CLIBSCOPED%)
set STEP=the whole conversion
rem Fresh every run, as kpp\ below: a file an older conversion wrote and this one does not (a header
rem no file includes any more, as ShaderProgram.hpp) would stay in the tree and be built with it. The
rem conversion's files only: tests\ and the libraries keep resources.cmd's (saves, READMEs).
if exist "%WHOLE%\src" rd /s /q "%WHOLE%\src"
for %%d in (libraries\CommonUtil libraries\Agui libraries\cxxopts libraries\StackWalker libraries\Lua libraries\crc32fast libraries\UnitTest++ tests) do if exist "%WHOLE%\%%d" del /s /q "%WHOLE%\%%d\*.kpp" >nul
rem Both configurations (docs/design.md 12, "The game's DEBUG as config::debug"): the game as its
rem FastDebug and its Release preprocess it, each #if group marked, made one text by --convert-merge,
rem a group whose two texts differ written as `if constexpr (config::debug)`
rem (overrides\configuration-choices.txt: the few no `if constexpr` can choose between).
for %%c in (debug release final) do if exist kpp\whole-%%c rd /s /q kpp\whole-%%c
%KPP% --no-std --quiet --convert-marks --convert-native-tests --convert kpp\whole-debug --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-native-tests --convert-release --convert kpp\whole-release --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
rem And the official builds' FinalRelease (docs/final-release.md): Release's defines with FINAL_RELEASE,
rem PACKAGED_BUILD and ALL_IN_ONE, a group whose text it alone has written with config::finalRelease.
%KPP% --no-std --quiet --convert-marks --convert-native-tests --convert-final --convert kpp\whole-final --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
rem The same three for Linux (docs/design.md 12, "The game's platforms as config::platform"; CONVERT_LINUX=0
rem leaves it out, the checkout then Windows' alone)
rem (--convert-platform linux, clang's predefines in linux-predefines.txt; StackWalker is Windows' alone), each configuration's two made
rem one by config::platform (--convert-merge-platform), and those three made one by the configurations.
set WHOLEIN=kpp\whole
if "%CONVERT_LINUX%"=="0" goto :wholemerge
set STEP=the Linux conversion of the whole
for %%c in (debug release final) do if exist kpp\whole-linux-%%c rd /s /q kpp\whole-linux-%%c
for %%c in (debug release final) do if exist kpp\whole-both-%%c rd /s /q kpp\whole-both-%%c
for %%c in (debug release final) do if exist kpp\whole-freebsd-%%c rd /s /q kpp\whole-freebsd-%%c
for %%c in (debug release final) do if exist kpp\whole-posix-%%c rd /s /q kpp\whole-posix-%%c
set LINUXCONVERT=--convert-platform linux --convert-predefines linux-predefines.txt
set FREEBSDCONVERT=--convert-platform freebsd --convert-predefines freebsd-predefines.txt
%KPP% --no-std --quiet --convert-marks --convert-native-tests %LINUXCONVERT% --convert kpp\whole-linux-debug --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-native-tests %FREEBSDCONVERT% --convert kpp\whole-freebsd-debug --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-native-tests %LINUXCONVERT% --convert-release --convert kpp\whole-linux-release --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-native-tests %FREEBSDCONVERT% --convert-release --convert kpp\whole-freebsd-release --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-native-tests %LINUXCONVERT% --convert-final --convert kpp\whole-linux-final --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-native-tests %FREEBSDCONVERT% --convert-final --convert kpp\whole-freebsd-final --convert-base %FACTORIO% %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || goto :failed
set STEP=the platforms made one
for %%c in (debug release final) do %KPP% --convert-merge-platform kpp\whole-linux-%%c kpp\whole-freebsd-%%c kpp\whole-posix-%%c Linux FreeBSD > kpp\whole-posix-%%c.log 2>&1 || (type kpp\whole-posix-%%c.log& goto :failed)
for %%c in (debug release final) do %KPP% --convert-merge-platform kpp\whole-%%c kpp\whole-posix-%%c kpp\whole-both-%%c Windows Linux,FreeBSD > kpp\whole-both-%%c.log 2>&1 || (type kpp\whole-both-%%c.log& goto :failed)
set WHOLEIN=kpp\whole-both
:wholemerge
%KPP% --convert-merge %WHOLEIN%-debug %WHOLEIN%-release %WHOLE% overrides\configuration-choices.txt %WHOLEIN%-final || goto :failed
type %WHOLE%\CONVERSION.md | findstr /B "Files"
set STEP=the debugger rules
rem The game's own Visual Studio rules (utils\tools\vs15\Visualizers\factorio.natvis) for the converted
rem types: tools\factorio.natvis, which the checkout's build.kpp gives both executables.
python -I natvis.py %FACTORIO% %WHOLE% %WHOLE%\tools\factorio.natvis || goto :failed
set STEP=the shims for the checkout
rem The shims, for the build of the checkout (its build.kpp): the sources native\build.cmd compiles,
rem with the headers they include from Factorio's src\ (which the checkout has as kpp only) and the
rem manifest and icons factorio.rc names, at paths relative to libraries\kpp-native.
set NATIVE=%WHOLE%\libraries\kpp-native
if exist "%NATIVE%" rd /s /q "%NATIVE%"
mkdir "%NATIVE%\include\LED\Logitech\Includes"
for %%f in (native\*.c native\*.cpp) do copy /y %%f "%NATIVE%" >nul
xcopy /s /y /q /i native\include "%NATIVE%\include" >nul
copy /y %FACTORIO%\src\LED\Logitech\Includes\LogitechLEDLib.h "%NATIVE%\include\LED\Logitech\Includes" >nul || goto :failed
copy /y %FACTORIO%\fastbuild\win\Factorio.manifest "%NATIVE%" >nul || goto :failed
rem (and its icons beside it: a thread's copy has no data\, setup-checkout.cmd)
for %%i in (factorio.ico factorio-icon.png) do copy /y %FACTORIO%\data\core\graphics\%%i "%NATIVE%" >nul || goto :failed
powershell -NoProfile -Command "(Get-Content -Raw native\factorio.rc).Replace('C:/Factorio/fastbuild/win/', '').Replace('C:/Factorio/data/core/graphics/', '') | Set-Content -NoNewline -Encoding ascii '%NATIVE%\factorio.rc'" || goto :failed
powershell -NoProfile -ExecutionPolicy Bypass -File tests\native-rc.ps1 -Native "%NATIVE%" || goto :failed
set STEP=the conversion into kpp\
set CINC=--cinclude %FACTORIO%\libraries\SDL3\include --cinclude %FACTORIO%\libraries\SDL_Mixer\include --cinclude %FACTORIO%\libraries\Lua --cinclude %FACTORIO%\libraries --cinclude %FACTORIO%\libraries\curl\include --cinclude %FACTORIO%\libraries\glad\include --cinclude %FACTORIO%\libraries\trio --cinclude native\include
rem Fresh every run: a file an older conversion wrote and this one does not (a header no file includes
rem any more) would still be compiled.
for %%d in (CommonUtil Agui cxxopts StackWalker Lua crc32fast src) do if exist kpp\%%d rd /s /q kpp\%%d
for %%c in (debug release final) do if exist kpp\conv-%%c rd /s /q kpp\conv-%%c
%KPP% --no-std --quiet --convert-marks --convert kpp\conv-debug %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-release --convert kpp\conv-release %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-final --convert kpp\conv-final %EXT% --overrides overrides %CLIBUSE% %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% || goto :failed
%KPP% --convert-merge kpp\conv-debug kpp\conv-release kpp overrides\configuration-choices.txt kpp\conv-final || goto :failed
type kpp\CONVERSION.md | findstr /B "Files -"
set CINC=%CINC% %CLIBINC%
rem (and their own headers: the configured imports, minizip's)
for %%l in (%CLIBRARIES%) do call set CINC=%%CINC%% --cinclude kpp\%%l
rem Remove stale top-level directories left by older conversions (the converter now writes src\
rem files under kpp\src\; a stale kpp\Map\ would shadow the correctly renamed kpp\src\Map\).
rem Keep CommonUtil, Agui, Util (from the libraries and the smoke tests) and src itself.
if exist kpp\src for /d %%d in (kpp\src\*) do if exist "kpp\%%~nxd" if /i not "%%~nxd"=="src" if /i not "%%~nxd"=="CommonUtil" if /i not "%%~nxd"=="Agui" if /i not "%%~nxd"=="Util" rd /s /q "kpp\%%~nxd"
rem Remove stale top-level .kpp files that are also in kpp\src\ (from older flat conversions);
rem the full-src build (kpp ... kpp) would see both copies and hit duplicate definitions.
rem Exempt files the smoke tests reference explicitly at kpp\*.kpp (they are not in kpp\src\
rem when they originated from the libraries or stubs).
if exist kpp\src for %%f in (kpp\src\*.kpp) do if exist "kpp\%%~nxf" del /q "kpp\%%~nxf"
rem Mirror kpp\src\Util\ to kpp\Util\ so the smoke tests find their files (the converter writes
rem src files under kpp\src\ but the smoke tests reference kpp\Util\). Fresh every run: a copy
rem kept from an older conversion would test stale code.
if exist kpp\Util rd /s /q kpp\Util
if exist kpp\src\Util xcopy /s /y /q /i kpp\src\Util kpp\Util >nul
rem Copy stubs for types that are outside the converted subset but needed for codegen.
if exist stubs\kpp xcopy /s /y /q stubs\kpp kpp\ >nul
rem The nullptr pass (kovarex, 2026-11-04): each literal 0 that converts to a pointer or member pointer,
rem as kpp's --zero-null-warnings finds it with kpp's own types, written nullptr (fix-null.py), in kpp\
rem and in the checkout (a build through codegen, where `return 0;` is reported too). The headline and the
rem tests' count below take the warning too: one left is a line.
set STEP=the nullptr pass
%KPP% --std ..\..\stdlib %CINC% --zero-null-warnings --diagnostic-columns --emit-llvm kpp\zero-null.ll kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast %CLIBKPP% kpp\src 2> kpp\zero-null.txt
python -I fix-null.py kpp\zero-null.txt kpp || goto :failed
set L=%WHOLE%\libraries
%KPP% --std ..\..\stdlib %CINC% --ext .kpp --tests --zero-null-warnings --diagnostic-columns --emit-llvm kpp\zero-null-whole.ll %L%\CommonUtil %L%\Agui %L%\cxxopts %L%\StackWalker %L%\Lua %L%\crc32fast %CLIBWHOLE% %L%\UnitTest++ %WHOLE%\src %WHOLE%\tests 2> kpp\zero-null-whole.txt
python -I fix-null.py kpp\zero-null-whole.txt "%WHOLE%" || goto :failed
rem And the Linux branches: Linux's build of the merged checkout (until it compiles, its other errors
rem are B's list; the warnings of the code it reaches are written all the same).
rem With the game's Linux sysroot (the -dev packages of linux-packages.txt: libudev, EGL, X11, dbus...),
rem made once by tools\sysroot\linux.ps1, and the Linux build's own include (libbacktrace).
set GAMESYSROOT=..\..\compiler\build\sysroots\linux-x86_64-factorio
if not "%CONVERT_LINUX%"=="0" if not exist %GAMESYSROOT%\kpp-sysroot.txt powershell -NoProfile -ExecutionPolicy Bypass -File ..\..\tools\sysroot\linux.ps1 -ExtraPackages linux-packages.txt -Out %GAMESYSROOT% > kpp\sysroot.log 2>&1 || (type kpp\sysroot.log& goto :failed)
if not "%CONVERT_LINUX%"=="0" (
  %KPP% --std ..\..\stdlib %CINC% --cinclude %FACTORIO%\libraries\libbacktrace --target linux --sysroot %GAMESYSROOT% --ext .kpp --tests --zero-null-warnings --diagnostic-columns --emit-llvm kpp\zero-null-linux.ll %L%\CommonUtil %L%\Agui %L%\cxxopts %L%\Lua %L%\crc32fast %CLIBWHOLE% %L%\UnitTest++ %WHOLE%\src %WHOLE%\tests 2> kpp\zero-null-linux.txt
  python -I fix-null.py kpp\zero-null-linux.txt "%WHOLE%" || goto :failed
)
rem The headline number (docs/path-to-factorio.md, "The headline number: how it is measured"):
rem stderr lines of the full build of exactly CommonUtil, Agui, cxxopts, StackWalker, Lua, crc32fast, the C
rem libraries and src, not the kpp\Util copy.
echo == full-src codegen errors: kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast%CLIBKPP% kpp\src (all errors in kpp\full-errors.txt)
%KPP% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --zero-null-warnings --emit-llvm kpp\full.ll kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast %CLIBKPP% kpp\src 2> kpp\full-errors.txt
set FULLRC=%errorlevel%
rem System32's find: a Unix find earlier on the PATH (Git's) would walk the disk instead.
%SystemRoot%\System32\find.exe /c /v "" kpp\full-errors.txt
for /f %%n in ('type kpp\full-errors.txt ^| %SystemRoot%\System32\find.exe /c /v ""') do set HEADLINE=%%n
if not "%FULLRC%"=="0" if not "%FULLRC%"=="1" echo NOT A VALID COUNT: kpp crashed (exit %FULLRC%)& set HEADLINE=not valid ^(kpp exit %FULLRC%^)
findstr /c:"importing C headers failed" kpp\full-errors.txt >nul && (echo NOT A VALID COUNT: importing the C headers failed, see kpp\full-errors.txt& set HEADLINE=not valid, importing the C headers failed)
rem The same build in release and in final, which the headline's debug build does not see: 2026-11-06, a
rem class with a debug-only operator= met Vector's assignments, the game built in neither, and every gate
rem passed. --check reports every error a build reports and writes nothing, about 20 s each. Gates as
rem the headline is (0): every stderr line.
echo == release and final checks: the headline's build with --check (errors in kpp\release-errors.txt, kpp\final-errors.txt)
%KPP% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --zero-null-warnings --check --release kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast %CLIBKPP% kpp\src > kpp\release-check.txt 2> kpp\release-errors.txt
set RELEASERC=%errorlevel%
%KPP% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --zero-null-warnings --check --release --config finalRelease=true kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast %CLIBKPP% kpp\src > kpp\final-check.txt 2> kpp\final-errors.txt
set FINALRC=%errorlevel%
for /f %%n in ('type kpp\release-errors.txt ^| %SystemRoot%\System32\find.exe /c /v ""') do set RELEASEERRORS=%%n
for /f %%n in ('type kpp\final-errors.txt ^| %SystemRoot%\System32\find.exe /c /v ""') do set FINALERRORS=%%n
if not "%RELEASERC%"=="0" if "%RELEASEERRORS%"=="0" set RELEASEERRORS=not valid ^(kpp exit %RELEASERC%^)
if not "%FINALRC%"=="0" if "%FINALERRORS%"=="0" set FINALERRORS=not valid ^(kpp exit %FINALRC%^)
echo release errors %RELEASEERRORS%, final errors %FINALERRORS%
rem The tests' count, a gate as the headline is (0): the same errors in the whole conversion
rem (%WHOLE%) with UnitTest++ and the tests, which the headline leaves out. Each is a function that
rem tests.cmd's --stub-missing build makes a stub of, so a test would not run what C++ runs.
echo == tests codegen errors: %WHOLE% with UnitTest++ and tests (all errors in kpp\tests-errors.txt)
set L=%WHOLE%\libraries
%KPP% --std ..\..\stdlib %CINC% --tests --ext .kpp --errors 0 --unresolved 0 --zero-null-warnings --emit-llvm kpp\tests-full.ll %L%\CommonUtil %L%\Agui %L%\cxxopts %L%\StackWalker %L%\Lua %L%\crc32fast %CLIBWHOLE% %L%\UnitTest++ %WHOLE%\src %WHOLE%\tests 2> kpp\tests-errors.txt
set TESTSRC=%errorlevel%
%SystemRoot%\System32\find.exe /c /v "" kpp\tests-errors.txt
for /f %%n in ('type kpp\tests-errors.txt ^| %SystemRoot%\System32\find.exe /c /v ""') do set TESTSERRORS=%%n
if not "%TESTSRC%"=="0" if not "%TESTSRC%"=="1" echo NOT A VALID COUNT: kpp crashed (exit %TESTSRC%)& set TESTSERRORS=not valid ^(kpp exit %TESTSRC%^)
set "STEP=smoke test"
echo == smoke test: tests\smoke.kpp against the converted CommonUtil (SystemClipboard stubbed: the real one needs SDL)
setlocal EnableDelayedExpansion
rem (CommonUtil's StringUtil prints through trio: the scoped library converted, trio::trio_snprintf, or
rem the C one)
set TRIOLINK=--lib C:\Factorio\vs2022\Releasex64\trio.lib
if exist c-libraries\trio\interface.txt set TRIOLINK=--library-root trio kpp\trio
set FILES=
for %%f in (kpp\CommonUtil\*.kpp) do if not %%~nf==SystemClipboard set FILES=!FILES! %%f
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\smoke.exe !FILES! tests\stubs\SystemClipboard.kpp tests\smoke.kpp %TRIOLINK% || goto :failed
kpp\smoke.exe > kpp\smoke.out.txt
fc /w kpp\smoke.out.txt tests\smoke.expected > nul && (echo smoke test: output as expected& set /a SMOKES+=1) || (echo smoke test: OUTPUT DIFFERS & fc /w kpp\smoke.out.txt tests\smoke.expected & goto :failed)
set "STEP=Agui smoke test"
echo == Agui smoke test: tests\agui_smoke.kpp against the converted Agui (Void backend)
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\agui_smoke.exe !FILES! tests\stubs\SystemClipboard.kpp tests\stubs\AguiGameWidgets.kpp kpp\Agui tests\agui_smoke.kpp %TRIOLINK% || goto :failed
kpp\agui_smoke.exe > kpp\agui_smoke.out.txt
fc /w kpp\agui_smoke.out.txt tests\agui_smoke.expected > nul && (echo agui smoke test: output as expected& set /a SMOKES+=1) || (echo agui smoke test: OUTPUT DIFFERS & fc /w kpp\agui_smoke.out.txt tests\agui_smoke.expected & goto :failed)
set "STEP=Util smoke test"
echo == Util smoke test: tests\util_smoke.kpp against the converted Util subset (containers, sorting, the math expression evaluator; Logging, Util helpers, Math, FloatCast, GameRuntimeError and prefetch stubbed)
set UFILES=
for %%f in (kpp\Util\Container\*.kpp) do if not %%~nf==ExtendedBitBuffer set UFILES=!UFILES! %%f
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\util_smoke.exe !FILES! !UFILES! tests\stubs\SystemClipboard.kpp tests\stubs\Log.kpp tests\stubs\UtilHelpers.kpp tests\stubs\MathStubs.kpp tests\stubs\FloatCast.kpp tests\stubs\UtilTemplateNames.kpp kpp\src\c-imports.kpp kpp\src\Util\ReleaseAssert.kpp kpp\Util\Sort kpp\Util\MathExpressions kpp\Util\BitBuffer.kpp kpp\Util\ToStr.kpp kpp\Util\LogLevel.kpp kpp\src\Typedefs.kpp kpp\src\TypeTraitsExtensions.kpp kpp\src\Graphics\BitmapFormat.kpp kpp\src\Noise\NoiseRegisterIndex.kpp tests\util_smoke.kpp %TRIOLINK% || goto :failed
kpp\util_smoke.exe > kpp\util_smoke.out.txt
fc /w kpp\util_smoke.out.txt tests\util_smoke.expected > nul && (echo util smoke test: output as expected& set /a SMOKES+=1) || (echo util smoke test: OUTPUT DIFFERS & fc /w kpp\util_smoke.out.txt tests\util_smoke.expected & goto :failed)
set "STEP=imgui smoke test"
echo == imgui smoke test: tests\imgui_smoke.kpp through kpp\src\Debug\ImGui.kpp and native\imgui_c.cpp with the game's imgui.lib (the same frames as tests\imgui_smoke.cpp, whose output is the expected one)
call native\build.cmd >nul || goto :failed
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\imgui_smoke.exe kpp\src\Debug\ImGui.kpp tests\imgui_smoke.kpp --lib native\build\factorio-native.lib --lib C:\Factorio\vs2022\Releasex64\imgui.lib --lib -luser32 --lib -limm32 || goto :failed
kpp\imgui_smoke.exe > kpp\imgui_smoke.out.txt
fc /w kpp\imgui_smoke.out.txt tests\imgui_smoke.expected > nul && (echo imgui smoke test: output as expected& set /a SMOKES+=1) || (echo imgui smoke test: OUTPUT DIFFERS & fc /w kpp\imgui_smoke.out.txt tests\imgui_smoke.expected & goto :failed)
set "STEP=glm smoke test"
echo == glm smoke test: tests\glm_smoke.kpp through the glm port in kpp\src\API\glm.kpp (bit for bit what tests\glm_smoke.cpp computes with glm itself)
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\glm_smoke.exe kpp\src\API\glm.kpp tests\glm_smoke.kpp || goto :failed
kpp\glm_smoke.exe > kpp\glm_smoke.out.txt
fc /w kpp\glm_smoke.out.txt tests\glm_smoke.expected > nul && (echo glm smoke test: output as expected& set /a SMOKES+=1) || (echo glm smoke test: OUTPUT DIFFERS & fc /w kpp\glm_smoke.out.txt tests\glm_smoke.expected & goto :failed)
set "STEP=Lua compatibility check"
echo == Lua compatibility check: tests\lua_compat.lua run by the converted Lua (tests\lua_compat.kpp with kpp\Lua) prints what the game's lua.lib prints (tests\lua_compat.expected, from tests\lua_compat.cpp)
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\lua_compat.exe tests\lua_compat.kpp kpp\Lua %TRIOLINK% || goto :failed
kpp\lua_compat.exe tests\lua_compat.lua > kpp\lua_compat.out.txt
fc /w kpp\lua_compat.out.txt tests\lua_compat.expected > nul && (echo Lua compatibility check: output as expected& set /a SMOKES+=1) || (echo Lua compatibility check: OUTPUT DIFFERS & fc /w kpp\lua_compat.out.txt tests\lua_compat.expected & goto :failed)
set "STEP=GDI+ smoke test"
echo == GDI+ smoke test: tests\gdiplus_smoke.kpp through kpp\src\Graphics\GdiplusClasses.kpp and gdiplus.dll (the layouts and results of tests\gdiplus_smoke.cpp, the same against the SDK's gdiplus.h)
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\gdiplus_smoke.exe kpp\src\Graphics\GdiplusClasses.kpp kpp\src\Graphics\DX11\ComInterfaces.kpp tests\gdiplus_smoke.kpp --lib -lgdiplus --lib -lshlwapi || goto :failed
kpp\gdiplus_smoke.exe > kpp\gdiplus_smoke.out.txt
fc /w kpp\gdiplus_smoke.out.txt tests\gdiplus_smoke.expected > nul && (echo GDI+ smoke test: output as expected& set /a SMOKES+=1) || (echo GDI+ smoke test: OUTPUT DIFFERS & fc /w kpp\gdiplus_smoke.out.txt tests\gdiplus_smoke.expected & goto :failed)
set "STEP=regex smoke test"
echo == regex smoke test: tests\regex_smoke.kpp through kpp\src\Util\RegexUtil.kpp and native\re2_c.cpp with the game's re2.lib (RE2's own results: skipped groups, typed targets, Consume, Replace)
%KPP% --std ..\..\stdlib %CINC% --quiet --exe kpp\regex_smoke.exe kpp\src\c-imports.kpp kpp\src\Util\RegexUtil.kpp tests\regex_smoke.kpp --lib native\build\factorio-native.lib --lib C:\Factorio\vs2022\Releasex64\re2.lib || goto :failed
kpp\regex_smoke.exe > kpp\regex_smoke.out.txt
fc /w kpp\regex_smoke.out.txt tests\regex_smoke.expected > nul && (echo regex smoke test: output as expected& set /a SMOKES+=1) || (echo regex smoke test: OUTPUT DIFFERS & fc /w kpp\regex_smoke.out.txt tests\regex_smoke.expected & goto :failed)
set "STEP=size widths"
echo == size widths: tests\convert_sizes\sizes.cpp, converted and run, writes the bytes C++ writes for each std container size a serialiser takes (tests\convert_sizes.expected, from the C++ build of the same file)
if exist kpp\convert_sizes rd /s /q kpp\convert_sizes
%KPP% --no-std --quiet --convert kpp\convert_sizes --ext .cpp tests\convert_sizes || goto :failed
%KPP% --std ..\..\stdlib --quiet --exe kpp\convert_sizes.exe kpp\convert_sizes || goto :failed
kpp\convert_sizes.exe > kpp\convert_sizes.out.txt
fc /w kpp\convert_sizes.out.txt tests\convert_sizes.expected > nul && (echo size widths: output as expected& set /a SMOKES+=1) || (echo size widths: OUTPUT DIFFERS & fc /w kpp\convert_sizes.out.txt tests\convert_sizes.expected & goto :failed)
set "STEP=variants"
echo == variants: tests\convert_variant\variant.cpp, std::variant as the game uses it, converted to a typed union and run, prints what its C++ build prints (tests\convert_variant.expected)
if exist kpp\convert_variant rd /s /q kpp\convert_variant
%KPP% --no-std --quiet --convert kpp\convert_variant --ext .cpp tests\convert_variant || goto :failed
%KPP% --std ..\..\stdlib --quiet --exe kpp\convert_variant.exe kpp\convert_variant || goto :failed
kpp\convert_variant.exe > kpp\convert_variant.out.txt
fc /w kpp\convert_variant.out.txt tests\convert_variant.expected > nul && (echo variants: output as expected& set /a SMOKES+=1) || (echo variants: OUTPUT DIFFERS & fc /w kpp\convert_variant.out.txt tests\convert_variant.expected & goto :failed)
set "STEP=printf formats"
echo == printf formats: tests\convert_formats\formats.cpp, Factorio's own 64-bit arguments read as 32 bits and extra arguments, converted and run, prints what its C++ build prints (tests\convert_formats.expected) and passes the format check
if exist kpp\convert_formats rd /s /q kpp\convert_formats
%KPP% --no-std --quiet --convert kpp\convert_formats --ext .cpp tests\convert_formats || goto :failed
%KPP% --std ..\..\stdlib --quiet --exe kpp\convert_formats.exe kpp\convert_formats || goto :failed
kpp\convert_formats.exe > kpp\convert_formats.out.txt
fc /w kpp\convert_formats.out.txt tests\convert_formats.expected > nul && (echo printf formats: output as expected& set /a SMOKES+=1) || (echo printf formats: OUTPUT DIFFERS & fc /w kpp\convert_formats.out.txt tests\convert_formats.expected & goto :failed)
echo == namespace-scope objects: tests\convert_init\init.cpp, objects initialized by parentheses at namespace and class scope, converted to braces or `= T(args)` (kpp reads `T f(...)` there as a function) and run, prints what its C++ build prints (tests\convert_init.expected)
if exist kpp\convert_init rd /s /q kpp\convert_init
%KPP% --no-std --quiet --convert kpp\convert_init --ext .cpp tests\convert_init || goto :failed
%KPP% --std ..\..\stdlib --quiet --exe kpp\convert_init.exe kpp\convert_init || goto :failed
kpp\convert_init.exe > kpp\convert_init.out.txt
fc /w kpp\convert_init.out.txt tests\convert_init.expected > nul && (echo namespace-scope objects: output as expected& set /a SMOKES+=1) || (echo namespace-scope objects: OUTPUT DIFFERS & fc /w kpp\convert_init.out.txt tests\convert_init.expected & goto :failed)
set "STEP=asserts"
echo == asserts: tests\convert_asserts\asserts.cpp, the game's release_assert written as kpp's release_assert statement (its file, line and text taken where it stands; the macro's handler called through kpp_assert_failed) and a do/while statement macro without its wrapper, converted and run, prints what its C++ build prints (tests\convert_asserts.expected), and its text has no do/while
if exist kpp\convert_asserts rd /s /q kpp\convert_asserts
%KPP% --no-std --quiet --convert kpp\convert_asserts --ext .cpp tests\convert_asserts || goto :failed
%KPP% --std ..\..\stdlib --quiet --exe kpp\convert_asserts.exe kpp\convert_asserts || goto :failed
kpp\convert_asserts.exe > kpp\convert_asserts.out.txt
findstr /c:"while (false)" kpp\convert_asserts\convert_asserts\asserts.kpp > nul && (echo asserts: A DO/WHILE IS LEFT& goto :failed)
findstr /c:"release_assert(this->value == 3);" kpp\convert_asserts\convert_asserts\asserts.kpp > nul || (echo asserts: RELEASE_ASSERT NOT WRITTEN AS KPP'S& goto :failed)
fc /w kpp\convert_asserts.out.txt tests\convert_asserts.expected > nul && (echo asserts: output as expected& set /a SMOKES+=1) || (echo asserts: OUTPUT DIFFERS & fc /w kpp\convert_asserts.out.txt tests\convert_asserts.expected & goto :failed)
echo == member function bodies: tests\convert_inline, Widget.hpp and Widget.cpp converted into one Widget.kpp whose class holds each body at its declaration (no out-of-class Widget::area; Widget::renamed with its declaration taking the definition's parameter names, Widget::shadowed outside, as a default argument names its parameter's name), and run, prints what its C++ build prints (tests\convert_inline.expected)
if exist kpp\convert_inline rd /s /q kpp\convert_inline
%KPP% --no-std --quiet --convert kpp\convert_inline --ext .cpp,.hpp tests\convert_inline || goto :failed
%KPP% --std ..\..\stdlib --quiet --exe kpp\convert_inline.exe kpp\convert_inline || goto :failed
findstr /s /c:"Widget::area(" kpp\convert_inline\*.kpp > nul && (echo member function bodies: Widget::area is still defined outside its class& goto :failed)
findstr /s /c:"Widget::renamed(" kpp\convert_inline\*.kpp > nul && (echo member function bodies: Widget::renamed, a parameter named otherwise in its declaration, is still defined outside its class& goto :failed)
findstr /s /c:"Widget::shadowed(" kpp\convert_inline\*.kpp > nul || (echo member function bodies: Widget::shadowed moved into its class, where its parameter would hide what its default argument names& goto :failed)
kpp\convert_inline.exe > kpp\convert_inline.out.txt
fc /w kpp\convert_inline.out.txt tests\convert_inline.expected > nul && (echo member function bodies: output as expected& set /a SMOKES+=1) || (echo member function bodies: OUTPUT DIFFERS & fc /w kpp\convert_inline.out.txt tests\convert_inline.expected & goto :failed)
set "STEP=platforms check"
echo == platforms: tests\convert_platform\platform.cpp converted for Windows and for Linux ^(--convert-platform linux, clang's predefines in linux-predefines.txt^) in debug and release, each configuration's two made one by config::platform ^(--convert-merge-platform^), then the configurations by config::debug; built and run for Windows ^(tests\convert_platform.expected^) and for Linux under WSL ^(tests\convert_platform.linux.expected^)
if exist kpp\cplat rd /s /q kpp\cplat
set LINUXCONVERT=--convert-platform linux --convert-predefines linux-predefines.txt
%KPP% --no-std --quiet --convert-marks --convert kpp\cplat\wd --ext .cpp tests\convert_platform || goto :failed
%KPP% --no-std --quiet --convert-marks --convert-release --convert kpp\cplat\wr --ext .cpp tests\convert_platform || goto :failed
%KPP% --no-std --quiet --convert-marks %LINUXCONVERT% --convert kpp\cplat\ld --ext .cpp tests\convert_platform || goto :failed
%KPP% --no-std --quiet --convert-marks %LINUXCONVERT% --convert-release --convert kpp\cplat\lr --ext .cpp tests\convert_platform || goto :failed
%KPP% --convert-merge-platform kpp\cplat\wd kpp\cplat\ld kpp\cplat\d > nul || goto :failed
%KPP% --convert-merge-platform kpp\cplat\wr kpp\cplat\lr kpp\cplat\r > nul || goto :failed
%KPP% --convert-merge kpp\cplat\d kpp\cplat\r kpp\cplat\out > nul || goto :failed
%KPP% --std ..\..\stdlib --quiet --exe kpp\cplat\platform.exe kpp\cplat\out || goto :failed
%KPP% --std ..\..\stdlib --quiet --target linux --sysroot ..\..\compiler\build\sysroots\linux-x86_64 --exe kpp\cplat\platform kpp\cplat\out || goto :failed
kpp\cplat\platform.exe > kpp\cplat\windows.txt
wsl.exe -e kpp/cplat/platform > kpp\cplat\linux.txt 2>&1 || (echo platforms: the Linux build did not run under WSL ^(kpp\cplat\linux.txt^)& goto :failed)
fc /w kpp\cplat\windows.txt tests\convert_platform.expected > nul || (echo platforms: WINDOWS OUTPUT DIFFERS & fc /w kpp\cplat\windows.txt tests\convert_platform.expected & goto :failed)
fc /w kpp\cplat\linux.txt tests\convert_platform.linux.expected > nul && (echo platforms: output as expected on Windows and Linux& set /a SMOKES+=1) || (echo platforms: LINUX OUTPUT DIFFERS & fc /w kpp\cplat\linux.txt tests\convert_platform.linux.expected & goto :failed)
set "STEP=configurations check"
echo == configurations: the game's DEBUG code is config::debug's (docs/design.md 12): tests\configurations.kpp with the converted BuildConstants prints IsDebug, IsFinalRelease and IsPackagedBuild in debug, release and final, and debug-only.py finds TransportLine's referenceSet and Rcu's writeLockCount only where config::debug is true, in a when (config::debug) declaration or an if constexpr (config::debug), and IsDebug chosen by config::debug (tests\configurations.expected)
%KPP% --std ..\..\stdlib --quiet --exe kpp\configurations-debug.exe kpp\CommonUtil\FinalRelease.kpp kpp\src\BuildConstants.kpp tests\configurations.kpp || goto :failed
%KPP% --std ..\..\stdlib --quiet --release --exe kpp\configurations-release.exe kpp\CommonUtil\FinalRelease.kpp kpp\src\BuildConstants.kpp tests\configurations.kpp || goto :failed
%KPP% --std ..\..\stdlib --quiet --release --config finalRelease=true --exe kpp\configurations-final.exe kpp\CommonUtil\FinalRelease.kpp kpp\src\BuildConstants.kpp tests\configurations.kpp || goto :failed
kpp\configurations-debug.exe > kpp\configurations.out.txt
kpp\configurations-release.exe >> kpp\configurations.out.txt
kpp\configurations-final.exe >> kpp\configurations.out.txt
python -I debug-only.py kpp >> kpp\configurations.out.txt
fc /w kpp\configurations.out.txt tests\configurations.expected > nul && (echo configurations: output as expected& set /a SMOKES+=1) || (echo configurations: OUTPUT DIFFERS & fc /w kpp\configurations.out.txt tests\configurations.expected & goto :failed)
set "STEP=C rules"
echo == C rules: tests\convert_c\rules.c, where C and C++ differ (void* conversions, designated initializers, compound literals, C++ keywords as names...), converted as C and run, prints what its C build prints (tests\convert_c.expected)
rem Pending until the converter's C mode exists (docs/path-to-factorio.md, "Converting the C libraries"):
rem it is reported, not failed, and a pass while pending fails the run, as spec.cmd's pending snippets do.
set CRULES=pending
if exist kpp\convert_c rd /s /q kpp\convert_c
if exist kpp\convert_c.exe del kpp\convert_c.exe
%KPP% --no-std --quiet --convert kpp\convert_c --ext .c tests\convert_c > kpp\convert_c.log 2>&1
%KPP% --std ..\..\stdlib --quiet --exe kpp\convert_c.exe kpp\convert_c >> kpp\convert_c.log 2>&1
set CRULESOK=0
if exist kpp\convert_c.exe (kpp\convert_c.exe > kpp\convert_c.out.txt 2>&1 && fc /w kpp\convert_c.out.txt tests\convert_c.expected > nul && set CRULESOK=1)
if %CRULES%==pending (if %CRULESOK%==1 (echo C rules: FIXED while pending: remove the pending mark& goto :failed) else (echo C rules: pending, kpp\convert_c.log)) else (if %CRULESOK%==1 (echo C rules: output as expected) else (echo C rules: OUTPUT DIFFERS, kpp\convert_c.log& goto :failed))
rem Every file the conversion wrote ends its lines with LF alone, in the checkout and in kpp\ (a file of
rem both is "inconsistent" to Visual Studio): tests\line-endings.ps1.
set STEP=the line ends (a converted file has a CR)
powershell -NoProfile -ExecutionPolicy Bypass -File tests\line-endings.ps1 -Checkout "%WHOLE%" -Also kpp || goto :failed
rem The converted files read as the program's own: no first line naming the sources, no forward
rem declaration of a class the program defines (tests\converted-text.ps1).
set STEP=the converted text (a source line or a needless forward declaration)
powershell -NoProfile -ExecutionPolicy Bypass -File tests\converted-text.ps1 -Checkout "%WHOLE%" || goto :failed
rem The one line that says whether everything passed: the headline, the release and final checks and
rem the tests' count are gates (0).
set SUMMARY=headline %HEADLINE%, release errors %RELEASEERRORS%, final errors %FINALERRORS%, tests errors %TESTSERRORS%, smoke tests %SMOKES%/16, C rules %CRULES%
if not "%HEADLINE%"=="0" goto :countsfailed
if not "%RELEASEERRORS%"=="0" goto :countsfailed
if not "%FINALERRORS%"=="0" goto :countsfailed
if not "%TESTSERRORS%"=="0" goto :countsfailed
echo convert: ALL PASSED (%SUMMARY%)
exit /b 0
:countsfailed
echo convert: FAILED: %SUMMARY%
exit /b 1
:libraryUses
rem (LIBUSE: the --convert-library options of the scoped libraries C library %1 uses,
rem c-libraries\%1\uses.txt, converted before it: minizip's zlib)
set LIBUSE=
if exist "c-libraries\%1\uses.txt" for /f "usebackq eol=# delims=" %%u in ("c-libraries\%1\uses.txt") do call set LIBUSE=%%LIBUSE%% --convert-library %%u kpp\clib\%%u\libraries\%%u
goto :eof
:nobuildversion
echo convert: %FACTORIO%\src\Info\BuildVersion.hpp is missing: Factorio's build generates it. Run %FACTORIO%\scripts\build\generate_build_version.bat in %FACTORIO% ^(or build Factorio once^), then convert again.
exit /b 2
:nocheckout
echo convert: name the factorio-in-kpp checkout to write: convert.cmd DIR, or set FACTORIO_IN_KPP=DIR. Each thread writes its own copy, made once by setup-checkout.cmd; C:\factorio-in-kpp only for a factorio-in-kpp push.
exit /b 2
:clibmergefailed
for %%f in (kpp\clib-b-*.log kpp\clib-*.log) do type %%f
set STEP=the C libraries made one by platform
goto :failed
:clibfailed
echo convert: a C library's conversion failed
:failed
rem (A top-level exit: a failing `call ... || exit /b 1` inside the C libraries' loop would leave the
rem exit code 0 when convert.cmd runs as `cmd /c convert.cmd`.)
echo convert: FAILED: %STEP%
exit /b 1
