@echo off
rem The first run of the converted game (docs/path-to-factorio.md, "The first run"). Links it as
rem link.cmd does, but with --stub-missing, a bring-up tool and never a way to ship: every function the
rem code generator left out (or found no definition of) is a stub that prints its name and line and
rem aborts when the game reaches it, so the game links and runs up to the first one. The compile's
rem errors stay in kpp\run\build-errors.txt and its exit code stays 1. factorio.exe goes where the C++
rem build puts its own relative to the data, C:\factorio-in-kpp\bin\x64 (the game's path discovery
rem works unchanged), and runs with a write directory of its own, C:\factorio-in-kpp\run (config,
rem log, saves): never %APPDATA%\Factorio, never C:\Factorio. Arguments go to the game:
rem `run.cmd --version`, `run.cmd --dump-data`; `--no-build` first runs the last build again. The
rem C++ build's log to compare with is C:\Factorio\factorio-current.log.
rem
rem A crash prints its stack (native\crash_report.c, named from the PDB this link writes) and ends
rem the game: the game's own crash handler hangs in the converted game, so it runs with
rem --no-crash-handler. KPPC_EXTRA adds kppc options, as for an AddressSanitizer build:
rem `set KPPC_EXTRA=--clang-flags -fsanitize=address --lib -fsanitize=address`.
setlocal
cd /d "%~dp0"
set GAME=C:\factorio-in-kpp
set BUILD=1
set ARGS=
:arguments
if "%~1"=="" goto argumentsDone
if "%~1"=="--no-build" (set BUILD=0) else (set ARGS=%ARGS% %1)
shift
goto arguments
:argumentsDone
if %BUILD%==0 goto run
call native\build.cmd >nul || exit /b 1
call "%~dp0link-settings.cmd"
rem The linker's map and a PDB of the functions' names (the objects carry no debug information), and
rem the crash report, which only this link pulls in.
set LIBS=%LIBS% --lib -Wl,/DEBUG --lib -Wl,/MAP --lib -ldbghelp --lib -Wl,/INCLUDE:kpp_crash_report_install
if not exist kpp\run mkdir kpp\run
if exist kpp\run\factorio.exe del kpp\run\factorio.exe
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going --stub-missing %LIBS% %KPPC_EXTRA% --exe kpp\run\factorio.exe kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\src > kpp\run\build.txt 2> kpp\run\build-errors.txt
echo == %GAME%\bin\x64\factorio.exe, built by kppc (exit %errorlevel%) with --stub-missing:
%SystemRoot%\System32\findstr.exe /b /c:"stubs:" kpp\run\build.txt
%SystemRoot%\System32\find.exe /c /v "" kpp\run\build-errors.txt
if not exist kpp\run\factorio.exe (
  echo the link failed: kpp\run\factorio.exe.link.txt
  exit /b 1
)
if not exist %GAME%\bin\x64 mkdir %GAME%\bin\x64
copy /y kpp\run\factorio.exe %GAME%\bin\x64\factorio.exe >nul || exit /b 1
if exist kpp\run\factorio.pdb copy /y kpp\run\factorio.pdb %GAME%\bin\x64\factorio.pdb >nul
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
:run
echo == running %GAME%\bin\x64\factorio.exe --no-crash-handler%ARGS%
%GAME%\bin\x64\factorio.exe --no-crash-handler%ARGS%
echo == factorio exit %errorlevel%; the log: %GAME%\run\factorio-current.log
