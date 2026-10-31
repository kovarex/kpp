@echo off
rem After a Visual Studio or VS Code build of the converted game (generate.cmd): factorio.exe (%1)
rem and its PDB into the game directory %2, where the debugger starts it. A game directory without
rem its config yet gets it from run.cmd.
setlocal
set GAME=%~2
if not exist "%~1" exit /b 1
if not exist "%GAME%\bin\x64" mkdir "%GAME%\bin\x64"
copy /y "%~1" "%GAME%\bin\x64\factorio.exe" >nul || exit /b 1
if exist "%~dpn1.pdb" copy /y "%~dpn1.pdb" "%GAME%\bin\x64\factorio.pdb" >nul
if not exist "%GAME%\run\config\config.ini" call "%~dp0..\..\..\programs\factorio\run.cmd" --no-build --version
exit /b 0
