# --toolchain (docs/build.md, "The toolchain"; design.md 17): an installation's own toolchain (bin\
# next to kpp.exe: llvm-mingw, building Windows' GNU dialect, windows-gnu) is its default, Visual
# Studio's is --toolchain msvc. Here an installation made of llvm-mingw (tools\sysroot\llvm-mingw.ps1,
# downloaded once into build\llvm-mingw) with this kpp, stdlib\ and projectlib\ put in its root and
# taken out again. Its programs must import nothing a Windows without Visual Studio lacks.
# run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\toolchain'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$KppExe = (Resolve-Path $KppExe).Path
$failed = 0
function Fail([string]$what) { "FAIL toolchain: $what"; $script:failed++ }
Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue

# Runs a command: its stdout and stderr together (as written: not PowerShell's error records), exit code.
function Run([string]$exe, [string[]]$arguments)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $exe
  $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
  $info.UseShellExecute = $false
  $info.WorkingDirectory = (Get-Location).Path
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $p = [System.Diagnostics.Process]::Start($info)
  $errTask = $p.StandardError.ReadToEndAsync()
  $out = $p.StandardOutput.ReadToEnd()
  $p.WaitForExit()
  return [pscustomobject]@{ exit = $p.ExitCode; text = (($out + $errTask.Result) -replace "`r", '') }
}

# A clang older than kpp's IR needs (KPP_CLANG naming one that says it is clang 18, as Ubuntu 24.04's
# default is): kpp stops before anything compiles and says which, never an undefined
# llvm.eh.typeid.for.p0 at the link (thread E's Linux host, 2026-11-06).
$oldClang = Join-Path $work 'clang18.cmd'
Set-Content -Encoding ascii $oldClang "@echo off`r`necho Ubuntu clang version 18.1.3 (1ubuntu1)`r`necho Target: x86_64-pc-linux-gnu"
Set-Content -Encoding ascii "$work\old.kpp" "int main() { return 0; }"
$env:KPP_CLANG = $oldClang
try
{
  $r = Run $KppExe @('--exe', "$work\old.exe", "$work\old.kpp")
  if ($r.exit -ne 2 -or $r.text -notmatch [regex]::Escape($oldClang) + ' is clang 18, and kpp''s IR needs clang 19 or later' -or (Test-Path "$work\old.exe")) { Fail "a clang 18 named by KPP_CLANG was not refused: exit $($r.exit), $($r.text)" }
}
finally { Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue }

$inst = (& "$PSScriptRoot\..\..\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1)
foreach ($d in 'stdlib', 'projectlib') { if (Test-Path "$inst\$d") { cmd /c rd /s /q "$inst\$d" } }
Copy-Item $KppExe "$inst\kpp.exe" -Force
Copy-Item -Recurse "$PSScriptRoot\..\..\stdlib" "$inst\stdlib"
Copy-Item -Recurse "$PSScriptRoot\..\..\projectlib" "$inst\projectlib"

# A C library's object that nothing names and that registers itself at startup, as kpp-native's crash
# report does: Visual Studio's CRT runs what .CRT$XCU holds, MinGW-w64's a constructor.
$keptSource = @'
#include <stdio.h>
int kept_marker = 1;
static void kept_report(void) { puts("kept by --keep-symbol"); }
#ifdef _MSC_VER
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*kept_init)(void) = kept_report;
#else
__attribute__((constructor)) static void kept_init(void) { kept_report(); }
#endif
'@

Set-Content -Encoding ascii "$work\probe.kpp" @'
extern "C" import "string.h";
int main()
{
  println("toolchain ", strlen("forty-two"));
  return 0;
}
'@

# Builds probe.kpp with `kpp` and runs it: the report's toolchain line, the output, the exit codes.
function Probe([string]$compiler, [string]$label, [string[]]$more)
{
  $exe = "$work\$label.exe"
  $build = Run $compiler (@('--std', "$inst\stdlib") + $more + @('--exe', $exe, "$work\probe.kpp"))
  $out = if ($build.exit -eq 0 -and (Test-Path $exe)) { (Run $exe @()).text.Trim() } else { '' }
  $line = ($build.text -split "`n" | Where-Object { $_ -like 'toolchain:*' } | Select-Object -First 1)
  return [pscustomobject]@{ code = $build.exit; out = $out; line = "$line".Trim(); report = $build.text; exe = $exe }
}

# The DLLs an executable imports that are not Windows' own (every Windows 10 and 11 has the UCRT).
function ForeignImports([string]$exe)
{
  $dump = Run "$inst\bin\llvm-objdump.exe" @('-p', $exe)
  $dlls = $dump.text -split "`n" | Where-Object { $_ -match 'DLL Name:\s*(\S+)' } | ForEach-Object { $Matches[1] }
  return @($dlls | Where-Object { $_ -notmatch '^(ucrtbase|api-ms-win-crt-[a-z0-9-]+|kernel32|user32|advapi32|shell32|ole32|ws2_32|ntdll)\.dll$' })
}

try
{
  $r = Probe "$inst\kpp.exe" 'default' @()
  if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "the installation's default did not build: $($r.report)" }
  elseif ($r.line -notlike "toolchain: $inst\bin\clang.exe (bundled, windows-gnu*") { Fail "the installation's default is not its bin\ for windows-gnu: '$($r.line)'" }
  elseif ((ForeignImports $r.exe).Count) { Fail "the program imports $((ForeignImports $r.exe) -join ', '), which Windows does not have" }

  $r = Probe "$inst\kpp.exe" 'msvc' @('--toolchain', 'msvc')
  if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "--toolchain msvc did not build: $($r.report)" }
  elseif ($r.line -notlike "toolchain: *(Visual Studio's, windows*") { Fail "--toolchain msvc is not Visual Studio's clang: '$($r.line)'" }

  # A list in the order preferred (the game's build.kpp: msvc where Visual Studio is, else bundled):
  # the first of them that is here, every name checked first.
  $r = Probe "$inst\kpp.exe" 'prefermsvc' @('--toolchain', 'msvc,bundled')
  if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "--toolchain msvc,bundled did not build: $($r.report)" }
  elseif ($r.line -notlike "toolchain: *(Visual Studio's, windows*") { Fail "--toolchain msvc,bundled with Visual Studio here is not Visual Studio's clang: '$($r.line)'" }
  $r = Probe "$inst\kpp.exe" 'preferbundled' @('--toolchain', 'bundled,msvc')
  if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "--toolchain bundled,msvc did not build: $($r.report)" }
  elseif ($r.line -notlike "toolchain: $inst\bin\clang.exe (bundled, windows-gnu*") { Fail "--toolchain bundled,msvc in an installation is not its bin\: '$($r.line)'" }
  $r = Probe $KppExe 'nextone' @('--toolchain', 'bundled,msvc')
  if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "--toolchain bundled,msvc without bin\ did not build with Visual Studio's clang: $($r.report)" }
  elseif ($r.line -notlike "toolchain: *(Visual Studio's, windows*") { Fail "--toolchain bundled,msvc without bin\ is not Visual Studio's clang: '$($r.line)'" }
  $r = Probe $KppExe 'badlist' @('--toolchain', 'msvc,gcc')
  if ($r.code -ne 2 -or $r.report -notmatch 'takes bundled, msvc or a list') { Fail "--toolchain msvc,gcc was not an error: exit $($r.code), $($r.report)" }

  $r = Probe $KppExe 'none' @('--toolchain', 'bundled')
  if ($r.code -ne 2 -or $r.report -notmatch '--toolchain bundled: this installation has no') { Fail "--toolchain bundled without bin\ was not an error: exit $($r.code), $($r.report)" }

  $r = Probe "$inst\kpp.exe" 'incremental' @('--incremental-link')
  if ($r.code -ne 2 -or $r.report -notmatch 'add --toolchain msvc') { Fail "--incremental-link with the bundled toolchain was not an error: exit $($r.code), $($r.report)" }

  $r = Probe "$inst\kpp.exe" 'msvcdialect' @('--target', 'windows')
  if ($r.code -ne 2 -or $r.report -notmatch 'needs Visual Studio') { Fail "--target windows with the bundled toolchain was not an error: exit $($r.code), $($r.report)" }

  $r = Probe $KppExe 'gnuvs' @('--target', 'windows-gnu')
  if ($r.code -ne 2 -or $r.report -notmatch 'needs the bundled toolchain') { Fail "--target windows-gnu with Visual Studio's clang was not an error: exit $($r.code), $($r.report)" }

  # The projects are built as from an MSYS2 shell (MSYSTEM, another clang first on PATH: here a
  # program that is no clang), which must change nothing: kpp's toolchain is its own or Visual Studio's.
  $decoy = Join-Path $work 'decoy'
  New-Item -ItemType Directory -Force $decoy | Out-Null
  Copy-Item "$env:SystemRoot\System32\where.exe" "$decoy\clang.exe"
  $savedPath = $env:PATH
  $env:PATH = "$decoy;$env:PATH"
  $env:MSYSTEM = 'UCRT64'

  # A project with a C library (a C and a C++ source, built with clang's own flags and archived by
  # llvm-ar): its debug build links with lld, whole (no --incremental-link), Windows' DLLs alone.
  $project = Join-Path $work 'hello'
  New-Item -ItemType Directory -Force $project | Out-Null
  Push-Location $project
  try
  {
    $null = Run "$inst\kpp.exe" @('new')
    New-Item -ItemType Directory -Force 'libs\plus' | Out-Null
    Set-Content -Encoding ascii 'libs\plus\plus.c' "#ifdef _MSC_VER`n#error the bundled toolchain builds the GNU dialect`n#endif`nint plus_one(int value) { return value + PLUS; }"
    Set-Content -Encoding ascii 'libs\plus\twice.cpp' "extern `"C`" int twice(int value) { return value * 2; }"
    Set-Content -Encoding ascii 'src\main.kpp' "extern `"C`" int plus_one(int value);`nextern `"C`" int twice(int value);`nint main()`n{`n  println(`"Hello from kpp `", twice(plus_one(20)));`n  return 0;`n}"
    Set-Content -Encoding ascii 'build.kpp' @'
void build(Project& project)
{
  CLibrary& plus = project.cLibrary("plus");
  plus.source("libs/plus");
  plus.define("PLUS=1");
  Executable& hello = project.executable("hello", {"src"});
  hello.link("plus");
}
'@
    $run = Run "$inst\kpp.exe" @('--no-server', '--run')
    if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from kpp 42') { Fail "kpp new with a C library, kpp --run did not run: $($run.text)" }
    # Through the compile server: a C library's change alone (the program's kpp source the same, so kpp
    # replays its build and links nothing) is linked in, by kpp's link after the libraries.
    $run = Run "$inst\kpp.exe" @('--run')
    Set-Content -Encoding ascii 'libs\plus\plus.c' "#ifdef _MSC_VER`n#error the bundled toolchain builds the GNU dialect`n#endif`nint plus_one(int value) { return value + PLUS + 1; }"
    $run = Run "$inst\kpp.exe" @('--run')
    $null = Run "$inst\kpp.exe" @('--stop-server')
    if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from kpp 44') { Fail "a C library's change alone was not linked in through the compile server: $($run.text)" }
    if ($run.text -notmatch 'total [0-9.]+ s: C libraries [0-9.]+ s alongside kpp [0-9.]+ s, link [0-9.]+ s') { Fail "kpp's build line does not give the whole build's time and its split: $($run.text)" }
    Set-Content -Encoding ascii 'libs\plus\plus.c' "#ifdef _MSC_VER`n#error the bundled toolchain builds the GNU dialect`n#endif`nint plus_one(int value) { return value + PLUS; }"
    $null = Run "$inst\kpp.exe" @('--no-server')
    $rsp = Get-ChildItem -Recurse "$project\build" -Filter *.link.rsp | Select-Object -First 1
    if (-not $rsp) { Fail "the project's build left no link response file" }
    else
    {
      $text = Get-Content $rsp.FullName -Raw
      if ($text -notmatch '-fuse-ld=lld' -or $text -match 'INCREMENTAL') { Fail "the project's debug link is not lld's whole link: $text" }
      $exe = Get-ChildItem -Recurse "$project\build" -Filter *.exe | Select-Object -First 1
      if ($exe -and (ForeignImports $exe.FullName).Count) { Fail "the project's program imports $((ForeignImports $exe.FullName) -join ', ')" }
    }
    # An object of the C library that nothing names, which registers itself at startup (as kpp-native's
    # crash report does): left out of the link, unless build.kpp keeps its symbol (--keep-symbol, -u
    # for this toolchain's lld). kpp's build line says the toolchain.
    Set-Content -Encoding ascii 'libs\plus\kept.c' $keptSource
    $run = Run "$inst\kpp.exe" @('--no-server', '--run')
    if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from kpp 42' -or $run.text -match 'kept by --keep-symbol') { Fail "an object nothing names was linked in without --keep-symbol: $($run.text)" }
    if ($run.text -notmatch 'kpp: built .*\(debug, bundled toolchain, total') { Fail "kpp's build line does not say the bundled toolchain: $($run.text)" }
    $built = Get-Content -Raw 'build.kpp'
    Set-Content -Encoding ascii 'build.kpp' ($built -replace 'hello\.link\("plus"\);', "hello.link(`"plus`");`n  for (StringView option : {`"--keep-symbol`", `"kept_marker`"})`n    hello.argument(option);")
    $run = Run "$inst\kpp.exe" @('--no-server', '--run')
    if ($run.exit -ne 0 -or $run.text -notmatch 'kept by --keep-symbol' -or $run.text -notmatch 'Hello from kpp 42') { Fail "--keep-symbol kept_marker did not link in its object (the GNU driver's -u): $($run.text)" }
    Set-Content -Encoding ascii 'build.kpp' $built
    Remove-Item 'libs\plus\kept.c'
  }
  finally { Pop-Location }

  # A project whose build.kpp asks for Visual Studio's toolchain (as the game does, which links
  # Visual Studio's C++ libraries): its C library is built by it too (MSVC's dialect, _MSC_VER), not
  # by the installation's own.
  $vsProject = Join-Path $work 'withvs'
  New-Item -ItemType Directory -Force "$vsProject\libs\plus" | Out-Null
  Push-Location $vsProject
  try
  {
    $null = Run "$inst\kpp.exe" @('new')
    Set-Content -Encoding ascii 'libs\plus\plus.c' "#ifndef _MSC_VER`n#error --toolchain msvc builds the C libraries with Visual Studio's clang`n#endif`nint plus_one(int value) { return value + 1; }"
    Set-Content -Encoding ascii 'src\main.kpp' "extern `"C`" int plus_one(int value);`nint main()`n{`n  println(`"Hello from kpp `", plus_one(41));`n  return 0;`n}"
    Set-Content -Encoding ascii 'build.kpp' @'
void build(Project& project)
{
  CLibrary& plus = project.cLibrary("plus");
  plus.source("libs/plus");
  Executable& withvs = project.executable("withvs", {"src"});
  withvs.link("plus");
  for (StringView option : {"--toolchain", "msvc"})
    withvs.argument(option);
}
'@
    $run = Run "$inst\kpp.exe" @('--no-server', '--run')
    if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from kpp 42') { Fail "a build.kpp asking for --toolchain msvc did not build its C library with it: $($run.text)" }
    # The same asked for on the command line instead (kpp passes --toolchain and its value on).
    $withArguments = Get-Content -Raw 'build.kpp'
    Set-Content -Encoding ascii 'build.kpp' ($withArguments -replace '(?s)  for \(StringView option.*?withvs\.argument\(option\);\r?\n', '')
    cmd /c rd /s /q build
    $run = Run "$inst\kpp.exe" @('--no-server', '--toolchain', 'msvc', '--run')
    if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from kpp 42') { Fail "kpp --toolchain msvc did not build the C library with it: $($run.text)" }
    Set-Content -Encoding ascii 'build.kpp' $withArguments
    # The self-registering object with Visual Studio's toolchain: kept by link.exe's /INCLUDE.
    cmd /c rd /s /q build
    Set-Content -Encoding ascii 'libs\plus\kept.c' $keptSource
    $run = Run "$inst\kpp.exe" @('--no-server', '--run')
    if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from kpp 42' -or $run.text -match 'kept by --keep-symbol') { Fail "an object nothing names was linked in without --keep-symbol (msvc): $($run.text)" }
    if ($run.text -notmatch 'kpp: built .*\(debug, msvc toolchain, total') { Fail "kpp's build line does not say the msvc toolchain: $($run.text)" }
    Set-Content -Encoding ascii 'build.kpp' ($withArguments -replace 'withvs\.link\("plus"\);', "withvs.link(`"plus`");`n  for (StringView option : {`"--keep-symbol`", `"kept_marker`"})`n    withvs.argument(option);")
    $run = Run "$inst\kpp.exe" @('--no-server', '--run')
    if ($run.exit -ne 0 -or $run.text -notmatch 'kept by --keep-symbol' -or $run.text -notmatch 'Hello from kpp 42') { Fail "--keep-symbol kept_marker did not link in its object (link.exe's /INCLUDE): $($run.text)" }
    Set-Content -Encoding ascii 'build.kpp' $withArguments
    Remove-Item 'libs\plus\kept.c'
  }
  finally { Pop-Location }

  # That project where Visual Studio lacks its Clang component (kovarex's other machine), and where
  # there is no Visual Studio: kpp stops at once and says what to install, before any C file compiles
  # (KPP_VSWHERE: a vswhere that answers so).
  $noClang = Join-Path $work 'vswhere-noclang.cmd'
  Set-Content -Encoding ascii $noClang "@echo off`r`necho %* | %SystemRoot%\System32\findstr.exe /c:`"-requires`" >nul && exit /b 0`r`necho C:\Program Files\Microsoft Visual Studio\2022\Community"
  $noVs = Join-Path $work 'vswhere-none.cmd'
  Set-Content -Encoding ascii $noVs "@echo off`r`nexit /b 0"
  Push-Location $vsProject
  try
  {
    foreach ($case in @(@($noClang, 'C\+\+ Clang Compiler for Windows.*does not have: Visual Studio Installer > Modify > Individual components'),
                        @($noVs, 'none is installed: install it with "Desktop development with C\+\+"')))
    {
      $env:KPP_VSWHERE = $case[0]
      if (Test-Path build) { cmd /c rd /s /q build }
      $run = Run "$inst\kpp.exe" @('--no-server')
      if ($run.exit -eq 0 -or $run.text -notmatch $case[1] -or $run.text -match 'compiling') { Fail "--toolchain msvc without Visual Studio's clang ($([IO.Path]::GetFileName($case[0]))): exit $($run.exit), $($run.text)" }
    }
    # kpp --version where its default is the bundled toolchain: whether Visual Studio's is here too,
    # and what to install where it is not (kovarex's other machine took the bundled one unsaid).
    $env:KPP_VSWHERE = $noClang
    $version = Run "$inst\kpp.exe" @('--version')
    if ($version.text -notmatch 'toolchain msvc: none: needs Visual Studio''s "C\+\+ Clang Compiler for Windows".*does not have') { Fail "kpp --version without Visual Studio's clang does not say what to install: $($version.text)" }
    # A Visual Studio whose Clang component only a prerelease one (Preview, Insiders) has: taken.
    $real = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
    if ($real)
    {
      $prerelease = Join-Path $work 'vswhere-prerelease.cmd'
      Set-Content -Encoding ascii $prerelease "@echo off`r`necho %* | %SystemRoot%\System32\findstr.exe /c:`"-prerelease`" >nul || exit /b 0`r`necho $real"
      $env:KPP_VSWHERE = $prerelease
      $r = Probe "$inst\kpp.exe" 'prerelease' @('--toolchain', 'msvc')
      if ($r.code -ne 0 -or $r.line -notlike "toolchain: $real\VC\Tools\Llvm\x64\bin\clang.exe*") { Fail "--toolchain msvc did not take a prerelease Visual Studio's clang: '$($r.line)' $($r.report)" }
    }
    $env:KPP_VSWHERE = $noVs
    # The repository's kpp (no toolchain of its own) without Visual Studio's clang: the same, said for it.
    $r = Probe $KppExe 'novs' @()
    if ($r.code -ne 2 -or $r.report -notmatch 'has no toolchain of its own.*none is installed') { Fail "a kpp without a toolchain and without Visual Studio: exit $($r.code), $($r.report)" }
    # Without Visual Studio, a list goes on to the bundled toolchain (and without that too, it says
    # none of them is here): a project's build.kpp asking for msvc,bundled builds with it, and its
    # debug build links whole (kpp adds no --incremental-link, which the bundled toolchain refuses).
    $r = Probe "$inst\kpp.exe" 'novsbundled' @('--toolchain', 'msvc,bundled')
    if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "--toolchain msvc,bundled without Visual Studio did not build with the bundled toolchain: $($r.report)" }
    elseif ($r.line -notlike "toolchain: $inst\bin\clang.exe (bundled, windows-gnu*") { Fail "--toolchain msvc,bundled without Visual Studio is not the bundled toolchain: '$($r.line)'" }
    $r = Probe $KppExe 'novsnone' @('--toolchain', 'msvc,bundled')
    if ($r.code -ne 2 -or $r.report -notmatch 'none of them is here') { Fail "--toolchain msvc,bundled with neither was not an error: exit $($r.code), $($r.report)" }
    Push-Location $project
    try
    {
      $built = Get-Content -Raw 'build.kpp'
      Set-Content -Encoding ascii 'build.kpp' ($built -replace 'hello\.link\("plus"\);', "hello.link(`"plus`");`n  for (StringView option : {`"--toolchain`", `"msvc,bundled`"})`n    hello.argument(option);")
      cmd /c rd /s /q build
      $run = Run "$inst\kpp.exe" @('--no-server', '--run')
      if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from kpp 42' -or $run.text -notmatch '\(debug, bundled toolchain, total') { Fail "a build.kpp asking for msvc,bundled without Visual Studio did not build with the bundled toolchain: $($run.text)" }
      # ... and says why, as a warning Visual Studio's Error List shows: what Visual Studio lacks.
      elseif ($run.text -notmatch 'warning: hello \(debug\): --toolchain msvc,bundled takes bundled: --toolchain msvc needs Visual Studio 2022 or its Build Tools, and none is installed') { Fail "a build.kpp's msvc,bundled took the bundled toolchain without saying why: $($run.text)" }
      Set-Content -Encoding ascii 'build.kpp' $built
    }
    finally { Pop-Location }
  }
  finally
  {
    Pop-Location
    Remove-Item Env:KPP_VSWHERE -ErrorAction SilentlyContinue
  }
  $env:PATH = $savedPath
  Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
}
finally
{
  Remove-Item "$inst\kpp.exe" -ErrorAction SilentlyContinue
  foreach ($d in 'stdlib', 'projectlib') { if (Test-Path "$inst\$d") { cmd /c rd /s /q "$inst\$d" } }
}

if ($failed -eq 0) { "ok   toolchain: llvm-mingw by default in an installation (windows-gnu, Windows' DLLs alone), msvc on request, a list's first one here, --keep-symbol with both" }
exit $failed
