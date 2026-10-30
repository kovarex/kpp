@echo off
rem Builds the C and C++ shims the converted game links with into native\build\factorio-native.lib:
rem re2_c.cpp, RE2's thin C interface (overrides/src/Util/RegexUtil.kpp is its Kpp side),
rem imgui_c.cpp, dear imgui's (generate_imgui.py writes it and its Kpp side,
rem overrides/added/src/Debug/ImGui.kpp), header_only.c and jc_voronoi.cpp, the header-only C
rem libraries the game implements inside its own .cpp files, logitech_led_c.cpp,
rem the Logitech LED SDK's C++ functions as C ones, crc32fast_c.cpp, crc32fast's, open_vcdiff_c.cpp,
rem open-vcdiff's decoding, inline_c.c,
rem the inline functions of C headers the game calls (gai_strerrorW, IsWindows8OrGreater) as
rem functions, and crash_report.c, which only run.cmd links in (an unhandled exception's stack, named
rem from the PDB). factorio.rc becomes build\factorio.res, the game's manifest and icons. With clang-cl
rem as the
rem game's Release builds are made: /MT, /O2, against the game's library headers, so that the
rem objects link with C:\Factorio\vs2022\Releasex64\re2.lib and imgui.lib. link.cmd
rem passes the .lib to kppc with --lib, next to those.
rem Nothing is built when the inputs are as at the last build (stamp.ps1 says what they are): a
rem rebuild took 3 s and changed the library's bytes, which made the game's next link a full one.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
powershell -NoProfile -ExecutionPolicy Bypass -File stamp.ps1 || exit /b 1
if exist build\factorio-native.lib if exist build\factorio.res if exist build\inputs.stamp fc build\inputs.new build\inputs.stamp >nul && (
  echo native\build\factorio-native.lib: up to date
  exit /b 0
)
if exist build\inputs.stamp del build\inputs.stamp
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries\re2 re2_c.cpp /Fobuild\re2_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries imgui_c.cpp /Fobuild\imgui_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /DNDEBUG /I C:\Factorio\libraries header_only.c /Fobuild\header_only.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries jc_voronoi.cpp /Fobuild\jc_voronoi.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\src logitech_led_c.cpp /Fobuild\logitech_led_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries crc32fast_c.cpp /Fobuild\crc32fast_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries open_vcdiff_c.cpp /Fobuild\open_vcdiff_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /DNDEBUG inline_c.c /Fobuild\inline_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /DNDEBUG crash_report.c /Fobuild\crash_report.obj || exit /b 1
rc /nologo /fo build\factorio.res factorio.rc || exit /b 1
llvm-lib /nologo /out:build\factorio-native.lib build\re2_c.obj build\imgui_c.obj build\header_only.obj build\jc_voronoi.obj build\logitech_led_c.obj build\crc32fast_c.obj build\open_vcdiff_c.obj build\inline_c.obj build\crash_report.obj || exit /b 1
move /y build\inputs.new build\inputs.stamp >nul || exit /b 1
echo native\build\factorio-native.lib
