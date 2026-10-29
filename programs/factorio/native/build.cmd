@echo off
rem Builds the C and C++ shims the converted game links with into native\build\factorio-native.lib:
rem re2_c.cpp, RE2's thin C interface (overrides/src/Util/RegexUtil.kpp is its Kpp side), and
rem header_only.c and jc_voronoi.cpp, the header-only C libraries the game implements inside its
rem own .cpp files. With clang-cl as the game's Release builds are made: /MT, /O2, against the game's
rem library headers, so that the objects link with C:\Factorio\vs2022\Releasex64\re2.lib. link.cmd
rem passes the .lib to kppc with --lib, next to re2.lib.
setlocal
cd /d "%~dp0"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not exist build mkdir build
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries\re2 re2_c.cpp /Fobuild\re2_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /DNDEBUG /I C:\Factorio\libraries header_only.c /Fobuild\header_only.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries jc_voronoi.cpp /Fobuild\jc_voronoi.obj || exit /b 1
llvm-lib /nologo /out:build\factorio-native.lib build\re2_c.obj build\header_only.obj build\jc_voronoi.obj || exit /b 1
echo native\build\factorio-native.lib
