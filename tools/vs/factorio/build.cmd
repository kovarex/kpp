@echo off
rem The converted game's build for Visual Studio (factorio.vcxproj; tools/vs/README.md): run.cmd's
rem build (programs\factorio\run.cmd says what each option is for) through the compile server, with
rem the errors on stderr for the Error List, then factorio.exe and its PDB copied into the game's
rem directory, where F5 starts it. %1 is the executable kppc writes, %2 more kppc options (the
rem configuration's). The converted tree is C:\factorio-in-kpp\kpp unless FIK names another, the
rem game's directory C:\factorio-in-kpp unless GAME does (as for run.cmd).
setlocal
set EXE=%~1
set OUT=%~dp1
if not defined FIK set FIK=C:\factorio-in-kpp\kpp
if not defined GAME set GAME=C:\factorio-in-kpp
set HERE=%~dp0..\..\..\programs\factorio\
rem (native\build.cmd leaves the directory changed.)
call "%HERE%native\build.cmd" >nul 2>&1 || (echo error: programs\factorio\native\build.cmd failed & exit /b 1)
cd /d "%HERE%"
call "%HERE%link-settings.cmd"
set LIBS=%LIBS% --lib -Wl,/DEBUG --lib -ldbghelp --lib -Wl,/INCLUDE:kpp_crash_report_install
if not exist "%OUT%" mkdir "%OUT%"
%KPPC% --server --msvc-diagnostics --quiet %~2 --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going --stub-missing --incremental-link %LIBS% --exe "%EXE%" %FIK%\CommonUtil %FIK%\Agui %FIK%\cxxopts %FIK%\StackWalker %FIK%\Lua %FIK%\src
set CODE=%errorlevel%
if not exist "%EXE%" exit /b 1
if not exist "%GAME%\bin\x64" mkdir "%GAME%\bin\x64"
copy /y "%EXE%" "%GAME%\bin\x64\factorio.exe" >nul || exit /b 1
if exist "%OUT%%~n1.pdb" copy /y "%OUT%%~n1.pdb" "%GAME%\bin\x64\factorio.pdb" >nul
rem A game directory without its config yet gets it from run.cmd (which then prints the version).
if not exist "%GAME%\run\config\config.ini" call "%HERE%run.cmd" --no-build --version
exit /b %CODE%
