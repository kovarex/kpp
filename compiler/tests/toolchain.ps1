# --toolchain (docs/build.md, "The toolchain"; design.md 17, stage 1): an installation's own clang and
# lld-link (bin\ next to kpp.exe) are its default, Visual Studio's are --toolchain msvc. Here an
# installation made of this kppc and copies of Visual Studio's clang, lld-link and clang's headers
# (what a release's bin\ and lib\clang\ hold, from LLVM 19.1.7). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\toolchain'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$failed = 0
function Fail([string]$what) { "FAIL toolchain: $what"; $script:failed++ }
Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
$llvm = Join-Path $vs 'VC\Tools\Llvm\x64'
$inst = Join-Path $work 'kpp'
New-Item -ItemType Directory -Force "$inst\bin", "$inst\lib\clang" | Out-Null
Copy-Item $Kppc "$inst\kpp.exe"
Copy-Item -Recurse "$PSScriptRoot\..\..\stdlib" "$inst\stdlib"
Copy-Item -Recurse "$PSScriptRoot\..\..\projectlib" "$inst\projectlib"
foreach ($b in 'clang', 'lld-link') { Copy-Item "$llvm\bin\$b.exe" "$inst\bin\" }
$resource = Get-ChildItem "$llvm\lib\clang" | Select-Object -First 1
New-Item -ItemType Directory -Force "$inst\lib\clang\$($resource.Name)" | Out-Null
Copy-Item -Recurse "$($resource.FullName)\include" "$inst\lib\clang\$($resource.Name)\include"

Set-Content -Encoding ascii "$work\probe.kpp" @'
extern "C" import "string.h";
int main()
{
  println("toolchain ", strlen("forty-two"));
  return 0;
}
'@

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

# Builds probe.kpp with `kpp` and runs it: the report's toolchain line, the output, the exit codes.
function Probe([string]$kpp, [string]$label, [string[]]$more)
{
  $exe = "$work\$label.exe"
  $build = Run $kpp (@('--std', "$inst\stdlib") + $more + @('--exe', $exe, "$work\probe.kpp"))
  $out = if ($build.exit -eq 0 -and (Test-Path $exe)) { (Run $exe @()).text.Trim() } else { '' }
  $line = ($build.text -split "`n" | Where-Object { $_ -like 'toolchain:*' } | Select-Object -First 1)
  return [pscustomobject]@{ code = $build.exit; out = $out; line = "$line".Trim(); report = $build.text }
}

$r = Probe "$inst\kpp.exe" 'default' @()
if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "the installation's default did not build: $($r.report)" }
elseif ($r.line -notlike "toolchain: $inst\bin\clang.exe (bundled*") { Fail "the installation's default is not its bin\: '$($r.line)'" }

$r = Probe "$inst\kpp.exe" 'msvc' @('--toolchain', 'msvc')
if ($r.code -ne 0 -or $r.out -ne 'toolchain 9') { Fail "--toolchain msvc did not build: $($r.report)" }
elseif ($r.line -notlike "toolchain: $llvm\bin\clang.exe (Visual Studio's*") { Fail "--toolchain msvc is not Visual Studio's clang: '$($r.line)'" }

$r = Probe $Kppc 'none' @('--toolchain', 'bundled')
if ($r.code -ne 2 -or $r.report -notmatch '--toolchain bundled: this installation has no') { Fail "--toolchain bundled without bin\ was not an error: exit $($r.code), $($r.report)" }

$r = Probe "$inst\kpp.exe" 'incremental' @('--incremental-link')
if ($r.code -ne 2 -or $r.report -notmatch 'add --toolchain msvc') { Fail "--incremental-link with the bundled toolchain was not an error: exit $($r.code), $($r.report)" }

# A project: its debug build links with lld-link, whole (no --incremental-link).
$project = Join-Path $work 'hello'
New-Item -ItemType Directory -Force $project | Out-Null
Push-Location $project
try
{
  $null = Run "$inst\kpp.exe" @('new')
  $run = Run "$inst\kpp.exe" @('--no-server', '--run')
  if ($run.exit -ne 0 -or $run.text -notmatch 'Hello from Kpp') { Fail "kpp new, kpp --run did not run: $($run.text)" }
  $rsp = Get-ChildItem -Recurse "$project\build" -Filter *.link.rsp | Select-Object -First 1
  if (-not $rsp) { Fail "the project's build left no link response file" }
  else
  {
    $text = Get-Content $rsp.FullName -Raw
    if ($text -notmatch '-fuse-ld=lld' -or $text -match 'INCREMENTAL') { Fail "the project's debug link is not lld-link's whole link: $text" }
  }
}
finally { Pop-Location }

if ($failed -eq 0) { "ok   toolchain: bundled by default in an installation, msvc on request" }
exit $failed
