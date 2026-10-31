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
if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build build %*
