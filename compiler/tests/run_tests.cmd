@echo off
rem Compiles every tests\run\*.kpp with kppc, runs it, and compares stdout with the .expected file.
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
set KPPC=build\kppc.exe
if not exist build\tests mkdir build\tests
set FAIL=0
set PASS=0
for %%f in (tests\run\*.kpp) do (
  set NAME=%%~nf
  %KPPC% --quiet --exe build\tests\!NAME!.exe %%f > build\tests\!NAME!.compile.txt 2>&1
  if errorlevel 1 (
    echo FAIL !NAME!: compile
    type build\tests\!NAME!.compile.txt
    set /a FAIL+=1
  ) else (
    build\tests\!NAME!.exe > build\tests\!NAME!.out.txt
    fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected > nul
    if errorlevel 1 (
      echo FAIL !NAME!: output differs
      fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected
      set /a FAIL+=1
    ) else (
      echo ok   !NAME!
      set /a PASS+=1
    )
  )
)
echo %PASS% passed, %FAIL% failed
rem --access-warnings (docs/design.md, "struct and class"): the warnings and notes on tests\access\access.kpp.
%KPPC% --quiet --access-warnings --exe build\tests\access.exe tests\access\access.kpp 2> build\tests\access.all.txt > nul
%SystemRoot%\System32\findstr.exe /b /c:"tests\access\access.kpp" build\tests\access.all.txt > build\tests\access.warnings.txt
fc /w build\tests\access.warnings.txt tests\access\access.expected > nul
if errorlevel 1 (
  echo FAIL access warnings: build\tests\access.warnings.txt differs from tests\access\access.expected
  fc /w build\tests\access.warnings.txt tests\access\access.expected
  set /a FAIL+=1
) else (
  echo ok   access warnings
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
    set /a FAIL+=1
  ) else (
    echo ok   %%m diagnostics
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
  set /a FAIL+=1
) else (
  echo ok   clashes
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
  set /a FAIL+=1
) else (
  fc /w build\tests\runbuild.errors.txt tests\runbuild\errors.expected > nul
  if errorlevel 1 (
    echo FAIL runbuild errors: build\tests\runbuild.errors.txt differs from tests\runbuild\errors.expected
    fc /w build\tests\runbuild.errors.txt tests\runbuild\errors.expected
    set /a FAIL+=1
  ) else (
    echo ok   runbuild
  )
)
%KPPC% --quiet --exe build\tests\allowed.exe tests\clashes\allowed > build\tests\allowed.compile.txt 2>&1
if errorlevel 1 (
  echo FAIL clashes allowed: compile
  type build\tests\allowed.compile.txt
  set /a FAIL+=1
) else (
  build\tests\allowed.exe > build\tests\allowed.out.txt
  fc /w build\tests\allowed.out.txt tests\clashes\allowed.expected > nul
  if errorlevel 1 (
    echo FAIL clashes allowed: output differs
    set /a FAIL+=1
  ) else (
    echo ok   clashes allowed
  )
)
rem A module clang rejects (here: a flag clang does not know) is an error whatever --quiet says, with
rem clang's message, and the build fails, --keep-going or not.
%KPPC% --quiet --keep-going --clang-flags -fno-such-kpp-flag --exe build\tests\rejected.exe tests\debuginfo\lines.kpp > nul 2> build\tests\rejected.errors.txt
if not errorlevel 1 (
  echo FAIL rejected module: the build exited 0
  set /a FAIL+=1
) else (
  %SystemRoot%\System32\findstr.exe /c:"error: clang rejected module 0 (exit 1)" build\tests\rejected.errors.txt > nul && %SystemRoot%\System32\findstr.exe /c:"-fno-such-kpp-flag" build\tests\rejected.errors.txt > nul
  if errorlevel 1 (
    echo FAIL rejected module: build\tests\rejected.errors.txt does not report module 0 with clang's message
    set /a FAIL+=1
  ) else (
    echo ok   rejected module reported
  )
)
rem --debug-info: the line tables in the PDB (tests\debuginfo.ps1, docs/debugging.md).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\debuginfo.ps1 -Kppc %KPPC%
if errorlevel 1 set /a FAIL+=1
rem --target (tests\targets.ps1, docs/design.md "Targets and hosts"): Windows, FreeBSD's IR and object,
rem Linux built here and run under WSL (skipped without WSL).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\targets.ps1 -Kppc %KPPC%
if errorlevel 1 set /a FAIL+=1
rem The output must not depend on the thread count (tests\determinism.ps1, docs/compile-speed.md).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\determinism.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL determinism: the output depends on the thread count
  set /a FAIL+=1
)
rem Nor on timing under load: hundreds of builds at -j 32, 16 at once (tests\concurrency.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\concurrency.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL concurrency: the output depends on timing
  set /a FAIL+=1
)
rem Project files for an IDE (tests\generate.ps1, docs/build.md): generated, built by MSBuild, errors in its form.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\generate.ps1 -Kppc %KPPC%
if errorlevel 1 set /a FAIL+=1
rem A project's build.kpp (tests\project.ps1, docs/build.md): kpp new, two executables, settings, errors.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\project.ps1 -Kppc %KPPC%
if errorlevel 1 set /a FAIL+=1
rem The language server (tests\lsp.ps1, docs/ide.md): a scripted editor session, every message it sends.
powershell -NoProfile -ExecutionPolicy Bypass -File tests\lsp.ps1 -Kppc %KPPC%
if errorlevel 1 set /a FAIL+=1
rem What editors' LSP clients rely on (docs/editors.md), against kppc --lsp; skipped without Node.js.
where node >nul 2>nul && (
  node ..\tools\vscode\test\lsp-smoke.js %KPPC% --lsp
  if errorlevel 1 set /a FAIL+=1
)
rem The compile server must build what a new process builds, edit after edit (tests\incremental.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File tests\incremental.ps1 -Kppc %KPPC%
if errorlevel 1 (
  echo FAIL incremental: the compile server's build differs from a new process's
  set /a FAIL+=1
)
rem The libraries in ..\libraries (libraries\README.md): each one's own tests, kppc --test on its directory.
for /d %%l in (..\libraries\*) do (
  %KPPC% --no-server --test %%l > build\tests\library-%%~nl.txt 2>&1
  if errorlevel 1 (
    echo FAIL library %%~nl: kppc --test %%l
    type build\tests\library-%%~nl.txt
    set /a FAIL+=1
  ) else (
    echo ok   library %%~nl
  )
)
if %FAIL% neq 0 exit /b 1
rem (Explicitly: the batch file's exit code would otherwise be the determinism check's.)
exit /b 0
