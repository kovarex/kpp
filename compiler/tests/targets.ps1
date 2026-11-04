# --target (docs/design.md, "Targets and hosts"): tests\targets\ built for each target kppc has.
# Windows: built and run here. Linux: built and linked here against Debian 11's glibc 2.31 sysroot
# (tools\sysroot\linux.ps1 into build\sysroots\linux-x86_64, made again whenever its stamp is not
# linux.ps1's, a copy of WSL's Ubuntu that linux-from-wsl.sh made included; or KPP_SYSROOT_LINUX), and
# run under WSL; without WSL nothing runs. A failed download or a wrong SHA-256 fails the check.
# FreeBSD: the IR and an object, and with its sysroot a static executable whose ELF headers are
# checked (nothing runs it here). run_tests.cmd runs it.
# Every call of wsl.exe has a timeout: a WSL that does not answer the first in 30 s runs no Linux
# program, and one that stops answering later fails the check; neither hangs it. Exit 0: passed;
# 1: failed; 3: passed, Linux not run (no WSL); 4: passed, Linux not run (WSL did not answer in 30 s).
# run_tests.cmd lists a skip in its summary.
# -WslExe: what runs as wsl.exe (a stub that hangs tests the timeout). -NoLinux: Linux's programs not run
# (exit 5), for run_tests' quick run: under other threads' load a program's run under WSL timed out
# (2026-11-05), and the full run (thread H's batch on master) has it. They are still built and linked.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [string]$WslExe = 'wsl.exe', [switch]$NoLinux)
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\targets'
New-Item -ItemType Directory -Force $work | Out-Null
$src = Join-Path $PSScriptRoot 'targets'
$failed = 0
function Fail([string]$what) { "FAIL targets: $what"; $script:failed++ }

# Runs a command: its stdout, its stderr (as written: not PowerShell's error records) and exit code.
# With $seconds, a command still running after that long is killed, and timedOut says so.
function Run([string]$exe, [string[]]$arguments, [int]$seconds = 0)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $exe
  $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $p = [System.Diagnostics.Process]::Start($info)
  $errTask = $p.StandardError.ReadToEndAsync()
  $outTask = $p.StandardOutput.ReadToEndAsync()
  $timedOut = $false
  if ($seconds -gt 0 -and -not $p.WaitForExit($seconds * 1000))
  {
    $timedOut = $true
    # (its whole tree: a child left running would hold this script's own output open)
    & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
    try { $p.Kill() } catch {}
  }
  $p.WaitForExit()
  # (a killed command's output so far: a child that holds its pipes open is not waited for)
  $null = [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($outTask, $errTask), 5000)
  $trim = { param($t) if ($t.IsCompleted) { ($t.Result -replace "`r", '').TrimEnd("`n") } else { '' } }
  return [pscustomobject]@{ exit = $p.ExitCode; out = (& $trim $outTask); err = (& $trim $errTask); timedOut = $timedOut }
}

function Build([string[]]$arguments)
{
  $r = Run $Kppc (@('--quiet') + $arguments)
  # (Fail's line goes to the host: in the function's output it would make a failed build's result true.)
  if ($r.exit -ne 0) { Fail "kppc $($arguments -join ' '): exit $($r.exit): $($r.err) $($r.out)" | Write-Host }
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
  $r = Run $exe @('large')
  if ($r.out -ne 'before' -or $r.err -ne 'unhandled exception of type Boom: it went boom, with a message too long for the object' -or $r.exit -eq 0) { Fail "windows uncaught, a heap message: '$($r.out)' '$($r.err)', exit $($r.exit)" }
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

# FreeBSD, linked against 14.3's base system (tools\sysroot\freebsd.ps1 makes it into
# build\sysroots\freebsd-x86_64, or KPP_SYSROOT_FREEBSD; never downloaded from here). Nothing runs it
# on this machine, so the executable itself is checked: a static x86-64 ELF executable for FreeBSD
# (OS ABI 9), with no interpreter and no dynamic section, and the ABI note FreeBSD's kernel reads
# naming 14.3 (__FreeBSD_version 1403xxx, from the sysroot's crt1.o).
function ElfProblems([string]$path, [int]$osRelease)
{
  $b = [IO.File]::ReadAllBytes($path)
  if ($b.Length -lt 64 -or $b[0] -ne 0x7f -or $b[1] -ne 0x45 -or $b[2] -ne 0x4c -or $b[3] -ne 0x46) { return 'not an ELF file' }
  $problems = @()
  if ($b[4] -ne 2 -or $b[5] -ne 1) { $problems += 'not 64-bit little-endian' }
  if ($b[7] -ne 9) { $problems += "OS ABI $($b[7]), not 9 (FreeBSD)" }
  if ([BitConverter]::ToUInt16($b, 0x10) -ne 2) { $problems += "type $([BitConverter]::ToUInt16($b, 0x10)), not an executable (ET_EXEC)" }
  if ([BitConverter]::ToUInt16($b, 0x12) -ne 62) { $problems += 'not x86-64' }
  $phoff = [BitConverter]::ToUInt64($b, 0x20)
  $phentsize = [BitConverter]::ToUInt16($b, 0x36)
  $note = $null
  for ($i = 0; $i -lt [BitConverter]::ToUInt16($b, 0x38); $i++)
  {
    $ph = [int]($phoff + $i * $phentsize)
    switch ([BitConverter]::ToUInt32($b, $ph))
    {
      2 { $problems += 'a dynamic section (not static)' }
      3 { $problems += 'an interpreter (not static)' }
      4 {
        # Notes: namesz, descsz, type, the name and the description, each padded to 4 bytes.
        $at = [int][BitConverter]::ToUInt64($b, $ph + 8)
        $end = $at + [int][BitConverter]::ToUInt64($b, $ph + 0x20)
        while ($at + 12 -le $end)
        {
          $namesz = [BitConverter]::ToInt32($b, $at); $descsz = [BitConverter]::ToInt32($b, $at + 4); $type = [BitConverter]::ToInt32($b, $at + 8)
          $name = [Text.Encoding]::ASCII.GetString($b, $at + 12, [Math]::Max(0, $namesz - 1))
          $desc = $at + 12 + (($namesz + 3) -band -4)
          if ($name -eq 'FreeBSD' -and $type -eq 1 -and $descsz -eq 4) { $note = [BitConverter]::ToInt32($b, $desc) }
          $at = $desc + (($descsz + 3) -band -4)
        }
      }
    }
  }
  if ($null -eq $note) { $problems += 'no FreeBSD ABI note (NT_FREEBSD_ABI_TAG)' }
  elseif ([Math]::Floor($note / 1000) -ne $osRelease) { $problems += "ABI note $note, not $($osRelease)xxx" }
  return ($problems -join '; ')
}
$freebsdSysroot = if ($env:KPP_SYSROOT_FREEBSD) { $env:KPP_SYSROOT_FREEBSD } else { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\sysroots\freebsd-x86_64')) }
$hasFreeBSD = Test-Path (Join-Path $freebsdSysroot 'usr\lib\libc.a')
if (-not $hasFreeBSD) { "skip targets: freebsd's link (no sysroot: tools\sysroot\freebsd.ps1)" }
else
{
  foreach ($configuration in @(@('debug'), @('release', '--release'), @('debug-info', '--debug-info')))
  {
    $name = $configuration[0]
    $elf = Join-Path $work "hello.freebsd.$name.elf"
    if (-not (Build (@('--target', 'freebsd', '--sysroot', $freebsdSysroot, '--exe', $elf) + $configuration[1..9] + @((Join-Path $src 'hello.kpp'))))) { continue }
    $problems = ElfProblems $elf 1403
    if ($problems) { Fail "freebsd hello ($name): $problems" }
  }
  # Without --sysroot (2026-11-04): the sysroot that comes with kppc, sysroots\freebsd-x86_64 beside
  # it (a release brings it; compiler\build\sysroots in this repository), when $Kppc has one.
  $beside = Join-Path (Split-Path $Kppc) 'sysroots\freebsd-x86_64'
  if (Test-Path (Join-Path $beside 'usr\lib\libc.a'))
  {
    $elf = Join-Path $work 'hello.freebsd.beside.elf'
    if (Build @('--target', 'freebsd', '--exe', $elf, (Join-Path $src 'hello.kpp')))
    {
      $problems = ElfProblems $elf 1403
      if ($problems) { Fail "freebsd hello without --sysroot: $problems" }
    }
  }
  # A kppc with no sysroot beside it: a link for freebsd without --sysroot is an error naming where it
  # looked, never one against no headers and libraries.
  $alone = [IO.Path]::GetFullPath((Join-Path $work 'kppc-alone'))
  New-Item -ItemType Directory -Force $alone | Out-Null
  Copy-Item $Kppc "$alone\kppc.exe" -Force
  $stdlib = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\stdlib'))
  $r = Run "$alone\kppc.exe" @('--quiet', '--std', $stdlib, '--target', 'freebsd', '--exe', (Join-Path $work 'hello.freebsd.none.elf'), (Join-Path $src 'hello.kpp'))
  if ($r.exit -ne 2 -or "$($r.err)" -notmatch [regex]::Escape("$alone\sysroots\freebsd-x86_64")) { Fail "freebsd without --sysroot and none beside kppc: exit $($r.exit): $($r.err) $($r.out)" }
  # The libraries kpp.net's programs use (their FreeBSD side: BSD sockets, environ), with FreeBSD's
  # own headers.
  $libraries = Join-Path $PSScriptRoot '..\..\libraries'
  $roots = @('net', 'url', 'http', 'mariadb') | ForEach-Object { '--library-root', $_, (Join-Path $libraries "$_\src") }
  $elf = Join-Path $work 'cgi.freebsd.elf'
  if (Build (@('--target', 'freebsd', '--sysroot', $freebsdSysroot, '--exe', $elf) + $roots + @((Join-Path $src 'cgi.kpp'))))
  {
    $problems = ElfProblems $elf 1403
    if ($problems) { Fail "freebsd cgi: $problems" }
  }
  # A kpp project built for FreeBSD, in debug and release: kpp's build line names that toolchain, clang
  # with ld.lld and the sysroot (it said "msvc toolchain"), and the debug build links whole (kpp passed
  # it --incremental-link, link.exe's, which kppc refuses for FreeBSD).
  $project = Join-Path $work 'freebsdproject'
  if (Test-Path "$project\build") { cmd /c rd /s /q "$project\build" }
  New-Item -ItemType Directory -Force "$project\src" | Out-Null
  [IO.File]::WriteAllText((Join-Path $project 'build.kpp'), "void build(Project& project)`n{`n  project.executable(`"hello`", {`"src`"});`n}`n")
  [IO.File]::WriteAllText((Join-Path $project 'src\main.kpp'), "int main()`n{`n  println(`"hello`");`n  return 0;`n}`n")
  $kpp = (Resolve-Path $Kppc).Path # (run_tests passes it relative to compiler\)
  Push-Location $project
  try
  {
    foreach ($configuration in 'debug', 'release')
    {
      $ErrorActionPreference = 'Continue'
      try { $output = @(& $kpp $configuration --no-server --target freebsd --sysroot $freebsdSysroot 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE }
      catch { $output = @("$_"); $code = -1 }
      $ErrorActionPreference = 'Stop'
      $line = @($output | Where-Object { $_ -like 'kpp: built *' }) -join ''
      if ($code -ne 0 -or $line -notmatch "\($configuration, freebsd: (Visual Studio's|the bundled|KPP_CLANG's) clang and ld\.lld, total") { Fail "a kpp project built for freebsd ($configuration): exit $code, its build line '$line': $($output -join ' ')" }
      # Its executable is build\CONFIGURATION\hello, without .exe (it was hello.exe).
      elseif (Test-Path "build\$configuration\hello.exe") { Fail "a kpp project built for freebsd ($configuration) named its executable hello.exe" }
      elseif (-not (Test-Path "build/$configuration/hello")) { Fail "a kpp project built for freebsd ($configuration) wrote no build/$configuration/hello" }
      elseif ($problems = ElfProblems (Join-Path $project "build\$configuration\hello") 1403) { Fail "a kpp project built for freebsd ($configuration): build\$configuration\hello: $problems" }
    }
  }
  finally { Pop-Location }
  # kppc --run --no-run likewise names a FreeBSD program without .exe.
  $ErrorActionPreference = 'Continue'
  $output = @(& $Kppc --no-server --target freebsd --sysroot $freebsdSysroot --no-run --run (Join-Path $src 'hello.kpp') 2>&1 | ForEach-Object { "$_" })
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  $built = if (($output -join "`n") -match 'kppc --run: built (.*) \(not run: --no-run\)') { $Matches[1] } else { '' }
  if ($code -ne 0 -or [IO.Path]::GetFileName($built) -ne 'hello') { Fail "kppc --target freebsd --no-run --run: exit $code, built '$built', not ...\hello: $($output -join ' ')" }
  elseif (-not (Test-Path $built)) { Fail "kppc --target freebsd --no-run --run: no $built" }
  elseif ($problems = ElfProblems $built 1403) { Fail "kppc --target freebsd --no-run --run: $built`: $problems" }
}

# Linux: built here, run under WSL. Each call of wsl.exe has a timeout (a hung WSL once blocked every
# run_tests): 30 s for the first, which also starts WSL's virtual machine, then 120 s for a program.
$wslSkip = $null
if ($NoLinux) { $wslSkip = 'left to the full run' }
elseif (-not (Get-Command $WslExe -ErrorAction SilentlyContinue)) { $wslSkip = 'no WSL' }
else
{
  $r = Run $WslExe @('-e', 'true') 30
  if ($r.timedOut) { $wslSkip = 'WSL did not answer in 30 s' }
  elseif ($r.exit -ne 0) { $wslSkip = 'no WSL' }
}
$hasWsl = -not $wslSkip
# A command under WSL (Run's result); one that does not finish in time fails the check, and after
# that no command runs under WSL (each would wait its own timeout).
$wslHung = $false
# Without WSL: nothing ($null), and the program is not run.
function Wsl([string]$what, [string[]]$arguments, [int]$seconds = 120)
{
  if (-not $hasWsl) { return $null }
  if ($script:wslHung) { return [pscustomobject]@{ exit = -1; out = ''; err = ''; timedOut = $true } }
  $r = Run $WslExe (@('-e') + $arguments) $seconds
  if ($r.timedOut)
  {
    $script:wslHung = $true
    Fail "linux $what`: WSL did not answer in $seconds s ($WslExe -e $($arguments -join ' ')); nothing more runs under WSL" | Write-Host
  }
  return $r
}
# A Windows path as WSL names it (/mnt/c/...).
function WslPath([string]$path) { if (-not $hasWsl) { return '' } (Wsl "wslpath $path" @('wslpath', '-a', $path) 30).out.Trim() }
if (-not $hasWsl) { "targets: linux built and linked, not run ($wslSkip)" }
# (the sysroot: linux.ps1's, which returns at once when its stamp is its own and makes it again
# otherwise; KPP_SYSROOT_LINUX's as it is)
$sysroot = if ($env:KPP_SYSROOT_LINUX) { $env:KPP_SYSROOT_LINUX } else { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\sysroots\linux-x86_64')) }
$sysrootMade = $true
if (-not $env:KPP_SYSROOT_LINUX)
{
  $made = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\..\tools\sysroot\linux.ps1') -Out $sysroot 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { $sysrootMade = $false; Fail "linux sysroot (tools\sysroot\linux.ps1): $($made.Trim())" }
}
elseif (-not (Test-Path (Join-Path $sysroot 'usr\lib\x86_64-linux-gnu\libc.so.6')))
{
  $sysrootMade = $false
  Fail "linux sysroot: KPP_SYSROOT_LINUX ($sysroot) has no usr\lib\x86_64-linux-gnu\libc.so.6"
}
if ($sysrootMade)
{
  foreach ($configuration in @(@('debug'), @('release', '--release'), @('debug-info', '--debug-info')))
  {
    $name = $configuration[0]
    $elf = Join-Path $work "hello.$name.elf"
    if (-not (Build (@('--target', 'linux', '--sysroot', $sysroot, '--exe', $elf) + $configuration[1..9] + @((Join-Path $src 'hello.kpp'))))) { continue }
    $r = Wsl "hello ($name)" @((WslPath $elf), 'a', 'b')
    if (-not $r -or $r.timedOut) { continue }
    if ($r.out -ne 'hello, world! 2 argument(s), sum 30, posix' -or $r.exit -ne 3) { Fail "linux hello ($name): '$($r.out)', exit $($r.exit) $($r.err)" }
  }
  # The target's own headers (glibc's, from the sysroot): C's long 64 bits.
  $elf = Join-Path $work 'imports.elf'
  if (Build @('--target', 'linux', '--sysroot', $sysroot, '--exe', $elf, (Join-Path $src 'imports.kpp')))
  {
    $r = Wsl 'imports' @(WslPath $elf)
    if ($r -and -not $r.timedOut -and ($r.out -ne '9000000000 8 4 5 1' -or $r.exit -ne 0)) { Fail "linux imports: '$($r.out)', exit $($r.exit) $($r.err)" }
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
    $r = Wsl 'uncaught' @(WslPath $elf)
    # (the search finds no handler: reported from the throw site, then abort's SIGABRT, 134)
    if ($r -and -not $r.timedOut -and ($r.out -ne 'before' -or $r.err -ne 'unhandled exception of type Boom: it went boom' -or $r.exit -ne 134)) { Fail "linux uncaught: '$($r.out)' '$($r.err)', exit $($r.exit)" }
    $r = Wsl 'uncaught, a heap message' @((WslPath $elf), 'large')
    if ($r -and -not $r.timedOut -and ($r.out -ne 'before' -or $r.err -ne 'unhandled exception of type Boom: it went boom, with a message too long for the object' -or $r.exit -ne 134)) { Fail "linux uncaught, a heap message: '$($r.out)' '$($r.err)', exit $($r.exit)" }
  }
}

if ($failed) { exit 1 }
"ok   targets (windows, freebsd's IR and object$(if ($hasFreeBSD) { ', freebsd linked statically' }), linux linked against glibc 2.31$(if ($hasWsl) { ' and run under WSL' } else { ", not run: $wslSkip" }))"
if ($NoLinux) { exit 5 }
if ($wslSkip -eq 'no WSL') { exit 3 }
if ($wslSkip) { exit 4 }
