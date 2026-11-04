@echo off
rem run_tests' own checks, one per call (tests\run_tests.ps1 runs them side by side, each a process):
rem   inline.cmd e2e NAME | access | diagnostics plain|msvc | clashes | missing | runbuild | allowed |
rem              rejected | library NAME | kppsite
rem Run from compiler\. Prints "ok   ..." or "FAIL ..." and exits 0 or 1.
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
set KPPC=build\kppc.exe
if not exist build\tests mkdir build\tests
goto :%~1

rem An end-to-end program: tests\run\NAME.kpp compiled, run, and its output compared with NAME.expected.
:e2e
set NAME=%~2
%KPPC% --quiet --exe build\tests\%NAME%.exe tests\run\%NAME%.kpp > build\tests\%NAME%.compile.txt 2>&1
if errorlevel 1 (
  echo FAIL %NAME%: compile
  type build\tests\%NAME%.compile.txt
  exit /b 1
)
build\tests\%NAME%.exe > build\tests\%NAME%.out.txt
fc /w build\tests\%NAME%.out.txt tests\run\%NAME%.expected > nul
if errorlevel 1 (
  echo FAIL %NAME%: output differs
  fc /w build\tests\%NAME%.out.txt tests\run\%NAME%.expected
  exit /b 1
)
echo ok   %NAME%
rem (a passing program's build goes: its objects and IR, about 400 files at kppc's default modules)
del /q build\tests\%NAME%.exe build\tests\%NAME%.exe.* build\tests\%NAME%.compile.txt build\tests\%NAME%.out.txt 2> nul
exit /b 0

rem Access (docs/design.md, "struct and class"): the errors of the uses it denies, and with --access-warnings
rem the notes of those friends allow, on tests\access\access.kpp.
:access
%KPPC% --quiet --access-warnings --exe build\tests\access.exe tests\access\access.kpp 2> build\tests\access.all.txt > nul
%SystemRoot%\System32\findstr.exe /b /c:"tests\access\access.kpp" build\tests\access.all.txt > build\tests\access.warnings.txt
fc /w build\tests\access.warnings.txt tests\access\access.expected > nul
if errorlevel 1 (
  echo FAIL access warnings: build\tests\access.warnings.txt differs from tests\access\access.expected
  fc /w build\tests\access.warnings.txt tests\access\access.expected
  exit /b 1
)
echo ok   access warnings
exit /b 0

rem Diagnostics (docs/build.md, "Visual Studio"): each labelled error, a code generator's gap too, at
rem path:line, and with --msvc-diagnostics at path(line), which Visual Studio reads. Sorted: codegen's
rem errors come in the order its threads finish.
:diagnostics
set M=%~2
if %M%==msvc (set FORM=--msvc-diagnostics) else (set FORM=)
%KPPC% --quiet %FORM% --exe build\tests\diagnostics-%M%.exe tests\diagnostics\diagnostics.kpp 2>&1 | %SystemRoot%\System32\sort.exe > build\tests\%M%.errors.txt
fc /w build\tests\%M%.errors.txt tests\diagnostics\%M%.expected > nul
if errorlevel 1 (
  echo FAIL %M% diagnostics: build\tests\%M%.errors.txt differs from tests\diagnostics\%M%.expected
  fc /w build\tests\%M%.errors.txt tests\diagnostics\%M%.expected
  exit /b 1
)
echo ok   %M% diagnostics
exit /b 0

rem Top-level names (docs/design.md, "One global scope, so clashes are errors"): a name two of the
rem program's files declare is an error, a static one's too (not local to its file yet), and no code
rem is generated; one type named twice or one constant written alike in two files is one name.
:clashes
%KPPC% --quiet --exe build\tests\clashes.exe tests\clashes\errors 2>&1 | %SystemRoot%\System32\sort.exe > build\tests\clashes.errors.txt
fc /w build\tests\clashes.errors.txt tests\clashes\errors.expected > nul
if errorlevel 1 (
  echo FAIL clashes: build\tests\clashes.errors.txt differs from tests\clashes\errors.expected
  fc /w build\tests\clashes.errors.txt tests\clashes\errors.expected
  exit /b 1
)
echo ok   clashes
exit /b 0

rem An input path that names no file or directory: an error naming it and exit 1, not a crash in the
rem listing of the sources ("fatal: recursive_directory_iterator ...").
:missing
%KPPC% --quiet --exe build\tests\missing.exe tests\no-such-input > build\tests\missing.out.txt 2>&1
set RC=!errorlevel!
%SystemRoot%\System32\findstr.exe /c:"error: tests\no-such-input: no such file or directory" build\tests\missing.out.txt > nul
if errorlevel 1 (
  echo FAIL missing input: build\tests\missing.out.txt does not name tests\no-such-input
  type build\tests\missing.out.txt
  exit /b 1
)
if !RC! neq 1 (
  echo FAIL missing input: exit !RC!, 1 expected
  exit /b 1
)
echo ok   missing input
exit /b 0

rem build.kpp at compile time (docs/design.md 12, GlobalScope::runBuild): the description of each
rem configuration, computed by the interpreter with Vector and String; a build() that calls C is an
rem error at its line.
:runbuild
(for %%c in (debug release) do %KPPC% --quiet --run-build %%c tests\runbuild\project.kpp tests\runbuild\build.kpp) > build\tests\runbuild.txt 2>&1
%KPPC% --quiet --run-build debug tests\runbuild\project.kpp tests\runbuild\calls_c.kpp 2> build\tests\runbuild.errors.txt > nul
fc /w build\tests\runbuild.txt tests\runbuild\descriptions.expected > nul
if errorlevel 1 (
  echo FAIL runbuild: build\tests\runbuild.txt differs from tests\runbuild\descriptions.expected
  fc /w build\tests\runbuild.txt tests\runbuild\descriptions.expected
  exit /b 1
)
fc /w build\tests\runbuild.errors.txt tests\runbuild\errors.expected > nul
if errorlevel 1 (
  echo FAIL runbuild errors: build\tests\runbuild.errors.txt differs from tests\runbuild\errors.expected
  fc /w build\tests\runbuild.errors.txt tests\runbuild\errors.expected
  exit /b 1
)
echo ok   runbuild
exit /b 0

rem A name declared alike in two files is one name: the program builds and runs.
:allowed
%KPPC% --quiet --exe build\tests\allowed.exe tests\clashes\allowed > build\tests\allowed.compile.txt 2>&1
if errorlevel 1 (
  echo FAIL clashes allowed: compile
  type build\tests\allowed.compile.txt
  exit /b 1
)
build\tests\allowed.exe > build\tests\allowed.out.txt
fc /w build\tests\allowed.out.txt tests\clashes\allowed.expected > nul
if errorlevel 1 (
  echo FAIL clashes allowed: output differs
  exit /b 1
)
echo ok   clashes allowed
exit /b 0

rem A module clang rejects (here: a flag clang does not know) is an error whatever --quiet says, with
rem clang's message, and the build fails, --keep-going or not.
:rejected
%KPPC% --quiet --keep-going --clang-flags -fno-such-kpp-flag --exe build\tests\rejected.exe tests\debuginfo\lines.kpp > nul 2> build\tests\rejected.errors.txt
if not errorlevel 1 (
  echo FAIL rejected module: the build exited 0
  exit /b 1
)
%SystemRoot%\System32\findstr.exe /c:"error: clang rejected module 0 (exit 1)" build\tests\rejected.errors.txt > nul && %SystemRoot%\System32\findstr.exe /c:"-fno-such-kpp-flag" build\tests\rejected.errors.txt > nul
if errorlevel 1 (
  echo FAIL rejected module: build\tests\rejected.errors.txt does not report module 0 with clang's message
  exit /b 1
)
echo ok   rejected module reported
exit /b 0

rem A library in ..\libraries (libraries\README.md): its own tests, kpp --test in its directory (its
rem build.kpp, so the libraries it uses come in: mariadb uses net).
:library
set L=%~2
pushd ..\libraries\%L%
..\..\compiler\%KPPC% --no-server --test > ..\..\compiler\build\tests\library-%L%.txt 2>&1
set LIBRARY_EXIT=!errorlevel!
popd
if !LIBRARY_EXIT! neq 0 (
  echo FAIL library %L%: kpp --test in ..\libraries\%L%
  type build\tests\library-%L%.txt
  exit /b 1
)
echo ok   library %L%
exit /b 0

rem kpp.net's site generator (kppnet\site\README.md): its tests, and a run over this repository with
rem fixture\registry as the registry's store, which must write the fixture's library pages.
:kppsite
pushd ..\kppnet\site
..\..\compiler\%KPPC% --no-server --test > ..\..\compiler\build\tests\kppsite-tests.txt 2>&1
set SITE_TESTS=!errorlevel!
..\..\compiler\%KPPC% --no-server --debug > ..\..\compiler\build\tests\kppsite-build.txt 2>&1
build\debug\kppsite.exe --out ..\..\compiler\build\tests\site --repository ..\.. --registry fixture\registry > ..\..\compiler\build\tests\kppsite-run.txt 2>&1
set SITE_RUN=!errorlevel!
popd
if !SITE_TESTS! neq 0 (
  echo FAIL kppsite tests
  type build\tests\kppsite-tests.txt
  exit /b 1
)
if !SITE_RUN! neq 0 (
  echo FAIL kppsite run
  type build\tests\kppsite-build.txt build\tests\kppsite-run.txt
  exit /b 1
)
if not exist build\tests\site\libraries\thirdparty.html (
  echo FAIL kppsite run: no page for the fixture registry's library
  exit /b 1
)
echo ok   kppsite
exit /b 0
