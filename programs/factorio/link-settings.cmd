@echo off
rem What link.cmd and run.cmd build the converted game with, so that the two link the same game:
rem kppc, the C include directories, and the libraries of the link line (link.cmd says what each
rem is for). Called by them from programs\factorio; sets KPPC, CINC and LIBS.
rem The Factorio source tree read (src, libraries, tests, data): C:\Factorio unless FACTORIO names another,
rem as a snapshot of an older commit taken with git archive. Its prebuilt libraries and executables
rem (vs2022\, bin\) are always C:\Factorio's.
if not defined FACTORIO set FACTORIO=C:\Factorio
set KPPC=..\..\compiler\build\kppc.exe
set CINC=--cinclude %FACTORIO%\libraries\SDL3\include --cinclude %FACTORIO%\libraries\SDL_Mixer\include --cinclude %FACTORIO%\libraries\Lua --cinclude %FACTORIO%\libraries --cinclude %FACTORIO%\libraries\curl\include --cinclude %FACTORIO%\libraries\glad\include --cinclude %FACTORIO%\libraries\trio --cinclude native\include
set L=C:\Factorio\vs2022\Releasex64
set LIBS=--lib native\build\factorio-native.lib --lib %FACTORIO%\libraries\Logitech\x64\LogitechLEDLib.lib
for %%l in (sdl sdl-mixer curl zlib zstd minizip ogg vorbis opus opusfile re2 stb fontstash glad imgui trio open-vcdiff) do call set LIBS=%%LIBS%% --lib %L%\%%l.lib
for %%l in (dnsapi iphlpapi wldap32 gdi32 comdlg32 ole32 winmm psapi opengl32 gdiplus shell32 shlwapi user32 imm32 oleaut32 setupapi dxguid uuid windowscodecs wbemuuid version) do call set LIBS=%%LIBS%% --lib -l%%l
rem The game's manifest and icons (native\factorio.rc): without the manifest's supportedOS entries
rem Windows runs the game as Windows 8 (the log said "Windows 8 <running in compatibility mode>"), and
rem it declares the game per-monitor DPI aware.
set LIBS=%LIBS% --lib native\build\factorio.res
