@echo off
rem The first run of the converted game (docs/path-to-factorio.md, "The first run"). Links it as
rem link.cmd does, but with --stub-missing, a bring-up tool and never a way to ship: every function the
rem code generator left out (or found no definition of) is a stub that prints its name and line and
rem aborts when the game reaches it, so the game links and runs up to the first one. The compile's
rem errors stay in kpp\run\build-errors.txt and its exit code stays 1. factorio.exe goes where the C++
rem build puts its own relative to the data, C:\factorio-in-kpp\bin\x64 (the game's path discovery
rem works unchanged), and runs with a write directory of its own, C:\factorio-in-kpp\run (config,
rem log, saves): never %APPDATA%\Factorio, never C:\Factorio. Arguments go to the game:
rem `run.cmd --version`, `run.cmd --dump-data`. The C++ build's log to compare with is
rem C:\Factorio\factorio-current.log.
setlocal
cd /d "%~dp0"
set GAME=C:\factorio-in-kpp
call native\build.cmd >nul || exit /b 1
set KPPC=..\..\compiler\build\kppc.exe
set CINC=--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\SDL_Mixer\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include --cinclude C:\Factorio\libraries\glad\include --cinclude native\include
set L=C:\Factorio\vs2022\Releasex64
set LIBS=--lib native\build\factorio-native.lib --lib C:\Factorio\libraries\Logitech\x64\LogitechLEDLib.lib
for %%l in (lua sdl sdl-mixer curl zlib zstd minizip ogg vorbis opus opusfile re2 crc32fast stb fontstash glad imgui trio open-vcdiff) do call set LIBS=%%LIBS%% --lib %L%\%%l.lib
for %%l in (dnsapi iphlpapi wldap32 gdi32 comdlg32 ole32 winmm psapi opengl32 gdiplus shell32 shlwapi user32 imm32 oleaut32 setupapi dxguid uuid windowscodecs version) do call set LIBS=%%LIBS%% --lib -l%%l
if not exist kpp\run mkdir kpp\run
if exist kpp\run\factorio.exe del kpp\run\factorio.exe
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going --stub-missing %LIBS% --exe kpp\run\factorio.exe kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\src > kpp\run\build.txt 2> kpp\run\build-errors.txt
echo == %GAME%\bin\x64\factorio.exe, built by kppc (exit %errorlevel%) with --stub-missing:
%SystemRoot%\System32\findstr.exe /b /c:"stubs:" kpp\run\build.txt
%SystemRoot%\System32\find.exe /c /v "" kpp\run\build-errors.txt
if not exist kpp\run\factorio.exe (
  echo the link failed: kpp\run\factorio.exe.link.txt
  exit /b 1
)
if not exist %GAME%\bin\x64 mkdir %GAME%\bin\x64
copy /y kpp\run\factorio.exe %GAME%\bin\x64\factorio.exe >nul || exit /b 1
rem The config file the game looks for next to its root: the config, and from it the write
rem directory, in run\ (use-system-read-write-data-directories=false: never %APPDATA%).
if not exist %GAME%\config-path.cfg (
  echo config-path=__PATH__executable__/../../run/config
  echo use-system-read-write-data-directories=false
) > %GAME%\config-path.cfg
if not exist %GAME%\run\config mkdir %GAME%\run\config
if not exist %GAME%\run\config\config.ini (
  echo [path]
  echo read-data=__PATH__executable__/../../data
  echo write-data=__PATH__executable__/../../run
) > %GAME%\run\config\config.ini
echo == running %GAME%\bin\x64\factorio.exe %*
%GAME%\bin\x64\factorio.exe %*
echo == factorio exit %errorlevel%; the log: %GAME%\run\factorio-current.log
