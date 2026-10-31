@echo off
rem Writes Visual Studio and VS Code project files for the converted game (tools/vs/README.md):
rem run.cmd's build (programs\factorio\run.cmd says what each option is for), with native\build.cmd
rem before it and copy-to-game.cmd after it, and the debugger starting the game in its directory with
rem --no-crash-handler. Into programs\factorio\build\visual-studio\factorio.sln and programs\factorio\.vscode.
rem The converted tree is C:\factorio-in-kpp\kpp unless FIK names another, the game's directory
rem C:\factorio-in-kpp unless GAME does; generate again after changing them.
setlocal
if not defined FIK set FIK=C:\factorio-in-kpp\kpp
if not defined GAME set GAME=C:\factorio-in-kpp
set HERE=%~dp0..\..\..\programs\factorio\
cd /d "%HERE%"
call "%HERE%link-settings.cmd"
set LIBS=%LIBS% --lib -Wl,/DEBUG --lib -ldbghelp --lib -Wl,/INCLUDE:kpp_crash_report_install
%KPPC% --generate-visual-studio-files --generate-vs-code-files --before-build "call native\build.cmd >nul" --after-build "call \"%~dp0copy-to-game.cmd\" {exe} \"%GAME%\"" --debug-command %GAME%\bin\x64\factorio.exe --debug-arguments --no-crash-handler --debug-directory %GAME%\bin\x64 --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going --stub-missing %LIBS% --exe build\factorio.exe %FIK%\CommonUtil %FIK%\Agui %FIK%\cxxopts %FIK%\StackWalker %FIK%\Lua %FIK%\src
