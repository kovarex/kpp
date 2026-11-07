@echo off
rem Configures (once) and builds kpp with clang-cl + Ninja into build\ next to this script.
setlocal
cd /d "%~dp0"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
rem Visual Studio: the newest with the Clang component, any edition (a CI runner has Enterprise),
rem else Community 2022.
set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VSDEVCMD=%%i\Common7\Tools\VsDevCmd.bat"
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
rem KPP_CCACHE names a ccache.exe that compiles through its cache (CI's build job, docs/build.md
rem "Continuous integration"); it is taken when build\ is configured.
set LAUNCHER=
if defined KPP_CCACHE set LAUNCHER="-DCMAKE_C_COMPILER_LAUNCHER=%KPP_CCACHE%" "-DCMAKE_CXX_COMPILER_LAUNCHER=%KPP_CCACHE%"
if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release %LAUNCHER% || exit /b 1
)
rem KPP_LLVM_DIR: LLVM's libraries and headers (the official clang+llvm-19.1.7 release's, as llvm.ps1
rem unpacks them) for the back end inside kpp (src\backend.cpp); set, it is configured in, and cleared
rem with KPP_LLVM_DIR=none.
if defined KPP_LLVM_DIR (
  if /i "%KPP_LLVM_DIR%"=="none" (cmake build -DKPP_LLVM_DIR= >nul || exit /b 1) else (cmake build "-DKPP_LLVM_DIR=%KPP_LLVM_DIR%" >nul || exit /b 1)
)
cmake --build build %* || exit /b 1
rem The compiler was kppc.exe until 2026-11-07 (docs/build.md, question 2: "kpp everywhere"): a
rem worktree's old one goes, so that a script still naming it fails instead of running a stale compiler.
for %%f in (build\kppc.exe build\kppc.pdb build\kppc.ilk) do if exist %%f del /q %%f
