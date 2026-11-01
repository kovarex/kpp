@echo off
rem Converts the whole Factorio source (CommonUtil, Agui, src, and the tests with UnitTest++, the
rem unit-test library they are written in) to Kpp into the factorio-in-kpp checkout (C:\factorio-in-kpp,
rem or the directory given as the first argument; commit there by hand), each file at its path in
rem Factorio's repository (kppc's --convert-base): src\, tests\ and libraries\CommonUtil\ ... With it,
rem the C and C++ shims the game links (native\) into libraries\kpp-native\. What the game takes from
rem Factorio unchanged (data\, shaders\, the other libraries, the tests' saves) resources.cmd copies. Then converts the CommonUtil and Agui libraries and the game's Util module again on their
rem own (into kpp\, with the hand-written files kept there), checks that kppc compiles them, and runs
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
setlocal
rem The Factorio source tree read (src, libraries, tests, data): C:\Factorio unless FACTORIO names another,
rem as a snapshot of an older commit taken with git archive. Its prebuilt libraries and executables
rem (vs2022\, bin\) are always C:\Factorio's.
if not defined FACTORIO set FACTORIO=C:\Factorio
cd /d "%~dp0"
set KPPC=..\..\compiler\build\kppc.exe
set SRC=%FACTORIO%\src
set WHOLE=C:\factorio-in-kpp
if not "%~1"=="" set WHOLE=%~1
rem Lua (libraries\Lua): its .c files as the C++ the game compiles them as, with the game's C++
rem overloads of its API (LuaCPPUtilities.hpp), into kpp\Lua\ (docs/path-to-factorio.md, "Converting
rem Lua"). No other input has .c files.
set LUA=%FACTORIO%\libraries\Lua
set EXT=--ext .cpp,.hpp,.h,.kpp,.c
rem Fresh every run, as kpp\ below: a file an older conversion wrote and this one does not (a header
rem no file includes any more, as ShaderProgram.hpp) would stay in the tree and be built with it. The
rem conversion's files only: tests\ and the libraries keep resources.cmd's (saves, READMEs).
if exist "%WHOLE%\src" rd /s /q "%WHOLE%\src"
for %%d in (libraries\CommonUtil libraries\Agui libraries\cxxopts libraries\StackWalker libraries\Lua libraries\crc32fast libraries\UnitTest++ tests) do if exist "%WHOLE%\%%d" del /s /q "%WHOLE%\%%d\*.kpp" >nul
%KPPC% --no-std --quiet --convert %WHOLE% --convert-base %FACTORIO% %EXT% --overrides overrides %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% %FACTORIO%\libraries\UnitTest++ %FACTORIO%\tests || exit /b 1
type %WHOLE%\CONVERSION.md | findstr /B "Files"
rem The shims, for the build of the checkout (its build.kpp): the sources native\build.cmd compiles,
rem with the headers they include from Factorio's src\ (which the checkout has as Kpp only) and the
rem manifest and icons factorio.rc names, at paths relative to libraries\kpp-native.
set NATIVE=%WHOLE%\libraries\kpp-native
if exist "%NATIVE%" rd /s /q "%NATIVE%"
mkdir "%NATIVE%\include\LED\Logitech\Includes"
for %%f in (native\*.c native\*.cpp) do copy /y %%f "%NATIVE%" >nul
xcopy /y /q native\include "%NATIVE%\include" >nul
copy /y %FACTORIO%\src\LED\Logitech\Includes\LogitechLEDLib.h "%NATIVE%\include\LED\Logitech\Includes" >nul || exit /b 1
copy /y %FACTORIO%\fastbuild\win\Factorio.manifest "%NATIVE%" >nul || exit /b 1
powershell -NoProfile -Command "(Get-Content -Raw native\factorio.rc).Replace('C:/Factorio/fastbuild/win/', '').Replace('C:/Factorio/', '../../') | Set-Content -NoNewline -Encoding ascii '%NATIVE%\factorio.rc'" || exit /b 1
set CINC=--cinclude %FACTORIO%\libraries\SDL3\include --cinclude %FACTORIO%\libraries\SDL_Mixer\include --cinclude %FACTORIO%\libraries\Lua --cinclude %FACTORIO%\libraries --cinclude %FACTORIO%\libraries\curl\include --cinclude %FACTORIO%\libraries\glad\include --cinclude %FACTORIO%\libraries\trio --cinclude native\include
rem Fresh every run: a file an older conversion wrote and this one does not (a header no file includes
rem any more) would still be compiled.
for %%d in (CommonUtil Agui cxxopts StackWalker Lua crc32fast src) do if exist kpp\%%d rd /s /q kpp\%%d
%KPPC% --no-std --quiet --convert kpp %EXT% --overrides overrides %FACTORIO%\libraries\CommonUtil %FACTORIO%\libraries\Agui %FACTORIO%\libraries\cxxopts %FACTORIO%\libraries\StackWalker %LUA% %FACTORIO%\libraries\crc32fast %SRC% || exit /b 1
type kpp\CONVERSION.md | findstr /B "Files -"
rem Remove stale top-level directories left by older conversions (the converter now writes src\
rem files under kpp\src\; a stale kpp\Map\ would shadow the correctly renamed kpp\src\Map\).
rem Keep CommonUtil, Agui, Util (from the libraries and the smoke tests) and src itself.
if exist kpp\src for /d %%d in (kpp\src\*) do if exist "kpp\%%~nxd" if /i not "%%~nxd"=="src" if /i not "%%~nxd"=="CommonUtil" if /i not "%%~nxd"=="Agui" if /i not "%%~nxd"=="Util" rd /s /q "kpp\%%~nxd"
rem Remove stale top-level .kpp files that are also in kpp\src\ (from older flat conversions);
rem the full-src build (kppc ... kpp) would see both copies and hit duplicate definitions.
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
rem The headline number (docs/path-to-factorio.md, "The headline number: how it is measured"):
rem stderr lines of the full build of exactly CommonUtil, Agui, cxxopts, StackWalker, Lua, crc32fast and src, not the kpp\Util copy.
echo == full-src codegen errors: kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast kpp\src (all errors in kpp\full-errors.txt)
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --emit-llvm kpp\full.ll kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast kpp\src 2> kpp\full-errors.txt
set FULLRC=%errorlevel%
rem System32's find: a Unix find earlier on the PATH (Git's) would walk the disk instead.
%SystemRoot%\System32\find.exe /c /v "" kpp\full-errors.txt
if not "%FULLRC%"=="0" if not "%FULLRC%"=="1" echo NOT A VALID COUNT: kppc crashed (exit %FULLRC%)
findstr /c:"importing C headers failed" kpp\full-errors.txt >nul && echo NOT A VALID COUNT: importing the C headers failed, see kpp\full-errors.txt
rem The tests' count, a gate as the headline is (0): the same errors in the whole conversion
rem (%WHOLE%) with UnitTest++ and the tests, which the headline leaves out. Each is a function that
rem tests.cmd's --stub-missing build makes a stub of, so a test would not run what C++ runs.
echo == tests codegen errors: %WHOLE% with UnitTest++ and tests (all errors in kpp\tests-errors.txt)
set L=%WHOLE%\libraries
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --emit-llvm kpp\tests-full.ll %L%\CommonUtil %L%\Agui %L%\cxxopts %L%\StackWalker %L%\Lua %L%\crc32fast %L%\UnitTest++ %WHOLE%\src %WHOLE%\tests 2> kpp\tests-errors.txt
set TESTSRC=%errorlevel%
%SystemRoot%\System32\find.exe /c /v "" kpp\tests-errors.txt
if not "%TESTSRC%"=="0" if not "%TESTSRC%"=="1" echo NOT A VALID COUNT: kppc crashed (exit %TESTSRC%)
echo == smoke test: tests\smoke.kpp against the converted CommonUtil (SystemClipboard stubbed: the real one needs SDL)
setlocal EnableDelayedExpansion
set FILES=
for %%f in (kpp\CommonUtil\*.kpp) do if not %%~nf==SystemClipboard set FILES=!FILES! %%f
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\smoke.exe !FILES! tests\stubs\SystemClipboard.kpp tests\smoke.kpp --lib C:\Factorio\vs2022\Releasex64\trio.lib || exit /b 1
kpp\smoke.exe > kpp\smoke.out.txt
fc /w kpp\smoke.out.txt tests\smoke.expected > nul && echo smoke test: output as expected || (echo smoke test: OUTPUT DIFFERS & fc /w kpp\smoke.out.txt tests\smoke.expected & exit /b 1)
echo == Agui smoke test: tests\agui_smoke.kpp against the converted Agui (Void backend)
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\agui_smoke.exe !FILES! tests\stubs\SystemClipboard.kpp tests\stubs\AguiGameWidgets.kpp kpp\Agui tests\agui_smoke.kpp --lib C:\Factorio\vs2022\Releasex64\trio.lib || exit /b 1
kpp\agui_smoke.exe > kpp\agui_smoke.out.txt
fc /w kpp\agui_smoke.out.txt tests\agui_smoke.expected > nul && echo agui smoke test: output as expected || (echo agui smoke test: OUTPUT DIFFERS & fc /w kpp\agui_smoke.out.txt tests\agui_smoke.expected & exit /b 1)
echo == Util smoke test: tests\util_smoke.kpp against the converted Util subset (containers, sorting, the math expression evaluator; Logging, Util helpers, Math, FloatCast, GameRuntimeError and prefetch stubbed)
set UFILES=
for %%f in (kpp\Util\Container\*.kpp) do if not %%~nf==ExtendedBitBuffer set UFILES=!UFILES! %%f
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\util_smoke.exe !FILES! !UFILES! tests\stubs\SystemClipboard.kpp tests\stubs\Log.kpp tests\stubs\UtilHelpers.kpp tests\stubs\MathStubs.kpp tests\stubs\FloatCast.kpp tests\stubs\UtilTemplateNames.kpp kpp\src\Util\ReleaseAssert.kpp kpp\Util\Sort kpp\Util\MathExpressions kpp\Util\BitBuffer.kpp kpp\Util\ToStr.kpp kpp\Util\LogLevel.kpp kpp\src\Typedefs.kpp kpp\src\TypeTraitsExtensions.kpp kpp\src\Graphics\BitmapFormat.kpp kpp\src\Noise\NoiseRegisterIndex.kpp tests\util_smoke.kpp --lib C:\Factorio\vs2022\Releasex64\trio.lib || exit /b 1
kpp\util_smoke.exe > kpp\util_smoke.out.txt
fc /w kpp\util_smoke.out.txt tests\util_smoke.expected > nul && echo util smoke test: output as expected || (echo util smoke test: OUTPUT DIFFERS & fc /w kpp\util_smoke.out.txt tests\util_smoke.expected & exit /b 1)
echo == imgui smoke test: tests\imgui_smoke.kpp through kpp\src\Debug\ImGui.kpp and native\imgui_c.cpp with the game's imgui.lib (the same frames as tests\imgui_smoke.cpp, whose output is the expected one)
call native\build.cmd >nul || exit /b 1
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\imgui_smoke.exe kpp\src\Debug\ImGui.kpp tests\imgui_smoke.kpp --lib native\build\factorio-native.lib --lib C:\Factorio\vs2022\Releasex64\imgui.lib --lib -luser32 --lib -limm32 || exit /b 1
kpp\imgui_smoke.exe > kpp\imgui_smoke.out.txt
fc /w kpp\imgui_smoke.out.txt tests\imgui_smoke.expected > nul && echo imgui smoke test: output as expected || (echo imgui smoke test: OUTPUT DIFFERS & fc /w kpp\imgui_smoke.out.txt tests\imgui_smoke.expected & exit /b 1)
echo == glm smoke test: tests\glm_smoke.kpp through the glm port in kpp\src\API\glm.kpp (bit for bit what tests\glm_smoke.cpp computes with glm itself)
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\glm_smoke.exe kpp\src\API\glm.kpp tests\glm_smoke.kpp || exit /b 1
kpp\glm_smoke.exe > kpp\glm_smoke.out.txt
fc /w kpp\glm_smoke.out.txt tests\glm_smoke.expected > nul && echo glm smoke test: output as expected || (echo glm smoke test: OUTPUT DIFFERS & fc /w kpp\glm_smoke.out.txt tests\glm_smoke.expected & exit /b 1)
echo == Lua compatibility check: tests\lua_compat.lua run by the converted Lua (tests\lua_compat.kpp with kpp\Lua) prints what the game's lua.lib prints (tests\lua_compat.expected, from tests\lua_compat.cpp)
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\lua_compat.exe tests\lua_compat.kpp kpp\Lua --lib C:\Factorio\vs2022\Releasex64\trio.lib || exit /b 1
kpp\lua_compat.exe tests\lua_compat.lua > kpp\lua_compat.out.txt
fc /w kpp\lua_compat.out.txt tests\lua_compat.expected > nul && echo Lua compatibility check: output as expected || (echo Lua compatibility check: OUTPUT DIFFERS & fc /w kpp\lua_compat.out.txt tests\lua_compat.expected & exit /b 1)
echo == GDI+ smoke test: tests\gdiplus_smoke.kpp through kpp\src\Graphics\GdiplusClasses.kpp and gdiplus.dll (the layouts and results of tests\gdiplus_smoke.cpp, the same against the SDK's gdiplus.h)
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\gdiplus_smoke.exe kpp\src\Graphics\GdiplusClasses.kpp kpp\src\Graphics\DX11\ComInterfaces.kpp tests\gdiplus_smoke.kpp --lib -lgdiplus --lib -lshlwapi || exit /b 1
kpp\gdiplus_smoke.exe > kpp\gdiplus_smoke.out.txt
fc /w kpp\gdiplus_smoke.out.txt tests\gdiplus_smoke.expected > nul && echo GDI+ smoke test: output as expected || (echo GDI+ smoke test: OUTPUT DIFFERS & fc /w kpp\gdiplus_smoke.out.txt tests\gdiplus_smoke.expected & exit /b 1)
echo == regex smoke test: tests\regex_smoke.kpp through kpp\src\Util\RegexUtil.kpp and native\re2_c.cpp with the game's re2.lib (RE2's own results: skipped groups, typed targets, Consume, Replace)
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\regex_smoke.exe kpp\src\Util\RegexUtil.kpp tests\regex_smoke.kpp --lib native\build\factorio-native.lib --lib C:\Factorio\vs2022\Releasex64\re2.lib || exit /b 1
kpp\regex_smoke.exe > kpp\regex_smoke.out.txt
fc /w kpp\regex_smoke.out.txt tests\regex_smoke.expected > nul && echo regex smoke test: output as expected || (echo regex smoke test: OUTPUT DIFFERS & fc /w kpp\regex_smoke.out.txt tests\regex_smoke.expected & exit /b 1)
echo == size widths: tests\convert_sizes\sizes.cpp, converted and run, writes the bytes C++ writes for each std container size a serialiser takes (tests\convert_sizes.expected, from the C++ build of the same file)
if exist kpp\convert_sizes rd /s /q kpp\convert_sizes
%KPPC% --no-std --quiet --convert kpp\convert_sizes --ext .cpp tests\convert_sizes || exit /b 1
%KPPC% --std ..\..\stdlib --quiet --exe kpp\convert_sizes.exe kpp\convert_sizes || exit /b 1
kpp\convert_sizes.exe > kpp\convert_sizes.out.txt
fc /w kpp\convert_sizes.out.txt tests\convert_sizes.expected > nul && echo size widths: output as expected || (echo size widths: OUTPUT DIFFERS & fc /w kpp\convert_sizes.out.txt tests\convert_sizes.expected & exit /b 1)
echo == variants: tests\convert_variant\variant.cpp, std::variant as the game uses it, converted to a typed union and run, prints what its C++ build prints (tests\convert_variant.expected)
if exist kpp\convert_variant rd /s /q kpp\convert_variant
%KPPC% --no-std --quiet --convert kpp\convert_variant --ext .cpp tests\convert_variant || exit /b 1
%KPPC% --std ..\..\stdlib --quiet --exe kpp\convert_variant.exe kpp\convert_variant || exit /b 1
kpp\convert_variant.exe > kpp\convert_variant.out.txt
fc /w kpp\convert_variant.out.txt tests\convert_variant.expected > nul && echo variants: output as expected || (echo variants: OUTPUT DIFFERS & fc /w kpp\convert_variant.out.txt tests\convert_variant.expected & exit /b 1)
echo == printf formats: tests\convert_formats\formats.cpp, Factorio's own 64-bit arguments read as 32 bits and extra arguments, converted and run, prints what its C++ build prints (tests\convert_formats.expected) and passes the format check
if exist kpp\convert_formats rd /s /q kpp\convert_formats
%KPPC% --no-std --quiet --convert kpp\convert_formats --ext .cpp tests\convert_formats || exit /b 1
%KPPC% --std ..\..\stdlib --quiet --exe kpp\convert_formats.exe kpp\convert_formats || exit /b 1
kpp\convert_formats.exe > kpp\convert_formats.out.txt
fc /w kpp\convert_formats.out.txt tests\convert_formats.expected > nul && echo printf formats: output as expected || (echo printf formats: OUTPUT DIFFERS & fc /w kpp\convert_formats.out.txt tests\convert_formats.expected & exit /b 1)
echo == C rules: tests\convert_c\rules.c, where C and C++ differ (void* conversions, designated initializers, compound literals, C++ keywords as names...), converted as C and run, prints what its C build prints (tests\convert_c.expected)
rem Pending until the converter's C mode exists (docs/path-to-factorio.md, "Converting the C libraries"):
rem it is reported, not failed, and a pass while pending fails the run, as spec.cmd's pending snippets do.
set CRULES=pending
if exist kpp\convert_c rd /s /q kpp\convert_c
if exist kpp\convert_c.exe del kpp\convert_c.exe
%KPPC% --no-std --quiet --convert kpp\convert_c --ext .c tests\convert_c > kpp\convert_c.log 2>&1
%KPPC% --std ..\..\stdlib --quiet --exe kpp\convert_c.exe kpp\convert_c >> kpp\convert_c.log 2>&1
set CRULESOK=0
if exist kpp\convert_c.exe (kpp\convert_c.exe > kpp\convert_c.out.txt 2>&1 && fc /w kpp\convert_c.out.txt tests\convert_c.expected > nul && set CRULESOK=1)
if %CRULES%==pending (if %CRULESOK%==1 (echo C rules: FIXED while pending: remove the pending mark& exit /b 1) else (echo C rules: pending, kpp\convert_c.log)) else (if %CRULESOK%==1 (echo C rules: output as expected) else (echo C rules: OUTPUT DIFFERS, kpp\convert_c.log& exit /b 1))
