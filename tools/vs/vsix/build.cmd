@echo off
rem Builds tools\vs\kpp-visualstudio.vsix, the kpp extension for Visual Studio 2022 (tools/vs/vsix/README.md), with
rem Visual Studio's MSBuild. Needs the .NET 8 SDK and the "Visual Studio extension development" workload.
setlocal
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set MSBUILD=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if not defined MSBUILD echo build.cmd: Visual Studio's MSBuild not found & exit /b 1
set DOTNET=
for /f "usebackq delims=" %%i in (`where dotnet 2^>nul`) do if not defined DOTNET set DOTNET=%%i
if not defined DOTNET if exist "%ProgramFiles%\dotnet\dotnet.exe" set DOTNET=%ProgramFiles%\dotnet\dotnet.exe
if not defined DOTNET echo build.cmd: the .NET 8 SDK is not installed: winget install --id Microsoft.DotNet.SDK.8 -e & exit /b 1
rem Visual Studio's MSBuild finds the .NET SDK only with its .NET workload's resolver; without it, the
rem SDK's own Sdks directory and no workload resolver (a standalone .NET 8 SDK is enough).
set SDKVERSION=
for /f "usebackq tokens=1" %%v in (`"%DOTNET%" --list-sdks`) do echo %%v| findstr /b "8." >nul && set SDKVERSION=%%v
if not defined SDKVERSION echo build.cmd: no .NET 8 SDK in "%DOTNET%" --list-sdks & exit /b 1
for %%X in ("%DOTNET%") do set MSBuildSDKsPath=%%~dpXsdk\%SDKVERSION%\Sdks
set MSBuildEnableWorkloadResolver=false
rem (each color's default per theme, from the table in make-colors.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-colors.ps1" || exit /b 1
"%MSBUILD%" "%~dp0KppVisualStudio.csproj" -restore -p:Configuration=Release -v:minimal -nologo || exit /b 1
set OUT=
for /r "%~dp0bin\Release" %%f in (kpp-visualstudio.vsix) do if exist "%%f" set OUT=%%f
if not defined OUT echo build.cmd: the build wrote no kpp-visualstudio.vsix & exit /b 1
copy /y "%OUT%" "%~dp0..\kpp-visualstudio.vsix" >nul || exit /b 1
echo build.cmd: wrote %~dp0..\kpp-visualstudio.vsix
