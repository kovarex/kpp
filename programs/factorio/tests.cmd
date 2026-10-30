@echo off
rem The converted game's unit tests (C:\Factorio\tests, written in UnitTest++), built and run as
rem run.cmd builds and runs the game. They come from a conversion that has them: convert.cmd's whole
rem one (C:\factorio-in-kpp\kpp, or the directory given as the first argument), with UnitTest++. The
rem build links link-settings.cmd's libraries with --keep-going and --stub-missing (a function the
rem code generator left out aborts when a test reaches it) and the crash report, into
rem kpp\tests-run\factorio-test.exe. The run has a write directory of its own, kpp\tests-run\run
rem (config, log, script output; never %APPDATA%, never C:\Factorio), and reads the game's data and
rem the tests' saves, images and mods from C:\Factorio, as the C++ build's factorio-test.exe does
rem (the tests open `read-data/../tests/...`): read only. Further arguments go to the tests (`--suite
rem Lua`, `--test SomeTest`, `--exit-on-failure`). The runner's summary line is printed next to the C++
rem build's: 9006 tests passed (2026-10-29). `tests.cmd --baseline` runs the C++ build's own
rem C:\Factorio\bin\Releasex64vs2022\factorio-test.exe the same way (its own write directory
rem kpp\tests-baseline\run) to measure that again. A stub or a failed assertion ends the run where it
rem is reached: suites.py runs the built binary one suite per process instead, and crashes.py tallies
rem what stopped each suite. `tests.cmd --asan` builds with AddressSanitizer (kppc's --clang-flags
rem -fsanitize=address, the ASan runtime linked) into kpp\tests-asan\factorio-test.exe and runs it
rem with the runtime's directory (VS 2022's clang 19) on PATH; to run that binary with suites.py, put
rem the directory on PATH too. The game's own signal handler reports a segfault before ASan can, so a
rem read through a wild pointer still shows only as "Received SIGSEGV"; a use after free or an
rem overflow gets ASan's report with the allocation and free stacks.
setlocal
cd /d "%~dp0"
set CONVERSION=C:\factorio-in-kpp\kpp
set BASELINE=0
set ASAN=0
set ASANRUNTIME=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\lib\clang\19\lib\windows
set ARGS=
:arguments
if "%~1"=="" goto argumentsDone
if "%~1"=="--baseline" (set BASELINE=1) else if "%~1"=="--asan" (set ASAN=1) else if exist "%~1\tests" (set CONVERSION=%~1) else (set ARGS=%ARGS% %1)
shift
goto arguments
:argumentsDone
set OUT=%~dp0kpp\tests-run
if %BASELINE%==1 set OUT=%~dp0kpp\tests-baseline
set SANITIZE=
if %ASAN%==1 set OUT=%~dp0kpp\tests-asan
if %ASAN%==1 set SANITIZE=--clang-flags -fsanitize=address --lib -fsanitize=address
if %ASAN%==1 set "PATH=%ASANRUNTIME%;%PATH%"
if not exist %OUT%\run mkdir %OUT%\run
rem The config the runner reads (--config): the game's data from C:\Factorio, the writes in run\.
(
  echo [path]
  echo read-data=C:/Factorio/data
  echo write-data=%OUT:\=/%/run
) > %OUT%\config.ini
set EXE=C:\Factorio\bin\Releasex64vs2022\factorio-test.exe
if %BASELINE%==1 goto run
call native\build.cmd >nul || exit /b 1
call "%~dp0link-settings.cmd"
set LIBS=%LIBS% --lib -Wl,/DEBUG --lib -Wl,/MAP --lib -ldbghelp --lib -Wl,/INCLUDE:kpp_crash_report_install
if exist %OUT%\factorio-test.exe del %OUT%\factorio-test.exe
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going --stub-missing %LIBS% %SANITIZE% --exe %OUT%\factorio-test.exe %CONVERSION%\CommonUtil %CONVERSION%\Agui %CONVERSION%\cxxopts %CONVERSION%\StackWalker %CONVERSION%\Lua %CONVERSION%\UnitTest++ %CONVERSION%\src %CONVERSION%\tests > %OUT%\build.txt 2> %OUT%\build-errors.txt
echo == %OUT%\factorio-test.exe, built by kppc (exit %errorlevel%) from %CONVERSION% with --stub-missing:
%SystemRoot%\System32\findstr.exe /b /c:"stubs:" %OUT%\build.txt
%SystemRoot%\System32\find.exe /c /v "" %OUT%\build-errors.txt
if not exist %OUT%\factorio-test.exe (
  echo the link failed: %OUT%\factorio-test.exe.link.txt
  exit /b 1
)
set EXE=%OUT%\factorio-test.exe
:run
rem Run from the write directory, so that nothing the runner writes next to its working directory
rem lands anywhere else.
pushd %OUT%\run
echo == running %EXE% --config %OUT%\config.ini --write-data %OUT%\run --no-colors%ARGS%
%EXE% --config %OUT%\config.ini --write-data %OUT%\run --no-colors%ARGS% > %OUT%\tests.txt 2>&1
set RESULT=%errorlevel%
popd
echo == factorio-test exit %RESULT%; its output: %OUT%\tests.txt
%SystemRoot%\System32\findstr.exe /c:"Success:" /c:"FAILURE:" /c:"Test time:" %OUT%\tests.txt
echo == the C++ build: Success: 9006 tests passed (2026-10-29; tests.cmd --baseline measures it again)
exit /b %RESULT%
