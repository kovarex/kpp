@echo off
rem The first run of the converted game (docs/path-to-factorio.md, "The first run"). Links it as
rem link.cmd does, but with --stub-missing, a bring-up tool and never a way to ship: every function the
rem code generator left out (or found no definition of) is a stub that prints its name and line and
rem aborts when the game reaches it, so the game links and runs up to the first one. The compile's
rem errors stay in kpp\run\build-errors.txt and its exit code stays 1. factorio.exe goes where the C++
rem build puts its own relative to the data, C:\factorio-in-kpp\bin\x64 (the game's path discovery
rem works unchanged), and runs with a write directory of its own, C:\factorio-in-kpp\run (config,
rem log, saves): never %APPDATA%\Factorio, never C:\Factorio. Arguments go to the game:
rem `run.cmd --version`, `run.cmd --dump-data`; `--no-build` first runs the last build again;
rem `--release` builds the release game (kppc --release, kpp\run-release) instead of the debug one. The
rem C++ build's log to compare with is C:\Factorio\factorio-current.log.
rem
rem A crash prints its stack (native\crash_report.c, named from the PDB this link writes) and ends
rem the game: the game's own crash handler hangs in the converted game, so it runs with
rem --no-crash-handler. KPPC_EXTRA adds kppc options, as for an AddressSanitizer build:
rem `set KPPC_EXTRA=--clang-flags -fsanitize=address --lib -fsanitize=address`.
setlocal
rem The Factorio source tree read (src, libraries, tests, data): C:\Factorio unless FACTORIO names another,
rem as a snapshot of an older commit taken with git archive. Its prebuilt libraries and executables
rem (vs2022\, bin\) are always C:\Factorio's.
if not defined FACTORIO set FACTORIO=C:\Factorio
cd /d "%~dp0"
rem The script's own directory: the argument loop's shift shifts %0 too.
set HERE=%~dp0
rem The game's directory (its data\, bin\x64\ and run\): GAME names another.
if not defined GAME set GAME=C:\factorio-in-kpp
set BUILD=1
set ARGS=
rem The debug build links incrementally into kpp\run; --release (config::debug false, ThinLTO) links
rem whole, into kpp\run-release.
set MODE=--incremental-link
set RUNDIR=kpp\run
:arguments
if "%~1"=="" goto argumentsDone
if "%~1"=="--no-build" (set BUILD=0) else if "%~1"=="--release" (set MODE=--release& set RUNDIR=kpp\run-release) else (set ARGS=%ARGS% %1)
shift
goto arguments
:argumentsDone
if %BUILD%==0 goto run
call native\build.cmd >nul || exit /b 1
call "%HERE%link-settings.cmd"
rem A PDB of the functions' names (the objects carry no debug information), and the crash report,
rem which only this link pulls in. The link is incremental (docs/compile-speed.md, "The link in the
rem edit loop"): kpp\run keeps factorio.exe and its .ilk between builds, and a build relinks only
rem the objects that changed (0.6 s instead of 2.3). So the executable is never deleted first: the
rem link's own line in build.txt ("+ link ... (exit 0)") says whether it linked.
set LIBS=%LIBS% --lib -Wl,/DEBUG --lib -ldbghelp --lib -Wl,/INCLUDE:kpp_crash_report_install
if not exist %RUNDIR% mkdir %RUNDIR%
%KPPC% --std ..\..\stdlib %CINC% --errors 0 --unresolved 0 --keep-going --stub-missing %MODE% %LIBS% %KPPC_EXTRA% --exe %RUNDIR%\factorio.exe kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\src > %RUNDIR%\build.txt 2> %RUNDIR%\build-errors.txt
echo == %GAME%\bin\x64\factorio.exe, built by kppc (exit %errorlevel%) with --stub-missing:
%SystemRoot%\System32\findstr.exe /b /c:"stubs:" %RUNDIR%\build.txt
%SystemRoot%\System32\find.exe /c /v "" %RUNDIR%\build-errors.txt
%SystemRoot%\System32\findstr.exe /r /b /c:"clang: .*+ link .*(exit 0)" %RUNDIR%\build.txt >nul || (
  echo the link failed: %RUNDIR%\factorio.exe.link.txt
  exit /b 1
)
if not exist %GAME%\bin\x64 mkdir %GAME%\bin\x64
copy /y %RUNDIR%\factorio.exe %GAME%\bin\x64\factorio.exe >nul || exit /b 1
if exist %RUNDIR%\factorio.pdb copy /y %RUNDIR%\factorio.pdb %GAME%\bin\x64\factorio.pdb >nul
:run
rem The config file the game looks for next to its root: the config, and from it the write
rem directory, in run\ (use-system-read-write-data-directories=false: never %APPDATA%).
if not exist %GAME%\config-path.cfg (
  echo config-path=__PATH__executable__/../../run/config
  echo use-system-read-write-data-directories=false
) > %GAME%\config-path.cfg
if not exist %GAME%\run\config mkdir %GAME%\run\config
rem Its first line is the version comment the game writes (GlobalContext's configVersion, read by
rem PropertyTreeIni::readIni): without it the game takes the file for an old one and asks to reset
rem it before the main menu. check-updates=false as in the C++ build's config: no login prompt for
rem the updater (and no call to the update server). One written before without them is written again.
if exist %GAME%\run\config\config.ini %SystemRoot%\System32\findstr.exe /b /c:"; version=" %GAME%\run\config\config.ini >nul || del %GAME%\run\config\config.ini
if exist %GAME%\run\config\config.ini %SystemRoot%\System32\findstr.exe /b /c:"check-updates=false" %GAME%\run\config\config.ini >nul || del %GAME%\run\config\config.ini
if not exist %GAME%\run\config\config.ini (
  echo ; version=13
  echo [path]
  echo read-data=__PATH__executable__/../../data
  echo write-data=__PATH__executable__/../../run
  echo [other]
  echo check-updates=false
) > %GAME%\run\config\config.ini
rem The C++ log loads the game's test_mod, so this run does too, from a copy (a mod the game finds
rem new is enabled): then its checksums, the prototype list's 486241314 among them, are comparable.
if not exist %GAME%\run\mods\test_mod_1.0.0 xcopy /e /i /q %FACTORIO%\tests\mods\test_mod_1.0.0 %GAME%\run\mods\test_mod_1.0.0 >nul
echo == running %GAME%\bin\x64\factorio.exe --no-crash-handler%ARGS%
%GAME%\bin\x64\factorio.exe --no-crash-handler%ARGS%
echo == factorio exit %errorlevel%; the log: %GAME%\run\factorio-current.log
