@echo off
rem What link.cmd and run.cmd build the converted game with, so that the two link the same game:
rem kpp, the C include directories, and the libraries of the link line (link.cmd says what each
rem is for). Called by them from programs\factorio; sets KPP, CINC and LIBS.
rem The Factorio source tree read (src, libraries, tests, data): C:\Factorio unless FACTORIO names another,
rem as a snapshot of an older commit taken with git archive. Its prebuilt libraries and executables
rem (vs2022\, bin\) are always C:\Factorio's.
if not defined FACTORIO set FACTORIO=C:\Factorio
set KPP=..\..\compiler\build\kpp.exe
set CINC=--cinclude %FACTORIO%\libraries\SDL3\include --cinclude %FACTORIO%\libraries\SDL_Mixer\include --cinclude %FACTORIO%\libraries\Lua --cinclude %FACTORIO%\libraries --cinclude %FACTORIO%\libraries\curl\include --cinclude %FACTORIO%\libraries\glad\include --cinclude %FACTORIO%\libraries\trio --cinclude native\include
rem The C libraries converted to kpp (c-libraries.cmd) are compiled with the game (CLIBKPP, kpp\NAME,
rem as convert.cmd writes them), not linked, and their headers imported.
call "%~dp0c-libraries.cmd"
rem (the converted game writes `this` as a reference, design.md 23d)
set CINC=%CINC% %CLIBINC% --this-reference
for %%l in (%CLIBRARIES%) do call set CINC=%%CINC%% --cinclude kpp\%%l
rem (a scoped one, CLIBSCOPED, as the library it is: --library-root, design.md 16)
set CLIBKPP=
for %%l in (%CLIBRARIES%) do if exist "%~dp0c-libraries\%%l\interface.txt" (call set CLIBKPP=%%CLIBKPP%% --library-root %%l kpp\%%l) else call set CLIBKPP=%%CLIBKPP%% kpp\%%l
set L=C:\Factorio\vs2022\Releasex64
set LIBS=--lib native\build\factorio-native.lib --lib %FACTORIO%\libraries\Logitech\x64\LogitechLEDLib.lib
for %%l in (sdl sdl-mixer curl zlib zstd minizip ogg vorbis opus opusfile re2 stb fontstash glad imgui trio open-vcdiff) do call :link %%l
for %%l in (dnsapi iphlpapi wldap32 gdi32 comdlg32 ole32 winmm psapi opengl32 gdiplus shell32 shlwapi user32 imm32 oleaut32 setupapi dxguid uuid windowscodecs wbemuuid version) do call set LIBS=%%LIBS%% --lib -l%%l
rem The game's manifest and icons (native\factorio.rc): without the manifest's supportedOS entries
rem Windows runs the game as Windows 8 (the log said "Windows 8 <running in compatibility mode>"), and
rem it declares the game per-monitor DPI aware.
set LIBS=%LIBS% --lib native\build\factorio.res
goto :eof

rem A C library's .lib on the link line, unless it is converted (c-libraries.cmd).
:link
for %%c in (%CLIBNAMES%) do if /i "%~1"=="%%c" goto :eof
set LIBS=%LIBS% --lib %L%\%~1.lib
goto :eof
