@echo off
rem Converts the whole Factorio source (CommonUtil, Agui, src, tests) to Kpp into the factorio-in-kpp
rem checkout (C:\factorio-in-kpp\kpp, or the directory given as the first argument; commit there by
rem hand). Then converts the CommonUtil and Agui libraries and the game's Util module again on their
rem own (into kpp\, with the hand-written files kept there), checks that kppc compiles them, and runs
rem the smoke tests against that result: the stubs in tests\stubs\ stand in for the modules a whole
rem conversion pulls in (FloatCast.cpp merged into FloatCast.kpp needs global and Logging, for example).
rem The last one runs imgui frames through the generated imgui interface (native\generate_imgui.py).
rem Hand-written parts are in overrides\.
setlocal
cd /d "%~dp0"
set KPPC=..\..\compiler\build\kppc.exe
set SRC=C:\Factorio\src
set WHOLE=C:\factorio-in-kpp\kpp
if not "%~1"=="" set WHOLE=%~1
rem The game's C++ overloads of Lua's API (lua_pushstring of a std::string, ...): converted with the
rem game, into kpp\Lua\, while Lua itself is imported as C.
set LUACPP=C:\Factorio\libraries\Lua\LuaCPPUtilities.hpp
%KPPC% --no-std --quiet --convert %WHOLE% --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui C:\Factorio\libraries\cxxopts %LUACPP% %SRC% C:\Factorio\tests || exit /b 1
type %WHOLE%\CONVERSION.md | findstr /B "Files"
set CINC=--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\SDL_Mixer\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include --cinclude C:\Factorio\libraries\glad\include --cinclude native\include
%KPPC% --no-std --quiet --convert kpp --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui C:\Factorio\libraries\cxxopts %LUACPP% %SRC% || exit /b 1
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
rem stderr lines of the full build of exactly CommonUtil, Agui, cxxopts, Lua and src, not the kpp\Util copy.
echo == full-src codegen errors: kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\Lua kpp\src (all errors in kpp\full-errors.txt)
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --emit-llvm kpp\full.ll kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\Lua kpp\src 2> kpp\full-errors.txt
set FULLRC=%errorlevel%
rem System32's find: a Unix find earlier on the PATH (Git's) would walk the disk instead.
%SystemRoot%\System32\find.exe /c /v "" kpp\full-errors.txt
if not "%FULLRC%"=="0" if not "%FULLRC%"=="1" echo NOT A VALID COUNT: kppc crashed (exit %FULLRC%)
findstr /c:"importing C headers failed" kpp\full-errors.txt >nul && echo NOT A VALID COUNT: importing the C headers failed, see kpp\full-errors.txt
echo == smoke test: tests\smoke.kpp against the converted CommonUtil (SystemClipboard stubbed: the real one needs SDL)
setlocal EnableDelayedExpansion
set FILES=
for %%f in (kpp\CommonUtil\*.kpp) do if not %%~nf==SystemClipboard set FILES=!FILES! %%f
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\smoke.exe !FILES! tests\stubs\SystemClipboard.kpp tests\smoke.kpp || exit /b 1
kpp\smoke.exe > kpp\smoke.out.txt
fc /w kpp\smoke.out.txt tests\smoke.expected > nul && echo smoke test: output as expected || (echo smoke test: OUTPUT DIFFERS & fc /w kpp\smoke.out.txt tests\smoke.expected & exit /b 1)
echo == Agui smoke test: tests\agui_smoke.kpp against the converted Agui (Void backend)
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\agui_smoke.exe !FILES! tests\stubs\SystemClipboard.kpp kpp\Agui tests\agui_smoke.kpp || exit /b 1
kpp\agui_smoke.exe > kpp\agui_smoke.out.txt
fc /w kpp\agui_smoke.out.txt tests\agui_smoke.expected > nul && echo agui smoke test: output as expected || (echo agui smoke test: OUTPUT DIFFERS & fc /w kpp\agui_smoke.out.txt tests\agui_smoke.expected & exit /b 1)
echo == Util smoke test: tests\util_smoke.kpp against the converted Util subset (containers, sorting, the math expression evaluator; Logging, Util helpers, Math and FloatCast stubbed)
set UFILES=
for %%f in (kpp\Util\Container\*.kpp) do if not %%~nf==ExtendedBitBuffer set UFILES=!UFILES! %%f
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\util_smoke.exe !FILES! !UFILES! tests\stubs\SystemClipboard.kpp tests\stubs\Log.kpp tests\stubs\UtilHelpers.kpp tests\stubs\MathStubs.kpp tests\stubs\FloatCast.kpp kpp\Util\Sort kpp\Util\MathExpressions kpp\Util\BitBuffer.kpp kpp\Util\ToStr.kpp kpp\Util\LogLevel.kpp kpp\src\Typedefs.kpp kpp\src\TypeTraitsExtensions.kpp kpp\src\Graphics\BitmapFormat.kpp kpp\src\Noise\NoiseRegisterIndex.kpp tests\util_smoke.kpp || exit /b 1
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
