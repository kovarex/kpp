@echo off
rem The markdown library against every example of the CommonMark spec and the GFM spec's table
rem examples (README.md, "Conformance"). The specs are CC-BY-SA 4.0, so their examples are fetched
rem here, at the commits below, into %TEMP%\kpp-conformance\markdown, never into the repository.
setlocal
set HERE=%~dp0
set OUT=%TEMP%\kpp-conformance\markdown
set COMMONMARK=8a5f7646e9f67c3004f023d262b3e6b596afc2ed
set GFM=27d942c8b0a62d192f616e5bf3578f4b6a89e180
if not exist "%OUT%\tests" mkdir "%OUT%\tests"
call :fetch commonmark-spec https://github.com/commonmark/commonmark-spec.git %COMMONMARK% || exit /b 1
call :fetch cmark-gfm https://github.com/github/cmark-gfm.git %GFM% || exit /b 1
python -I "%HERE%generate_spec.py" "%OUT%\commonmark-spec" "%OUT%\cmark-gfm" "%OUT%\tests" || exit /b 1
copy /y "%HERE%CommonMarkTests.kpp.in" "%OUT%\tests\CommonMarkTests.kpp" > nul
"%HERE%..\..\..\compiler\build\kpp.exe" --no-server --test "%HERE%..\src" "%OUT%\tests"
exit /b %errorlevel%

rem A repository at one commit, fetched once.
:fetch
if exist "%OUT%\%1\.git" (
  git -C "%OUT%\%1" cat-file -e %3 2>nul && (git -C "%OUT%\%1" checkout -q %3 & exit /b 0)
)
if not exist "%OUT%\%1" git init -q "%OUT%\%1"
git -C "%OUT%\%1" fetch -q --depth 1 %2 %3 || exit /b 1
git -C "%OUT%\%1" checkout -q FETCH_HEAD
exit /b %errorlevel%
