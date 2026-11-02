@echo off
rem Configures (once) and builds kppc with clang-cl + Ninja into build\ next to this script.
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
cmake --build build %*
