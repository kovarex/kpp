@echo off
rem A thread's own factorio-in-kpp checkout, for its gates and runs (convert.cmd, tests.cmd,
rem harness.cmd, run.cmd write the checkout FACTORIO_IN_KPP names): a git worktree of C:\factorio-in-kpp
rem (its objects shared, not copied) checked out without data\ and shaders\, which are junctions to
rem C:\factorio-in-kpp's (read only; 4.3 GB not copied). Checked out it is about 0.2 GB; a game
rem build adds build\ (about 3 GB per configuration) and bin\ (about 0.6 GB per configuration),
rem within a thread's scratch budget. C:\factorio-in-kpp itself, which every thread reads, is written
rem only for a factorio-in-kpp push. resources.cmd refuses a checkout whose data\ is a junction: it
rem updates C:\factorio-in-kpp's, which the copies share.
rem
rem   setup-checkout.cmd DIR        then: set FACTORIO_IN_KPP=DIR
setlocal
set MAIN=C:\factorio-in-kpp
if "%~1"=="" (echo usage: setup-checkout.cmd DIR& exit /b 2)
if exist "%~1" (echo setup-checkout: %~1 exists already& exit /b 2)
set COPY=%~f1
git -C %MAIN% worktree add --detach --no-checkout "%COPY%" || exit /b 1
git -C "%COPY%" sparse-checkout set --no-cone "/*" "!/data/" "!/shaders/" || exit /b 1
git -C "%COPY%" checkout -q --detach HEAD || exit /b 1
mklink /J "%COPY%\data" %MAIN%\data >nul || exit /b 1
mklink /J "%COPY%\shaders" %MAIN%\shaders >nul || exit /b 1
echo setup-checkout: %COPY%, a worktree of %MAIN% at its HEAD, data\ and shaders\ linked; set FACTORIO_IN_KPP=%COPY%
