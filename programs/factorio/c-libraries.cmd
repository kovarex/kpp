@echo off
rem The C libraries converted to kpp by C's rules (kpp --c-sources; docs/path-to-factorio.md,
rem "Converting the C libraries"), each once its differential harness (harness.cmd NAME) was
rem byte-identical: the game compiles their kpp instead of linking the C library. Each has its
rem directory c-libraries\NAME: the conversion's overrides (expand-macros.txt) and includes.txt, the
rem directories below FACTORIO\libraries\NAME of its headers, which stay C: the converted files and
rem the C libraries that use it (vorbis uses ogg) import them.
rem Called by convert.cmd, link-settings.cmd and tests.cmd; needs FACTORIO. Sets CLIBRARIES (the
rem names), CLIBINC (a --cinclude for each header directory) and CLIBIN_NAME (the other C interfaces a
rem library uses, parsed with it and not written: c-libraries\NAME\inputs.txt, paths below
rem FACTORIO\libraries; minizip parses zlib.h), CLIBSRC_NAME (the files converted: every .c and .h of
rem FACTORIO\libraries\NAME, or those its sources.txt names: stb_image's two headers, whose
rem implementations stb_image.cpp compiles) and CLIBNAMES (build.kpp's names of the libraries: a
rem clib.txt's where it is not the directory's, stb_image's stb) and CLIBSCOPED (below).
set CLIBRARIES=ogg zlib glad zstd minizip fontstash trio stb_image
set CLIBINC=
for %%l in (%CLIBRARIES%) do for /f "usebackq delims=" %%i in ("%~dp0c-libraries\%%l\includes.txt") do call set CLIBINC=%%CLIBINC%% --cinclude %FACTORIO%\libraries\%%l\%%i
for %%l in (%CLIBRARIES%) do set CLIBIN_%%l=
for %%l in (%CLIBRARIES%) do if exist "%~dp0c-libraries\%%l\inputs.txt" for /f "usebackq eol=# delims=" %%i in ("%~dp0c-libraries\%%l\inputs.txt") do call set CLIBIN_%%l=%%CLIBIN_%%l%% %FACTORIO%\libraries\%%i
for %%l in (%CLIBRARIES%) do set CLIBSRC_%%l=%FACTORIO%\libraries\%%l
for %%l in (%CLIBRARIES%) do if exist "%~dp0c-libraries\%%l\sources.txt" set CLIBSRC_%%l=
for %%l in (%CLIBRARIES%) do if exist "%~dp0c-libraries\%%l\sources.txt" for /f "usebackq eol=# delims=" %%i in ("%~dp0c-libraries\%%l\sources.txt") do call set CLIBSRC_%%l=%%CLIBSRC_%%l%% %FACTORIO%\libraries\%%l\%%i
set CLIBNAMES=
for %%l in (%CLIBRARIES%) do if exist "%~dp0c-libraries\%%l\clib.txt" (for /f "usebackq eol=# delims=" %%n in ("%~dp0c-libraries\%%l\clib.txt") do call set CLIBNAMES=%%CLIBNAMES%% %%n) else call set CLIBNAMES=%%CLIBNAMES%% %%l
rem CLIBSCOPED: the scoped ones (design.md 16), whose c-libraries\NAME\interface.txt names the headers
rem converted into the library's scope: the game writes their names NAME::name and builds the library
rem as a library (build.kpp's use, kpp's --library-root), not as its own sources.
set CLIBSCOPED=
for %%l in (%CLIBRARIES%) do if exist "%~dp0c-libraries\%%l\interface.txt" call set CLIBSCOPED=%%CLIBSCOPED%% %%l
