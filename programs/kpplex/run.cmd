@echo off
rem Builds kpplex from kpp and from the C++ transliteration, then times both on the Factorio
rem corpus, followed by kppc's own (thread-safe) lexer on one thread. See docs/prototype-results.md.
setlocal
cd /d "%~dp0"
..\..\compiler\build\kppc.exe --release --quiet --exe kpplex.exe kpplex.kpp || exit /b 1
call build_cpp.cmd || exit /b 1
set CORPUS=C:\Factorio\src C:\Factorio\tests C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui
echo == kpplex, kpp
kpplex.exe %CORPUS%
echo == kpplex, C++ transliteration, same clang
kpplex_cpp.exe %CORPUS%
echo == kppc --lex-only -j 1 (the thread-safe lexer, best of 3)
..\..\compiler\build\kppc.exe --lex-only -j 1 --repeat 3 %CORPUS%
