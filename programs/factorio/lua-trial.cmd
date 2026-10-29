@echo off
rem The Lua trial (docs/path-to-factorio.md, "Converting Lua"): converts libraries\Lua on its own, its
rem .c files as the C++ the game compiles them as (lua.c and luac.c left out, as fbuild.bff leaves them
rem out), into the directory given as the first argument (%TEMP%\kpp-lua-trial by default), and
rem builds that alone the way the headline builds the game, with trio's headers for the
rem override_printf.h that Lua's files see first. Prints the error line count, the "Lua trial" of
rem the reports; the errors are in <directory>.err.
rem Then step 3's gate: tests\lua_compat.lua run by the converted Lua (tests\lua_compat.kpp, built
rem with the trial's files) must print what the C++ build prints (tests\lua_compat.expected, from
rem tests\lua_compat.cpp). While the trial has errors, that build fails and the check says so.
setlocal
cd /d "%~dp0"
set KPPC=..\..\compiler\build\kppc.exe
set OUT=%TEMP%\kpp-lua-trial
if not "%~1"=="" set OUT=%~1
if exist "%OUT%" rd /s /q "%OUT%"
%KPPC% --no-std --quiet --convert "%OUT%" --ext .c,.h,.hpp C:\Factorio\libraries\Lua > "%OUT%.convert.log" 2>&1
if not exist "%OUT%\CONVERSION.md" (type "%OUT%.convert.log" & exit /b 1)
set CINC=--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\SDL_Mixer\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include --cinclude C:\Factorio\libraries\glad\include --cinclude C:\Factorio\libraries\trio --cinclude native\include
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --emit-llvm "%OUT%.ll" "%OUT%" > "%OUT%.out" 2> "%OUT%.err"
for /f %%n in ('%SystemRoot%\System32\find.exe /c /v "" ^< "%OUT%.err"') do echo Lua trial: %%n error lines
findstr /C:"parse errors:" "%OUT%.out"
%KPPC% --std ..\..\stdlib %CINC% --quiet --exe "%OUT%.compat.exe" tests\lua_compat.kpp "%OUT%" --lib C:\Factorio\vs2022\Releasex64\trio.lib > "%OUT%.compat.log" 2>&1
if errorlevel 1 (echo Lua compatibility: not built, see %OUT%.compat.log & exit /b 0)
"%OUT%.compat.exe" tests\lua_compat.lua > "%OUT%.compat.txt"
fc /w "%OUT%.compat.txt" tests\lua_compat.expected > nul && echo Lua compatibility: output as expected || (echo Lua compatibility: OUTPUT DIFFERS & fc /w "%OUT%.compat.txt" tests\lua_compat.expected & exit /b 1)
exit /b 0
