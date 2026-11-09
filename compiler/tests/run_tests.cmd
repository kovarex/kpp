@echo off
rem run_tests: compiles every tests\run\*.kpp with kpp, runs it, and compares stdout with the .expected
rem file; then the other checks (tests\*.ps1, tests\inline.cmd's sections, the libraries' tests). They
rem run side by side, each a process of its own (tests\run_tests.ps1, which lists them). The last line
rem sums up every part, and the exit code agrees with it:
rem   run_tests: ALL PASSED (end-to-end 45/45, checks 27/27)                      exit 0
rem   run_tests: 2 FAILED: e2e enums, targets (end-to-end 44/45, checks 26/27)     exit 1
rem A check that cannot run here (lsp-smoke without Node.js) is added as ", skipped: ...".
rem --part 1 or --part 2 runs one half, for CI's two parallel jobs (docs/build.md, "Continuous
rem integration"): part 2 is determinism, concurrency, generate, project and format ours, part 1 the rest; its
rem line names it: run_tests: ALL PASSED (part 2/2: checks 4/4). --jobs N: how many checks at once
rem (by default a third of the logical processors, 4 to 12). Without --full, the quick gate before a
rem push: the longest checks left out (tests\run_tests.ps1 names them) and determinism's quick mode;
rem --full, all of it: CI on every push and the batch runs on master.
setlocal
rem (before any shift, which moves %0 too)
set "HERE=%~dp0"
set PART=
set JOBS=0
set FULL=
:args
if "%~1"=="" goto :run
if "%~1"=="--part" (set "PART=%~2"& shift & shift & goto :args)
if "%~1"=="--jobs" (set "JOBS=%~2"& shift & shift & goto :args)
if "%~1"=="--full" (set "FULL=-Full"& shift & goto :args)
echo run_tests.cmd takes --part 1 or --part 2, --jobs N and --full
echo run_tests: FAILED: bad arguments %*
exit /b 2
:run
if not "%PART%"=="" if not "%PART%"=="1" if not "%PART%"=="2" (
  echo run_tests.cmd takes --part 1 or --part 2, --jobs N and --full
  echo run_tests: FAILED: bad arguments
  exit /b 2
)
set "PARTARG="
if defined PART set "PARTARG=-Part %PART%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%run_tests.ps1" %PARTARG% -Jobs %JOBS% %FULL%
exit /b %errorlevel%
