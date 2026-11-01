# --target (docs/design.md, "Targets and hosts"): tests\targets\ built for each target kppc has.
# Windows: built and run here. Linux: built here against a glibc sysroot and run under WSL (made from
# WSL's Ubuntu by tools\sysroot\linux-from-wsl.sh into build\sysroots\linux-x86_64 when missing, or
# KPP_SYSROOT_LINUX); skipped without WSL. FreeBSD: the IR and an object (no sysroot, nothing that
# runs it here). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\targets'
New-Item -ItemType Directory -Force $work | Out-Null
$src = Join-Path $PSScriptRoot 'targets'
$failed = 0
function Fail([string]$what) { "FAIL targets: $what"; $script:failed++ }

# Runs a command: its stdout, its stderr (as written: not PowerShell's error records) and exit code.
function Run([string]$exe, [string[]]$arguments)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $exe
  $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $p = [System.Diagnostics.Process]::Start($info)
  $errTask = $p.StandardError.ReadToEndAsync()
  $out = $p.StandardOutput.ReadToEnd()
  $p.WaitForExit()
  $trim = { param($s) ($s -replace "`r", '').TrimEnd("`n") }
  return [pscustomobject]@{ exit = $p.ExitCode; out = (& $trim $out); err = (& $trim $errTask.Result) }
}

function Build([string[]]$arguments)
{
  $r = Run $Kppc (@('--quiet') + $arguments)
  if ($r.exit -ne 0) { Fail "kppc $($arguments -join ' '): exit $($r.exit): $($r.err) $($r.out)" }
  return $r.exit -eq 0
}

# Windows.
$exe = Join-Path $work 'hello.exe'
if (Build @('--exe', $exe, (Join-Path $src 'hello.kpp')))
{
  $r = Run $exe @('a', 'b')
  if ($r.out -ne 'hello, world! 2 argument(s), sum 30, windows' -or $r.exit -ne 3) { Fail "windows hello: '$($r.out)', exit $($r.exit)" }
}
$exe = Join-Path $work 'imports.exe'
if (Build @('--exe', $exe, (Join-Path $src 'imports.kpp')))
{
  $r = Run $exe @()
  if ($r.out -ne '2147483647 4 2 5 2' -or $r.exit -ne 0) { Fail "windows imports: '$($r.out)', exit $($r.exit)" }
}
$exe = Join-Path $work 'uncaught.exe'
if (Build @('--exe', $exe, (Join-Path $src 'uncaught.kpp')))
{
  $r = Run $exe @()
  if ($r.out -ne 'before' -or $r.err -ne 'unhandled exception of type Boom: it went boom' -or $r.exit -eq 0) { Fail "windows uncaught: '$($r.out)' '$($r.err)', exit $($r.exit)" }
}

# Nothing of Windows' runtime in another target's IR.
function CheckIR([string]$path, [string]$target, [string]$triple)
{
  $ir = [IO.File]::ReadAllText($path)
  if (-not $ir.Contains("target triple = `"$triple`"")) { Fail "$target IR: no triple $triple" }
  foreach ($windowsOnly in '__acrt_iob_func', '__C_specific_handler', 'dllimport', 'catchswitch', 'CodeView', 'wmain')
  {
    if ($ir.Contains($windowsOnly)) { Fail "$target IR ($([IO.Path]::GetFileName($path))) names $windowsOnly" }
  }
}

# FreeBSD: the IR, and clang makes an ELF object for FreeBSD of it.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$clang = if ($env:KPP_CLANG) { $env:KPP_CLANG } else { Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang.exe' }
foreach ($program in 'hello', 'uncaught')
{
  $ll = Join-Path $work "$program.freebsd.ll"
  if (-not (Build @('--target', 'freebsd', '--emit-llvm', $ll, (Join-Path $src "$program.kpp")))) { continue }
  CheckIR $ll 'freebsd' 'x86_64-unknown-freebsd14'
  $object = Join-Path $work "$program.freebsd.o"
  $r = Run $clang @('--target=x86_64-unknown-freebsd14', '-c', $ll, '-o', $object)
  if ($r.exit -ne 0) { Fail "freebsd $program object: clang exit $($r.exit): $($r.err)"; continue }
  $bytes = [IO.File]::ReadAllBytes($object)
  # (an ELF object, OS ABI 9: FreeBSD)
  if ($bytes[0] -ne 0x7f -or $bytes[1] -ne 0x45 -or $bytes[7] -ne 9) { Fail "freebsd $program object: not a FreeBSD ELF object" }
}
if ([IO.File]::ReadAllText((Join-Path $work 'uncaught.freebsd.ll')) -notmatch '@"__stderrp"') { Fail 'freebsd IR: the runtime does not write to __stderrp' }

# Linux, under WSL.
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$hasWsl = $false
if ($wsl)
{
  $ErrorActionPreference = 'Continue'
  & wsl.exe -e true 2> $null
  $hasWsl = $LASTEXITCODE -eq 0
  $ErrorActionPreference = 'Stop'
}
if (-not $hasWsl) { "skip targets: linux (no WSL to run its programs)" }
else
{
  $sysroot = if ($env:KPP_SYSROOT_LINUX) { $env:KPP_SYSROOT_LINUX } else { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\sysroots\linux-x86_64')) }
  if (-not (Test-Path (Join-Path $sysroot 'usr\lib\x86_64-linux-gnu\libc.so.6')))
  {
    $script = (& wsl.exe -e wslpath -a ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\tools\sysroot\linux-from-wsl.sh')))).Trim()
    $target = (& wsl.exe -e wslpath -a $sysroot).Trim()
    $r = Run wsl.exe @('-e', 'sh', $script, $target)
    if ($r.exit -ne 0) { Fail "linux sysroot: $($r.err)" }
  }
  foreach ($configuration in @(@('debug'), @('release', '--release'), @('debug-info', '--debug-info')))
  {
    $name = $configuration[0]
    $elf = Join-Path $work "hello.$name.elf"
    if (-not (Build (@('--target', 'linux', '--sysroot', $sysroot, '--exe', $elf) + $configuration[1..9] + @((Join-Path $src 'hello.kpp'))))) { continue }
    $r = Run wsl.exe @('-e', (& wsl.exe -e wslpath -a $elf).Trim(), 'a', 'b')
    if ($r.out -ne 'hello, world! 2 argument(s), sum 30, posix' -or $r.exit -ne 3) { Fail "linux hello ($name): '$($r.out)', exit $($r.exit) $($r.err)" }
  }
  # The target's own headers (glibc's, from the sysroot): C's long 64 bits.
  $elf = Join-Path $work 'imports.elf'
  if (Build @('--target', 'linux', '--sysroot', $sysroot, '--exe', $elf, (Join-Path $src 'imports.kpp')))
  {
    $r = Run wsl.exe @('-e', (& wsl.exe -e wslpath -a $elf).Trim())
    if ($r.out -ne '9000000000 8 4 5 1' -or $r.exit -ne 0) { Fail "linux imports: '$($r.out)', exit $($r.exit) $($r.err)" }
  }
  # C's long is 64 bits on Linux and 32 on Windows, decided at compile time for each target (Q1): the
  # same static_assert compiles for one and is an error for the other. (C-variadic calls are not
  # format-checked any more, design.md 14: this replaced a check that printf's %ld refused an int32.)
  $sized = Join-Path $work 'long_size.kpp'
  [IO.File]::WriteAllText($sized, "static_assert(sizeof(long) == 8 && sizeof(unsigned long) == 8);`nint main() { return 0; }`n")
  $r = Run $Kppc @('--quiet', '--target', 'linux', '--sysroot', $sysroot, '--exe', (Join-Path $work 'long_size.elf'), $sized)
  if ($r.exit -ne 0) { Fail "linux long is 64 bits: exit $($r.exit), '$($r.err)'" }
  $r = Run $Kppc @('--quiet', '--exe', (Join-Path $work 'long_size.exe'), $sized)
  if ($r.exit -eq 0 -or -not $r.err.Contains('static assertion failed')) { Fail "windows long is 32 bits: exit $($r.exit), '$($r.err)'" }
  $ll = Join-Path $work 'hello.linux.ll'
  if (Build @('--target', 'linux', '--emit-llvm', $ll, (Join-Path $src 'hello.kpp'))) { CheckIR $ll 'linux' 'x86_64-unknown-linux-gnu' }
  $elf = Join-Path $work 'uncaught.elf'
  if (Build @('--target', 'linux', '--sysroot', $sysroot, '--exe', $elf, (Join-Path $src 'uncaught.kpp')))
  {
    $r = Run wsl.exe @('-e', (& wsl.exe -e wslpath -a $elf).Trim())
    $want = 'exception of type Boom thrown: it went boom (exceptions do not unwind on this target yet: docs/design.md, "Targets and hosts", stage 3)'
    if ($r.out -ne 'before' -or -not $r.err.StartsWith($want) -or $r.exit -ne 134) { Fail "linux uncaught: '$($r.out)' '$($r.err)', exit $($r.exit)" }
  }
}

if ($failed) { exit 1 }
"ok   targets (windows, freebsd's IR and object$(if ($hasWsl) { ', linux run under WSL' }))"
