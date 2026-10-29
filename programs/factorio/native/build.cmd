@echo off
rem Builds the C and C++ shims the converted game links with into native\build\factorio-native.lib:
rem re2_c.cpp, RE2's thin C interface (overrides/src/Util/RegexUtil.kpp is its Kpp side),
rem imgui_c.cpp, dear imgui's (generate_imgui.py writes it and its Kpp side,
rem overrides/added/src/Debug/ImGui.kpp), header_only.c and jc_voronoi.cpp, the header-only C
rem libraries the game implements inside its own .cpp files, and lua_internals.c, the C names of the
rem Lua internals that lua.lib has under C++'s (include\kpp_lua_internals.h), and logitech_led_c.cpp,
rem the Logitech LED SDK's C++ functions as C ones, crc32fast_c.cpp, crc32fast's, and inline_c.c,
rem the inline functions of C headers the game calls (gai_strerrorW, IsWindows8OrGreater) as
rem functions. With clang-cl as the
rem game's Release builds are made: /MT, /O2, against the game's library headers, so that the
rem objects link with C:\Factorio\vs2022\Releasex64\re2.lib, imgui.lib and lua.lib. link.cmd
rem passes the .lib to kppc with --lib, next to those.
setlocal
cd /d "%~dp0"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not exist build mkdir build
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries\re2 re2_c.cpp /Fobuild\re2_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries imgui_c.cpp /Fobuild\imgui_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /DNDEBUG /I C:\Factorio\libraries header_only.c /Fobuild\header_only.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /DNDEBUG /I include /I C:\Factorio\libraries\Lua lua_internals.c /Fobuild\lua_internals.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries jc_voronoi.cpp /Fobuild\jc_voronoi.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\src logitech_led_c.cpp /Fobuild\logitech_led_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /EHsc /std:c++17 /DNDEBUG /I C:\Factorio\libraries crc32fast_c.cpp /Fobuild\crc32fast_c.obj || exit /b 1
clang-cl /nologo /c /O2 /MT /DNDEBUG inline_c.c /Fobuild\inline_c.obj || exit /b 1
llvm-lib /nologo /out:build\factorio-native.lib build\re2_c.obj build\imgui_c.obj build\header_only.obj build\lua_internals.obj build\jc_voronoi.obj build\logitech_led_c.obj build\crc32fast_c.obj build\inline_c.obj || exit /b 1
echo native\build\factorio-native.lib
