@echo off
rem Links the converted game (kpp\ from convert.cmd) into kpp\factorio.exe and counts what does not
rem link. kppc builds with --keep-going (what codegen emitted links although it reported errors),
rem against the game's own Release builds of its libraries (C:\Factorio\vs2022\Releasex64) and the
rem Logitech LED SDK's LogitechLEDLib.lib, the shims of native\ (built first: RE2's and the Logitech
rem SDK's C interfaces, the header-only C libraries the game implements in its .cpp files), and the
rem Windows libraries of the game's link line (fastbuild\fbuild.bff, .extraExecLibs) plus what the
rem conversion needs on top: dxguid, uuid, windowscodecs and wbemuuid for the COM interfaces' and
rem WMI's ids, which C++ compiled in through __uuidof, version for SDL's IME code and iphlpapi for
rem UDPSocket's GetIpAddrTable. The linker's output (`unresolved external symbol`) is in
rem kpp\factorio.exe.link.txt, the code generator's errors in kpp\link-errors.txt.
setlocal
cd /d "%~dp0"
call native\build.cmd >nul || exit /b 1
call "%~dp0link-settings.cmd"
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going %LIBS% --exe kpp\factorio.exe kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\crc32fast %CLIBKPP% kpp\src > kpp\link.txt 2> kpp\link-errors.txt
echo kppc exit %errorlevel%
echo unresolved externals (lines, then the linker's count):
%SystemRoot%\System32\find.exe /c "unresolved external symbol" kpp\factorio.exe.link.txt
%SystemRoot%\System32\findstr.exe /c:"unresolved externals" kpp\factorio.exe.link.txt
rem Why each one has no definition: left out by a codegen error, never emitted with no error, or a C
rem name (externals.py; docs/path-to-factorio.md, "The link line").
python -X utf8 externals.py kpp\factorio.exe.link.txt kpp\link-errors.txt
