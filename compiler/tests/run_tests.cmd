@echo off
rem Compiles every tests\run\*.kpp with kppc, runs it, and compares stdout with the .expected file;
rem then the other checks below. The last line sums up every part, and the exit code agrees with it:
rem   run_tests: ALL PASSED (end-to-end 45/45, checks 27/27)                      exit 0
rem   run_tests: 2 FAILED: e2e enums, targets (end-to-end 44/45, checks 26/27)     exit 1
rem A check that cannot run here (lsp-smoke without Node.js) is added as ", skipped: ...".
rem --part 1 or --part 2 runs one half, for CI's two parallel jobs (docs/build.md, "Continuous
rem integration"): part 2 is determinism, concurrency, generate and project, part 1 the rest; its
rem line names it: run_tests: ALL PASSED (part 2/2: checks 4/4).
rem --full runs determinism in full, every thread count and the converted game's IR (tests\determinism.ps1
rem -Full): the batch run on master (H's). Without it, the quick gate's (coordinator, 2026-11-05).
setlocal EnableDelayedExpansion
set PART=
set FULL=
rem (`shift /1`, never `shift`: a plain shift moves %0 too, and `cd /d "%~dp0.."` below then went to the
rem first argument's directory: CI's `run_tests --part 1` ran its checks from the wrong one.)
:arguments
if "%~1"=="" goto :argumentsdone
if "%~1"=="--full" (
  set FULL=1
  shift /1
  goto :arguments
)
if "%~1"=="--part" (
  set "PART=%~2"
  shift /1
  shift /1
  goto :arguments
)
echo run_tests.cmd takes --part 1 or --part 2, --full, or nothing
echo run_tests: FAILED: bad arguments %*
exit /b 2
:argumentsdone
if not "%PART%"=="" if not "%PART%"=="1" if not "%PART%"=="2" (
  echo run_tests.cmd takes --part 1 or --part 2, --full, or nothing
  echo run_tests: FAILED: bad arguments %*
  exit /b 2
)
cd /d "%~dp0.."
rem Streaming's barrier check in codegen (KPP_CHECK_BARRIER, docs/codegen-speed.md), on for every
rem build here until program pieces rely on it.
set KPP_CHECK_BARRIER=1
set KPPC=build\kppc.exe
if not exist build\tests mkdir build\tests
set FAIL=0
set PASS=0
set E2E=0
set CHECKS=0
set CHECKSOK=0
set FAILED=
set SKIPPED=
if "%PART%"=="2" goto :part2
for %%f in (tests\run\*.kpp) do (
  set NAME=%%~nf
  %KPPC% --quiet --exe build\tests\!NAME!.exe %%f > build\tests\!NAME!.compile.txt 2>&1
  if errorlevel 1 (
    echo FAIL !NAME!: compile
    type build\tests\!NAME!.compile.txt
    call :failed "e2e !NAME!"
  ) else (
    build\tests\!NAME!.exe > build\tests\!NAME!.out.txt
    fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected > nul
    if errorlevel 1 (
      echo FAIL !NAME!: output differs
      fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected
      call :failed "e2e !NAME!"
    ) else (
      echo ok   !NAME!
      set /a PASS+=1
      rem (a passing program's build goes: its objects and IR, about 400 files at kppc's default modules)
      del /q build\tests\!NAME!.exe build\tests\!NAME!.exe.* build\tests\!NAME!.compile.txt build\tests\!NAME!.out.txt 2> nul
    )
  )
)
echo %PASS% passed, %FAIL% failed
set /a E2E=PASS+FAIL
rem Access (docs/design.md, "struct and class"): the errors of the uses it denies, and with --access-warnings
rem the notes of those friends allow, on tests\access\access.kpp.
%KPPC% --quiet --access-warnings --exe build\tests\access.exe tests\access\access.kpp 2> build\tests\access.all.txt > nul
%SystemRoot%\System32\findstr.exe /b /c:"tests\access\access.kpp" build\tests\access.all.txt > build\tests\access.warnings.txt
fc /w build\tests\access.warnings.txt tests\access\access.expected > nul
if errorlevel 1 (
  echo FAIL access warnings: build\tests\access.warnings.txt differs from tests\access\access.expected
  fc /w build\tests\access.warnings.txt tests\access\access.expected
  call :fail "access warnings"
) else (
  echo ok   access warnings
  call :ok
)
rem Diagnostics (docs/build.md, "Visual Studio"): each labelled error, a code generator's gap too, at
rem path:line, and with --msvc-diagnostics at path(line), which Visual Studio reads. Sorted: codegen's
rem errors come in the order its threads finish.
for %%m in (plain msvc) do (
  if %%m==msvc (set FORM=--msvc-diagnostics) else (set FORM=)
  %KPPC% --quiet !FORM! --exe build\tests\diagnostics.exe tests\diagnostics\diagnostics.kpp 2>&1 | %SystemRoot%\System32\sort.exe > build\tests\%%m.errors.txt
  fc /w build\tests\%%m.errors.txt tests\diagnostics\%%m.expected > nul
  if errorlevel 1 (
    echo FAIL %%m diagnostics: build\tests\%%m.errors.txt differs from tests\diagnostics\%%m.expected
    fc /w build\tests\%%m.errors.txt tests\diagnostics\%%m.expected
    call :fail "%%m diagnostics"
  ) else (
    echo ok   %%m diagnostics
    call :ok
  )
)
rem Top-level names (docs/design.md, "One global scope, so clashes are errors"): a name two of the
rem program's files declare is an error, a static one's too (not local to its file yet), and no code
rem is generated; one type named twice or one constant written alike in two files is one name.
%KPPC% --quiet --exe build\tests\clashes.exe tests\clashes\errors 2>&1 | %SystemRoot%\System32\sort.exe > build\tests\clashes.errors.txt
fc /w build\tests\clashes.errors.txt tests\clashes\errors.expected > nul
if errorlevel 1 (
  echo FAIL clashes: build\tests\clashes.errors.txt differs from tests\clashes\errors.expected
  fc /w build\tests\clashes.errors.txt tests\clashes\errors.expected
  call :fail "clashes"
) else (
  echo ok   clashes
  call :ok
)
rem An input path that names no file or directory: an error naming it and exit 1, not a crash in the
rem listing of the sources ("fatal: recursive_directory_iterator ...").
%KPPC% --quiet --exe build\tests\missing.exe tests\no-such-input > build\tests\missing.out.txt 2>&1
set RC=!errorlevel!
%SystemRoot%\System32\findstr.exe /c:"error: tests\no-such-input: no such file or directory" build\tests\missing.out.txt > nul
if errorlevel 1 (
  echo FAIL missing input: build\tests\missing.out.txt does not name tests\no-such-input
  type build\tests\missing.out.txt
  call :fail "missing input"
) else if !RC! neq 1 (
  echo FAIL missing input: exit !RC!, 1 expected
  call :fail "missing input"
) else (
  echo ok   missing input
  call :ok
)
rem build.kpp at compile time (docs/design.md 12, GlobalScope::runBuild): the description of each
rem configuration, computed by the interpreter with Vector and String; a build() that calls C is an
rem error at its line.
(for %%c in (debug release) do %KPPC% --quiet --run-build %%c tests\runbuild\project.kpp tests\runbuild\build.kpp) > build\tests\runbuild.txt 2>&1
%KPPC% --quiet --run-build debug tests\runbuild\project.kpp tests\runbuild\calls_c.kpp 2> build\tests\runbuild.errors.txt > nul
fc /w build\tests\runbuild.txt tests\runbuild\descriptions.expected > nul
if errorlevel 1 (
  echo FAIL runbuild: build\tests\runbuild.txt differs from tests\runbuild\descriptions.expected
  fc /w build\tests\runbuild.txt tests\runbuild\descriptions.expected
  call :fail "runbuild"
) else (
  fc /w build\tests\runbuild.errors.txt tests\runbuild\errors.expected > nul
  if errorlevel 1 (
    echo FAIL runbuild errors: build\tests\runbuild.errors.txt differs from tests\runbuild\errors.expected
    fc /w build\tests\runbuild.errors.txt tests\runbuild\errors.expected
    call :fail "runbuild errors"
  ) else (
    echo ok   runbuild
    call :ok
  )
)
%KPPC% --quiet --exe build\tests\allowed.exe tests\clashes\allowed > build\tests\allowed.compile.txt 2>&1
if errorlevel 1 (
  echo FAIL clashes allowed: compile
  type build\tests\allowed.compile.txt
  call :fail "clashes allowed"
) else (
  build\tests\allowed.exe > build\tests\allowed.out.txt
  fc /w build\tests\allowed.out.txt tests\clashes\allowed.expected > nul
  if errorlevel 1 (
    echo FAIL clashes allowed: output differs
    call :fail "clashes allowed"
  ) else (
    echo ok   clashes allowed
    call :ok
  )
)
rem A module clang rejects (here: a flag clang does not know) is an error whatever --quiet says, with
rem clang's message, and the build fails, --keep-going or not.
%KPPC% --quiet --keep-going --clang-flags -fno-such-kpp-flag --exe build\tests\rejected.exe tests\debuginfo\lines.kpp > nul 2> build\tests\rejected.errors.txt
if not errorlevel 1 (
  echo FAIL rejected module: the build exited 0
  call :fail "rejected module"
) else (
  %SystemRoot%\System32\findstr.exe /c:"error: clang rejected module 0 (exit 1)" build\tests\rejected.errors.txt > nul && %SystemRoot%\System32\findstr.exe /c:"-fno-such-kpp-flag" build\tests\rejected.errors.txt > nul
  if errorlevel 1 (
    echo FAIL rejected module: build\tests\rejected.errors.txt does not report module 0 with clang's message
    call :fail "rejected module"
  ) else (
    echo ok   rejected module reported
    call :ok
  )
)
rem The same through LLVM's back end in kppc (tests\backend.ps1), when this kppc has it: an IR bug
rem (the verifier) or what the back end rejects (LLVM's diagnostics) fails its module with LLVM's
rem message, for windows and windows-gnu.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\backend.ps1 -Kppc %KPPC%
if errorlevel 3 (
  if defined SKIPPED (set "SKIPPED=!SKIPPED!, LLVM back end (none in this kppc)") else (set "SKIPPED=LLVM back end (none in this kppc)")
) else if errorlevel 1 (call :fail "LLVM back end") else (call :ok)
rem And through clang (tests\verifier.ps1): its -fverify-intermediate-code rejects the same IR bug, in a
rem release build and a debug build, for windows and windows-gnu, never compiling it or hanging on it.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\verifier.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "verifier") else (call :ok)
rem --keep-going --stub-missing with an error the checker reported (tests\stubs.ps1): the function it
rem left out is a stub in the module that calls it, in one module, four and a release build.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\stubs.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "stubs") else (call :ok)
rem Static locals with constant initialization (tests\staticconstants.ps1): a const or constexpr one
rem whose initializer folds is a constant in the IR, with no guard; a call's keeps its guard.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\staticconstants.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "static constants") else (call :ok)
rem --lto full and --opt-level 3 (tests\lto.ps1, docs/build.md "Whole-program optimization"): a
rem program built as one optimization over the whole program, for Windows and windows-gnu.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\lto.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "lto") else (call :ok)
rem A section per function where the link drops what nobody calls, none in an --incremental-link
rem build, whose link keeps every function (tests\sections.ps1, docs/debugging.md "The edit's link").
powershell -NoProfile -ExecutionPolicy Bypass -File tests\sections.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "sections") else (call :ok)
rem --debug-info: the line tables in the PDB (tests\debuginfo.ps1, docs/debugging.md).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\debuginfo.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "debug info") else (call :ok)
rem --debug-info for Visual Studio: the natvis linked into the PDB (tests\natvis.ps1, docs/debugging.md).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\natvis.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "natvis") else (call :ok)
rem --target (tests\targets.ps1, docs/design.md "Targets and hosts"): Windows, FreeBSD's IR and object,
rem Linux built here and run under WSL (skipped without WSL, or when WSL does not answer in 30 s: exit 3
rem and 4, the rest passed; the summary lists the skip).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\targets.ps1 -Kppc %KPPC%
if errorlevel 4 (
  call :ok
  if defined SKIPPED (set "SKIPPED=!SKIPPED!, linux targets (WSL did not answer in 30 s)") else (set "SKIPPED=linux targets (WSL did not answer in 30 s)")
) else if errorlevel 3 (
  call :ok
  if defined SKIPPED (set "SKIPPED=!SKIPPED!, linux targets (no WSL)") else (set "SKIPPED=linux targets (no WSL)")
) else if errorlevel 1 (call :fail "targets") else (call :ok)
rem What a spec run leaves (tests\specfiles.ps1): a passing snippet's build deleted, a failing one's
rem kept and failing the run.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\specfiles.ps1
if errorlevel 1 (call :fail "specfiles") else (call :ok)
rem kppc --run's build cache in TEMP (tests\runcache.ps1): four modules a build, the 100 most recently
rem used directories kept, one a run holds never deleted.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\runcache.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "runcache") else (call :ok)
rem --toolchain: an installation's bin\ by default, Visual Studio's on request (tests\toolchain.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\toolchain.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "toolchain") else (call :ok)
rem The end-to-end programs in Windows' GNU dialect, with llvm-mingw (tests\windowsgnu.ps1, design.md 17).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\windowsgnu.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "windows-gnu") else (call :ok)
rem The C import cache keys an import on the toolchain's environment (tests\importcache.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\importcache.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "import cache") else (call :ok)
rem A compile server exits once its kppc changes; --stop-all-servers stops them all (tests\servers.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\servers.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "servers") else (call :ok)
rem An edit's check through the compile server takes well under the first build (tests\editcost.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\editcost.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "edit cost") else (call :ok)
rem An editor's check of a program with front-end errors and no codegen session to follow the edit
rem stops at those errors; a server's session still shows codegen's (tests\checkerrors.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\checkerrors.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "check errors") else (call :ok)
rem A thread's factorio-in-kpp copy never reaches the main checkout's files, through any git command in
rem it (programs\factorio\tests\checkout.ps1, on a throwaway main checkout).
powershell -NoProfile -ExecutionPolicy Bypass -File ..\programs\factorio\tests\checkout.ps1
if errorlevel 1 (call :fail "checkout") else (call :ok)
if "%PART%"=="1" goto :afterpart2
:part2
rem The output must not depend on the thread count (tests\determinism.ps1, docs/compile-speed.md):
rem in full with --full, else the quick gate's (fewer thread counts, the game's front end alone).
set DETERMINISM=
if "%FULL%"=="1" set DETERMINISM=-Full
powershell -NoProfile -ExecutionPolicy Bypass -File tests\determinism.ps1 -Kppc %KPPC% %DETERMINISM%
if errorlevel 1 (
  echo FAIL determinism: the output depends on the thread count
  call :fail "determinism"
) else (call :ok)
rem Nor on timing under load: hundreds of builds at -j 32, 16 at once (tests\concurrency.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\concurrency.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL concurrency: the output depends on timing
  call :fail "concurrency"
) else (call :ok)
rem Project files for an IDE (tests\generate.ps1, docs/build.md): generated, built by MSBuild, errors in its form.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\generate.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "generate") else (call :ok)
rem A project's build.kpp (tests\project.ps1, docs/build.md): kpp new, two executables, settings, errors.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\project.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "project") else (call :ok)
if "%PART%"=="2" goto :summary
:afterpart2
rem kpp in lower case (tests\lowercase.ps1, CLAUDE.md): no capitalised "kpp" outside identifiers.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\lowercase.ps1
if errorlevel 1 (call :fail "lower case") else (call :ok)
rem The language server (tests\lsp.ps1, docs/ide.md): a scripted editor session, every message it sends.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\lsp.ps1 -Kppc %KPPC%
if errorlevel 1 (call :fail "lsp") else (call :ok)
rem What editors' LSP clients rely on (docs/editors.md), against kppc --lsp; skipped without Node.js.
where node >nul 2>nul
if errorlevel 1 (
  if defined SKIPPED (set "SKIPPED=!SKIPPED!, lsp-smoke (no Node.js)") else (set "SKIPPED=lsp-smoke (no Node.js)")
) else (
  node ..\tools\vscode\test\lsp-smoke.js %KPPC% --lsp
  if errorlevel 1 (call :fail "lsp-smoke") else (call :ok)
)
rem The compile server must build what a new process builds, edit after edit (tests\incremental.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\incremental.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL incremental: the compile server's build differs from a new process's
  call :fail "incremental"
) else (call :ok)
rem The libraries in ..\libraries (libraries\README.md): each one's own tests, kpp --test in its
rem directory (its build.kpp, so the libraries it uses come in: mariadb uses net).
for /d %%l in (..\libraries\*) do (
  pushd %%l
  ..\..\compiler\%KPPC% --no-server --test > ..\..\compiler\build\tests\library-%%~nl.txt 2>&1
  set LIBRARY_EXIT=!errorlevel!
  popd
  if !LIBRARY_EXIT! neq 0 (
    echo FAIL library %%~nl: kpp --test in %%l
    type build\tests\library-%%~nl.txt
    call :fail "library %%~nl"
  ) else (
    echo ok   library %%~nl
    call :ok
  )
)
rem kpp.net's site generator (kppnet\site\README.md): its tests, and a run over this repository with
rem fixture\registry as the registry's store, which must write the fixture's library pages.
pushd ..\kppnet\site
..\..\compiler\%KPPC% --no-server --test > ..\..\compiler\build\tests\kppsite-tests.txt 2>&1
set SITE_TESTS=%errorlevel%
..\..\compiler\%KPPC% --no-server --debug > ..\..\compiler\build\tests\kppsite-build.txt 2>&1
build\debug\kppsite.exe --out ..\..\compiler\build\tests\site --repository ..\.. --registry fixture\registry > ..\..\compiler\build\tests\kppsite-run.txt 2>&1
set SITE_RUN=%errorlevel%
popd
if %SITE_TESTS% neq 0 (
  echo FAIL kppsite tests
  type build\tests\kppsite-tests.txt
  call :fail "kppsite tests"
) else if %SITE_RUN% neq 0 (
  echo FAIL kppsite run
  type build\tests\kppsite-build.txt build\tests\kppsite-run.txt
  call :fail "kppsite run"
) else if not exist build\tests\site\libraries\thirdparty.html (
  echo FAIL kppsite run: no page for the fixture registry's library
  call :fail "kppsite run"
) else (
  echo ok   kppsite
  call :ok
)
rem The one line that says whether everything passed, and the exit code that says the same
rem (explicitly: the batch file's exit code would otherwise be the last check's).
rem (!...!, not %...%: a skip's reason or a check's name may hold parentheses, which would end the
rem if block below)
:summary
set "SUMMARY=end-to-end %PASS%/%E2E%, checks %CHECKSOK%/%CHECKS%"
if "%PART%"=="1" set "SUMMARY=part 1/2: !SUMMARY!"
if "%PART%"=="2" set "SUMMARY=part 2/2: checks %CHECKSOK%/%CHECKS%"
if defined SKIPPED set "SUMMARY=!SUMMARY!, skipped: !SKIPPED!"
if %FAIL% neq 0 (
  echo run_tests: %FAIL% FAILED: !FAILED! ^(!SUMMARY!^)
  exit /b 1
)
rem Everything passed: the checks' builds go (build\tests held some 50,000 files of objects and IR after
rem a run, and the kernel keeps a record of each cached file in its pool; a failed run keeps them).
rd /s /q build\tests 2> nul
echo run_tests: ALL PASSED ^(!SUMMARY!^)
exit /b 0

rem A check failed (:fail NAME) or passed (:ok); :failed counts a failure, "e2e NAME" or a check's.
:fail
set /a CHECKS+=1
:failed
set /a FAIL+=1
if defined FAILED (set "FAILED=%FAILED%, %~1") else (set "FAILED=%~1")
exit /b 0
:ok
set /a CHECKS+=1
set /a CHECKSOK+=1
exit /b 0
