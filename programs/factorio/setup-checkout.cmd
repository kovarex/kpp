@echo off
rem A thread's own factorio-in-kpp checkout, for its gates and runs (convert.cmd, tests.cmd,
rem harness.cmd, run.cmd write the checkout FACTORIO_IN_KPP names): a git worktree of C:\factorio-in-kpp
rem (its objects shared, not copied) checked out without data\ and shaders\ (4.3 GB not copied). The
rem game built there reads C:\factorio-in-kpp's data through its config (config\config.ini's read-data,
rem which build.kpp and run.cmd keep), and writes into the copy. Checked out it is about 0.2 GB; a
rem game build adds build\ (about 3 GB per configuration) and bin\ (about 0.6 GB per configuration),
rem within a thread's scratch budget. C:\factorio-in-kpp itself, which every thread reads, is written
rem only for a factorio-in-kpp push.
rem
rem There is no junction to C:\factorio-in-kpp's data\ in a copy (an earlier version made data\ and
rem shaders\ junctions): git 2.37 and later find a sparse checkout's files that exist on disk outside
rem its patterns, through a junction too, clear their skip-worktree bits, and the next checkout
rem deletes them, which deleted C:\factorio-in-kpp's data\ on 2026-11-03; `git worktree remove` of such
rem a copy deletes through the junctions too (rd /s /q and `git worktree prune` remove it safely).
rem tests\checkout.ps1 checks it.
rem
rem   setup-checkout.cmd DIR [MAIN]          a new copy; then: set FACTORIO_IN_KPP=DIR
rem   setup-checkout.cmd --unlink DIR [MAIN] an existing copy made with junctions: they go (the links
rem                                          alone, never what they point to), and the config is written
rem MAIN is C:\factorio-in-kpp unless given (tests\checkout.ps1 gives a throwaway one).
setlocal
set UNLINK=0
if "%~1"=="--unlink" (set UNLINK=1& shift)
if "%~1"=="" (echo usage: setup-checkout.cmd [--unlink] DIR [MAIN]& exit /b 2)
set COPY=%~f1
set MAIN=C:\factorio-in-kpp
if not "%~2"=="" set MAIN=%~f2
if %UNLINK%==1 goto unlink
if exist "%COPY%" (echo setup-checkout: %COPY% exists already ^(--unlink for a copy with junctions^)& exit /b 2)
git -C "%MAIN%" worktree add --detach --no-checkout "%COPY%" || exit /b 1
git -C "%COPY%" sparse-checkout set --no-cone "/*" "!/data/" "!/shaders/" || exit /b 1
call :expectOutside || exit /b 1
git -C "%COPY%" checkout -q --detach HEAD || exit /b 1
call :config || exit /b 1
echo setup-checkout: %COPY%, a worktree of %MAIN% at its HEAD without data\ and shaders\ (the game reads %MAIN%\data); set FACTORIO_IN_KPP=%COPY%
exit /b 0

:unlink
if not exist "%COPY%\.git" (echo setup-checkout: %COPY% is no checkout& exit /b 2)
rem (a directory that is no junction, the main checkout's own data\, is never touched)
for %%d in (data shaders) do if exist "%COPY%\%%d" call :unlinkOne "%COPY%\%%d" || exit /b 1
call :expectOutside || exit /b 1
call :config || exit /b 1
rem With no junction left, nothing of MAIN's is on disk in the copy: reapply marks the files outside
rem the patterns skip-worktree again (a git that cleared them saw them through the junction).
git -C "%COPY%" sparse-checkout reapply || exit /b 1
echo setup-checkout: %COPY% has no junction to %MAIN% now; the game reads %MAIN%\data
exit /b 0

:unlinkOne
set ATTRIBUTES=%~a1
if "%ATTRIBUTES:l=%"=="%ATTRIBUTES%" (echo setup-checkout: %~1 is no junction: left as it is, and nothing done& exit /b 1)
rem rmdir without /s removes the junction itself, never the directory it points to.
rmdir "%~1" || exit /b 1
echo setup-checkout: removed the junction %~1
exit /b 0

rem The copy's own setting (its worktree config, which sparse-checkout enables): a file outside the
rem patterns that is on disk keeps its skip-worktree bit, so no git command takes it for the copy's.
rem With no junction nothing of MAIN's is there; this is the second guard.
:expectOutside
git -C "%COPY%" config --worktree sparse.expectFilesOutsideOfPatterns true || exit /b 1
exit /b 0

rem The game's config, as build.kpp and run.cmd write it, but reading MAIN's data\: config-path.cfg in
rem the root names config\, whose config.ini they keep (it has the version line and check-updates=false).
:config
set DATA=%MAIN:\=/%/data
(
  echo config-path=__PATH__executable__/../../config
  echo use-system-read-write-data-directories=false
) > "%COPY%\config-path.cfg" || exit /b 1
if not exist "%COPY%\config" mkdir "%COPY%\config"
(
  echo ; version=13
  echo [path]
  echo read-data=%DATA%
  echo write-data=__PATH__executable__/../..
  echo [other]
  echo check-updates=false
) > "%COPY%\config\config.ini" || exit /b 1
exit /b 0
