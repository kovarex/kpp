@echo off
rem Links the converted game (kpp\ from convert.cmd) into kpp\factorio.exe and counts what does not
rem link. kppc builds with --keep-going (what codegen emitted links although it reported errors),
rem against the game's own Release builds of its libraries (C:\Factorio\vs2022\Releasex64), the shims
rem of native\ (built first: RE2's C interface, the header-only C libraries the game implements in
rem its .cpp files), and the Windows libraries of the game's link line (fastbuild\fbuild.bff,
rem .extraExecLibs) plus what the conversion needs on top: dxguid, uuid and windowscodecs for the
rem COM interfaces' IIDs, which C++ compiled in through __uuidof, and version for SDL's IME code. The
rem linker's output (`unresolved external symbol`) is in kpp\factorio.exe.link.txt, the code
rem generator's errors in kpp\link-errors.txt.
setlocal
cd /d "%~dp0"
call native\build.cmd >nul || exit /b 1
set KPPC=..\..\compiler\build\kppc.exe
set CINC=--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\SDL_Mixer\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include --cinclude C:\Factorio\libraries\glad\include --cinclude native\include
set L=C:\Factorio\vs2022\Releasex64
set LIBS=--lib native\build\factorio-native.lib
for %%l in (lua sdl sdl-mixer curl zlib zstd minizip ogg vorbis opus opusfile re2 crc32fast stb fontstash glad imgui trio open-vcdiff stack-walker) do call set LIBS=%%LIBS%% --lib %L%\%%l.lib
for %%l in (dnsapi wldap32 gdi32 comdlg32 ole32 winmm psapi opengl32 gdiplus shell32 shlwapi user32 imm32 oleaut32 setupapi dxguid uuid windowscodecs version) do call set LIBS=%%LIBS%% --lib -l%%l
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going %LIBS% --exe kpp\factorio.exe kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\Lua kpp\src > kpp\link.txt 2> kpp\link-errors.txt
echo kppc exit %errorlevel%
echo unresolved externals (lines, then the linker's count):
%SystemRoot%\System32\find.exe /c "unresolved external symbol" kpp\factorio.exe.link.txt
%SystemRoot%\System32\findstr.exe /c:"unresolved externals" kpp\factorio.exe.link.txt
