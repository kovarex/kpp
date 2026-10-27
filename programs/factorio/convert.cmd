@echo off
rem Converts the whole Factorio source (CommonUtil, Agui, src, tests) to Kov into the factorio-in-kpp
rem checkout (C:\factorio-in-kpp\kov, or the directory given as the first argument; commit there by
rem hand). Then converts the CommonUtil and Agui libraries and the game's Util module again on their
rem own (into kov\, with the hand-written files kept there), checks that kovc compiles them, and runs
rem the smoke tests against that result: the stubs in tests\stubs\ stand in for the modules a whole
rem conversion pulls in (FloatCast.cpp merged into FloatCast.kov needs global and Logging, for example).
rem Hand-written parts are in overrides\.
setlocal
cd /d "%~dp0"
set KOVC=..\..\compiler\build\kovc.exe
set SRC=C:\Factorio\src
set WHOLE=C:\factorio-in-kpp\kov
if not "%~1"=="" set WHOLE=%~1
%KOVC% --no-std --quiet --convert %WHOLE% --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui %SRC% C:\Factorio\tests || exit /b 1
type %WHOLE%\CONVERSION.md | findstr /B "Files"
set CINC=--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include
%KOVC% --no-std --quiet --convert kov --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui %SRC%\Util %SRC%\Typedefs.hpp %SRC%\Graphics\BitmapFormat.hpp %SRC%\Noise\NoiseRegisterIndex.hpp %SRC%\FloatCast.hpp || exit /b 1
type kov\CONVERSION.md | findstr /B "Files -"
echo == kovc on the converted sources
%KOVC% --std ..\..\stdlib %CINC% --errors 10 --unresolved 20 --emit-llvm kov\out.ll kov
echo == smoke test: tests\smoke.kov against the converted CommonUtil (SystemClipboard stubbed: the real one needs SDL)
setlocal EnableDelayedExpansion
set FILES=
for %%f in (kov\CommonUtil\*.kov) do if not %%~nf==SystemClipboard set FILES=!FILES! %%f
%KOVC% --std ..\..\stdlib %CINC% --quiet --exe kov\smoke.exe !FILES! tests\stubs\SystemClipboard.kov tests\smoke.kov || exit /b 1
kov\smoke.exe > kov\smoke.out.txt
fc /w kov\smoke.out.txt tests\smoke.expected > nul && echo smoke test: output as expected || (echo smoke test: OUTPUT DIFFERS & fc /w kov\smoke.out.txt tests\smoke.expected & exit /b 1)
echo == Agui smoke test: tests\agui_smoke.kov against the converted Agui (Void backend)
%KOVC% --std ..\..\stdlib %CINC% --quiet --exe kov\agui_smoke.exe !FILES! tests\stubs\SystemClipboard.kov kov\Agui tests\agui_smoke.kov || exit /b 1
kov\agui_smoke.exe > kov\agui_smoke.out.txt
fc /w kov\agui_smoke.out.txt tests\agui_smoke.expected > nul && echo agui smoke test: output as expected || (echo agui smoke test: OUTPUT DIFFERS & fc /w kov\agui_smoke.out.txt tests\agui_smoke.expected & exit /b 1)
echo == Util smoke test: tests\util_smoke.kov against the converted Util subset (containers, sorting, the math expression evaluator; Logging, Util helpers, Math and FloatCast stubbed)
set UFILES=
for %%f in (kov\Util\Container\*.kov) do if not %%~nf==ExtendedBitBuffer set UFILES=!UFILES! %%f
%KOVC% --std ..\..\stdlib %CINC% --quiet --exe kov\util_smoke.exe !FILES! !UFILES! tests\stubs\SystemClipboard.kov tests\stubs\Log.kov tests\stubs\UtilHelpers.kov tests\stubs\MathStubs.kov tests\stubs\FloatCast.kov kov\Util\Sort kov\Util\MathExpressions kov\Util\BitBuffer.kov kov\Util\ToStr.kov kov\Util\LogLevel.kov kov\Typedefs.kov kov\BitmapFormat.kov kov\NoiseRegisterIndex.kov kov\FloatCast.kov tests\util_smoke.kov || exit /b 1
kov\util_smoke.exe > kov\util_smoke.out.txt
fc /w kov\util_smoke.out.txt tests\util_smoke.expected > nul && echo util smoke test: output as expected || (echo util smoke test: OUTPUT DIFFERS & fc /w kov\util_smoke.out.txt tests\util_smoke.expected & exit /b 1)
