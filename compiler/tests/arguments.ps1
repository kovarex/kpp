# main's arguments on Windows (design.md, "Targets and hosts"): UTF-8, from the C runtime's wide argv
# through the generated wmain (codegen's emitEntry), for the MSVC target and windows-gnu; an argument
# holding a lone surrogate, which has no UTF-8 form, ends the program before main with a message that
# names it (kovarex, 2026-11-06: a path holding one throws), and kppc --run refuses to pass one on. A
# surrogate pair is one character, four bytes. The UTF-8 of ordinary text is the spec's
# (interop/main_arguments_are_utf8): a snippet cannot hold a lone surrogate. run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\arguments'))
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0
function Fail([string]$what) { "FAIL arguments: $what"; $script:failed++ }

if (Test-Path $work) { cmd /c rd /s /q $work }
New-Item -ItemType Directory -Force $work | Out-Null
$source = Join-Path $work 'bytes.kpp'
Set-Content -Encoding ascii $source @'
extern "C" import "stdio.h";
int main(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i)
  {
    if (i > 1)
      printf(" ");
    for (const char* p = argv[i]; *p; ++p)
      printf("%02x", (unsigned char)*p);
  }
  printf("\n");
  return 0;
}
'@

# A process of the arguments (each quoted, none holding a quote or a backslash), its output, errors
# and exit code.
function RunWith([string]$program, [string[]]$arguments)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo $program
  $info.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.WorkingDirectory = $work
  $p = [System.Diagnostics.Process]::Start($info)
  $out = $p.StandardOutput.ReadToEndAsync()
  $err = $p.StandardError.ReadToEnd()
  $p.WaitForExit()
  return @{ out = $out.Result.Trim(); err = $err.Trim(); exit = $p.ExitCode }
}

$lone = 'a' + [char]0xD800 + 'b'
$pair = 'x' + [char]0xD83D + [char]0xDE00 # U+1F600
$mingw = (& "$PSScriptRoot\..\..\tools\sysroot\llvm-mingw.ps1" | Select-Object -Last 1)
foreach ($target in 'windows', 'windows-gnu')
{
  $exe = Join-Path $work "bytes-$target.exe"
  if ($target -eq 'windows-gnu') { $env:KPP_CLANG = "$mingw\bin\clang.exe" }
  $ErrorActionPreference = 'Continue'
  & $Kppc --quiet --target $target --exe $exe $source > "$exe.build.txt" 2>&1
  $built = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  Remove-Item Env:\KPP_CLANG -ErrorAction SilentlyContinue
  if ($built -ne 0) { Fail "$target build (exit $built): $exe.build.txt"; continue }
  $r = RunWith $exe @('plain', $pair)
  if ($r.out -ne '706c61696e 78f09f9880' -or $r.exit -ne 0) { Fail "$target, a surrogate pair: '$($r.out)' exit $($r.exit) $($r.err)" }
  $r = RunWith $exe @('ok', $lone)
  if ($r.out -ne '' -or $r.exit -eq 0 -or $r.err -notmatch "argument 2 holds a lone surrogate, which has no UTF-8 form") { Fail "$target, a lone surrogate: '$($r.out)' exit $($r.exit) '$($r.err)'" }
}

# kppc --run: the program's arguments as UTF-8 (kppc's own argv is the ANSI code page's), a lone
# surrogate refused.
$r = RunWith $Kppc @('--quiet', '--run', $source, ([string][char]0x17E), $pair)
if ($r.out -ne 'c5be 78f09f9880' -or $r.exit -ne 0) { Fail "kppc --run, ordinary text and a surrogate pair: '$($r.out)' exit $($r.exit) $($r.err)" }
$r = RunWith $Kppc @('--quiet', '--run', $source, 'ok', $lone)
if ($r.exit -ne 2 -or $r.err -notmatch "kppc --run: the program's argument 2 holds a lone surrogate") { Fail "kppc --run, a lone surrogate: '$($r.out)' exit $($r.exit) '$($r.err)'" }

if ($failed -eq 0) { "ok   arguments: UTF-8 main arguments on windows and windows-gnu, a lone surrogate refused, kppc --run's passed on" }
exit $failed
