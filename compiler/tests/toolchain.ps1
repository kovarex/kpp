# --toolchain (docs/build.md, "The toolchain"; design.md 17): an installation's own toolchain (bin\
# next to kpp.exe: llvm-mingw, building Windows' GNU dialect, windows-gnu) is its default, Visual
# Studio's is --toolchain msvc. Here an installation made of llvm-mingw (tools\sysroot\llvm-mingw.ps1,
# downloaded once into build\llvm-mingw) with this kppc, stdlib\ and projectlib\ put in its root and
# taken out again. Its programs must import nothing a Windows without Visual Studio lacks.
# run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\toolchain'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$Kppc = (Resolve-Path $Kppc).Path
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

$inst = (& "$PSScriptRoot\..\..\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1)
foreach ($d in 'stdlib', 'projectlib') { if (Test-Path "$inst\$d") { cmd /c rd /s /q "$inst\$d" } }
Copy-Item $Kppc "$inst\kpp.exe" -Force
Copy-Item -Recurse "$PSScriptRoot\..\..\stdlib" "$inst\stdlib"
Copy-Item -Recurse "$PSScriptRoot\..\..\projectlib" "$inst\projectlib"

Set-Content -Encoding ascii "$work\probe.kpp" @'
extern "C" import "string.h";
int main()
{
  println("toolchain ", strlen("forty-two"));
  return 0;
}
'@

# Builds probe.kpp with `kpp` and runs it: the report's toolchain line, the output, the exit codes.
function Probe([string]$kpp, [string]$label, [string[]]$more)
{
  $exe = "$work\$label.exe"
  $build = Run $kpp (@('--std', "$inst\stdlib") + $more + @('--exe', $exe, "$work\probe.kpp"))
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

  $r = Probe $Kppc 'none' @('--toolchain', 'bundled')
  if ($r.code -ne 2 -or $r.report -notmatch '--toolchain bundled: this installation has no') { Fail "--toolchain bundled without bin\ was not an error: exit $($r.code), $($r.report)" }

  $r = Probe "$inst\kpp.exe" 'incremental' @('--incremental-link')
  if ($r.code -ne 2 -or $r.report -notmatch 'add --toolchain msvc') { Fail "--incremental-link with the bundled toolchain was not an error: exit $($r.code), $($r.report)" }

  $r = Probe "$inst\kpp.exe" 'msvcdialect' @('--target', 'windows')
  if ($r.code -ne 2 -or $r.report -notmatch 'needs Visual Studio') { Fail "--target windows with the bundled toolchain was not an error: exit $($r.code), $($r.report)" }

  $r = Probe $Kppc 'gnuvs' @('--target', 'windows-gnu')
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
    # Through the compile server: a C library's change alone (the program's Kpp the same, so kppc
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
    # The repository's kppc (no toolchain of its own) without Visual Studio's clang: the same, said for it.
    $r = Probe $Kppc 'novs' @()
    if ($r.code -ne 2 -or $r.report -notmatch 'has no toolchain of its own.*none is installed') { Fail "a kppc without a toolchain and without Visual Studio: exit $($r.code), $($r.report)" }
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

if ($failed -eq 0) { "ok   toolchain: llvm-mingw by default in an installation (windows-gnu, Windows' DLLs alone), msvc on request" }
exit $failed
