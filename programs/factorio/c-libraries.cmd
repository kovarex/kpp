@echo off
rem The C libraries converted to Kpp by C's rules (kppc --c-sources; docs/path-to-factorio.md,
rem "Converting the C libraries"), each once its differential harness (harness.cmd NAME) was
rem byte-identical: the game compiles their Kpp instead of linking the C library. Each has its
rem directory c-libraries\NAME: the conversion's overrides (expand-macros.txt) and includes.txt, the
rem directories below FACTORIO\libraries\NAME of its headers, which stay C: the converted files and
rem the C libraries that use it (vorbis uses ogg) import them.
rem Called by convert.cmd, link-settings.cmd and tests.cmd; needs FACTORIO. Sets CLIBRARIES (the
rem names), CLIBINC (a --cinclude for each header directory) and CLIBIN_NAME (the other C interfaces a
rem library uses, parsed with it and not written: c-libraries\NAME\inputs.txt, paths below
rem FACTORIO\libraries; minizip parses zlib.h).
set CLIBRARIES=ogg zlib glad zstd minizip
set CLIBINC=
for %%l in (%CLIBRARIES%) do for /f "usebackq delims=" %%i in ("%~dp0c-libraries\%%l\includes.txt") do call set CLIBINC=%%CLIBINC%% --cinclude %FACTORIO%\libraries\%%l\%%i
for %%l in (%CLIBRARIES%) do set CLIBIN_%%l=
for %%l in (%CLIBRARIES%) do if exist "%~dp0c-libraries\%%l\inputs.txt" for /f "usebackq eol=# delims=" %%i in ("%~dp0c-libraries\%%l\inputs.txt") do call set CLIBIN_%%l=%%CLIBIN_%%l%% %FACTORIO%\libraries\%%i
