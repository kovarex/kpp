@echo off
rem Converts the whole Factorio source (CommonUtil, Agui, src, tests) to Kpp into the factorio-in-kpp
rem checkout (C:\factorio-in-kpp\kpp, or the directory given as the first argument; commit there by
rem hand). Then converts the CommonUtil and Agui libraries and the game's Util module again on their
rem own (into kpp\, with the hand-written files kept there), checks that kppc compiles them, and runs
rem the smoke tests against that result: the stubs in tests\stubs\ stand in for the modules a whole
rem conversion pulls in (FloatCast.cpp merged into FloatCast.kpp needs global and Logging, for example).
rem Hand-written parts are in overrides\.
setlocal
cd /d "%~dp0"
set KPPC=..\..\compiler\build\kppc.exe
set SRC=C:\Factorio\src
set WHOLE=C:\factorio-in-kpp\kpp
if not "%~1"=="" set WHOLE=%~1
%KPPC% --no-std --quiet --convert %WHOLE% --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui %SRC% C:\Factorio\tests || exit /b 1
type %WHOLE%\CONVERSION.md | findstr /B "Files"
set CINC=--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include
%KPPC% --no-std --quiet --convert kpp --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui %SRC%\Util %SRC%\Typedefs.hpp %SRC%\Graphics\BitmapFormat.hpp %SRC%\Noise\NoiseRegisterIndex.hpp %SRC%\FloatCast.hpp || exit /b 1
type kpp\CONVERSION.md | findstr /B "Files -"
rem Copy stubs for types that are outside the converted subset but needed for codegen.
if exist stubs\kpp xcopy /s /y /q stubs\kpp kpp\ >nul
echo == kppc on the converted sources
%KPPC% --std ..\..\stdlib %CINC% --errors 10 --unresolved 20 --emit-llvm kpp\out.ll kpp
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
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe kpp\util_smoke.exe !FILES! !UFILES! tests\stubs\SystemClipboard.kpp tests\stubs\Log.kpp tests\stubs\UtilHelpers.kpp tests\stubs\MathStubs.kpp tests\stubs\FloatCast.kpp kpp\Util\Sort kpp\Util\MathExpressions kpp\Util\BitBuffer.kpp kpp\Util\ToStr.kpp kpp\Util\LogLevel.kpp kpp\Typedefs.kpp kpp\BitmapFormat.kpp kpp\NoiseRegisterIndex.kpp kpp\FloatCast.kpp tests\util_smoke.kpp || exit /b 1
kpp\util_smoke.exe > kpp\util_smoke.out.txt
fc /w kpp\util_smoke.out.txt tests\util_smoke.expected > nul && echo util smoke test: output as expected || (echo util smoke test: OUTPUT DIFFERS & fc /w kpp\util_smoke.out.txt tests\util_smoke.expected & exit /b 1)
