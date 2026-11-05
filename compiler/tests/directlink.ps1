# An MSVC link that is no LTO's runs link.exe directly with the clang driver's translation of its
# response file (Driver/ModuleCompile.inl, directLinkCommand): EXE.link.direct holds its key and
# link.exe's command, EXE.link.exe.rsp link.exe's arguments. The program links and runs; the key
# follows the toolchain's environment (a changed PATH asks the driver again) and a stale one is replaced;
# a release build (ThinLTO, lld) keeps going through clang. run_tests.ps1 runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\directlink'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force "$work\out" | Out-Null
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0
function Fail([string]$what) { "FAIL directlink: $what"; $script:failed++ }
[IO.File]::WriteAllText("$work\main.kpp", "int main()`n{`n  Vector<int> v;`n  v.push_back(6);`n  println(`"linked `", v[0] * 7);`n  return 0;`n}`n")
$exe = "$work\out\main.exe"
function Build([string]$flags = '')
{
  $ErrorActionPreference = 'Continue'
  cmd /c "`"$Kppc`" --quiet $flags --modules 4 --exe `"$exe`" `"$work\main.kpp`" > `"$work\build.txt`" 2>&1"
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  $printed = if ($code -eq 0) { (& $exe | Out-String).Trim() } else { '' }
  return [pscustomobject]@{ code = $code; printed = $printed; text = [IO.File]::ReadAllText("$work\build.txt") }
}
function Direct() { if (Test-Path "$exe.link.direct") { return @([IO.File]::ReadAllText("$exe.link.direct") -split "`n") } else { return @() } }

$first = Build
$direct = Direct
if ($first.code -ne 0 -or $first.printed -ne 'linked 42') { Fail "the first build: exit $($first.code), printed '$($first.printed)'`n$($first.text)" }
elseif ($direct.Count -ne 2 -or $direct[0] -notmatch '^[0-9a-f]{64}$' -or $direct[1] -notmatch 'link\.exe" @"' -or -not (Test-Path "$exe.link.exe.rsp"))
{ Fail "no link.exe command kept beside the executable: [$($direct -join ' | ')]" }

# Another toolchain environment (PATH, which a VsDevCmd changes with the rest): the driver is asked
# again, the program links.
$savedPath = $env:PATH
$env:PATH = "$savedPath;$work"
$again = Build
$env:PATH = $savedPath
$changed = Direct
if ($again.code -ne 0 -or $again.printed -ne 'linked 42' -or $changed.Count -ne 2 -or $changed[0] -eq $direct[0])
{ Fail "with PATH changed: exit $($again.code), printed '$($again.printed)', key $(if ($changed.Count) { $changed[0] }) (was $($direct[0]))" }

# A stale translation (another key) is replaced, never run.
[IO.File]::WriteAllText("$exe.link.direct", ('0' * 64) + "`n`"`"no-such-linker.exe`" @`"nothing.rsp`"`"")
$stale = Build
$replaced = Direct
if ($stale.code -ne 0 -or $stale.printed -ne 'linked 42' -or $replaced.Count -ne 2 -or $replaced[0] -ne $direct[0])
{ Fail "a stale EXE.link.direct: exit $($stale.code), printed '$($stale.printed)', key $(if ($replaced.Count) { $replaced[0] })`n$($stale.text)" }

# A release build links through lld (ThinLTO): no translation of its own.
cmd /c rd /s /q "$work\out"
New-Item -ItemType Directory -Force "$work\out" | Out-Null
$release = Build '--release'
if ($release.code -ne 0 -or $release.printed -ne 'linked 42' -or (Test-Path "$exe.link.direct"))
{ Fail "a release build: exit $($release.code), printed '$($release.printed)', $(if (Test-Path "$exe.link.direct") { 'a' } else { 'no' }) link.exe command" }

if ($failed -eq 0)
{
  'ok   directlink'
  cmd /c rd /s /q "$work"
}
exit $failed
