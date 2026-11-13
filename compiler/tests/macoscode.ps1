# macOS arm64 code that a Windows host checks without running it (thread O's reports from macOS, 2026-11-13; design.md,
# "Targets and hosts"): built against the stand-in SDK (spec kpp/macos_sdk_checked_before_the_build.sdk), its link run
# with ld64.lld.
# 1. Every access of a thread_local goes through llvm.threadlocal.address, as clang emits it: with the global's own name
#    as the operand, an unoptimized build for macOS arm64 kept its TLV address in a register across calls that clobbered
#    it, and every thread_local access of a debug build crashed (language/thread_local_locals, the test runner's).
# 2. A release build's main, which catches, has compact unwind with its personality and LSDA, and no function is left
#    without unwind information: its tables were DWARF's FDEs (asynchronous tables, no frame pointers, kpp's personality
#    none of Darwin's), which ld64.lld dropped from a ThinLTO build's objects, and no handler of main was found
#    (language/rethrow_from_nested_catch_release: "unhandled exception").
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\macoscode'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$failed = 0
function Fail([string]$what) { "FAIL macos code: $what"; $script:failed++ }
$sdk = (Resolve-Path "$PSScriptRoot\spec\kpp\macos_sdk_checked_before_the_build.sdk").Path
$spec = (Resolve-Path "$PSScriptRoot\spec").Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$tools = Join-Path $vs 'VC\Tools\Llvm\x64\bin' # (llvm-objdump, llvm-nm: Visual Studio's clang's)

# 1. The IR of a program with thread_local locals (and the exception runtime's, kpp.eh.current), every function's use of
#    a thread_local.
$ErrorActionPreference = 'Continue'
$out = & $KppExe --target macos --sysroot $sdk --defer-link --emit-llvm "$work\tls.ll" --exe "$work\tls" "$spec\language\thread_local_locals.kpp" 2>&1 | Out-String
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0 -or -not (Test-Path "$work\tls.ll")) { Fail "language/thread_local_locals did not build for macOS (exit $code): $out" }
else
{
  $text = [IO.File]::ReadAllText("$work\tls.ll")
  $names = @([regex]::Matches($text, '(?m)^(@"[^"]+"|@[-A-Za-z0-9$._]+) = [^\n]*\bthread_local\b') | ForEach-Object { $_.Groups[1].Value })
  if ($names.Count -lt 4) { Fail "the IR names $($names.Count) thread_local globals, the snippet's two and their guards expected: the check proves nothing" }
  $direct = 0
  $inFunction = $false
  foreach ($line in $text -split "`n")
  {
    if ($line.StartsWith('define ')) { $inFunction = $true; continue }
    if ($line.StartsWith('}')) { $inFunction = $false; continue }
    if (-not $inFunction -or $line.Contains('llvm.threadlocal.address')) { continue }
    foreach ($name in $names)
    {
      $at = $line.IndexOf($name)
      if ($at -ge 0 -and ($at + $name.Length -eq $line.Length -or $line[$at + $name.Length] -notmatch '[-A-Za-z0-9$._"]'))
      {
        if (++$direct -le 3) { Fail "a thread_local used without llvm.threadlocal.address: $($line.Trim())" }
      }
    }
  }
}

# 2. A release build's executable: every function's compact unwind entry, main's with an LSDA.
$ErrorActionPreference = 'Continue'
$out = & $KppExe --target macos --sysroot $sdk --release --defer-link --exe "$work\rethrow" "$spec\language\rethrow_from_nested_catch_release.kpp" 2>&1 | Out-String
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0 -or -not (Test-Path "$work\rethrow.link.command")) { Fail "language/rethrow_from_nested_catch_release did not build for macOS (exit $code): $out" }
else
{
  $ErrorActionPreference = 'Continue'
  $linked = cmd /c "$(Get-Content "$work\rethrow.link.command" -Raw) 2>&1" | Out-String
  $ErrorActionPreference = 'Stop'
  if (-not (Test-Path "$work\rethrow")) { Fail "its link did not write a Mach-O executable: $linked" }
  else
  {
    $unwind = & "$tools\llvm-objdump.exe" --macho --unwind-info "$work\rethrow" 2>&1
    $none = @($unwind | Select-String 'encoding\[\d+\]=0x00000000').Count
    if ($none) { Fail "$none function(s) without unwind information in the release build" }
    $main = (& "$tools\llvm-nm.exe" -n --defined-only "$work\rethrow" | Select-String ' _main$')
    if (-not $main) { Fail 'no _main in the release build' }
    else
    {
      $offset = '{0:x8}' -f ([Convert]::ToInt64($main.ToString().Split(' ')[0], 16) - 0x100000000)
      if (-not ($unwind | Select-String "function offset=0x$offset, LSDA")) { Fail "main's compact unwind has no LSDA: its handlers would not be found" }
    }
  }
}

if (Test-Path $work) { cmd /c rd /s /q "$work" }
if ($failed -eq 0) { "ok   macos code: thread_locals through llvm.threadlocal.address, a release build's catching main has compact unwind with its LSDA" }
exit $failed
