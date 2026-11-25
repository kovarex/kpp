# An MSVC link that is no LTO's runs link.exe directly with the clang driver's translation of its
# response file (Driver/ModuleCompile.cpp, directLinkCommand): EXE.link.direct holds its key, link.exe's
# command and its arguments with a line `<objects>` for the objects, EXE.link.exe.rsp those arguments with
# the build's objects. The program links and runs; the arguments are the driver's for the whole response,
# though it is asked without the objects; the key follows the toolchain's environment (a changed PATH asks
# the driver again) and a stale one is replaced; other objects ask nobody; a release build (ThinLTO, lld)
# keeps going through clang. run_tests.ps1 runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\directlink'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force "$work\out" | Out-Null
$KppExe = (Resolve-Path $KppExe).Path
$failed = 0
function Fail([string]$what) { "FAIL directlink: $what"; $script:failed++ }
[IO.File]::WriteAllText("$work\main.kpp", "int main()`n{`n  std::Vector<int> v;`n  v.push_back(6);`n  std::println(`"linked `", v[0] * 7);`n  return 0;`n}`n")
$exe = "$work\out\main.exe"
function Build([string]$flags = '--quiet --modules 4')
{
  $ErrorActionPreference = 'Continue'
  cmd /c "`"$KppExe`" $flags --exe `"$exe`" `"$work\main.kpp`" > `"$work\build.txt`" 2>&1"
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  $printed = if ($code -eq 0) { (& $exe | Out-String).Trim() } else { '' }
  return [pscustomobject]@{ code = $code; printed = $printed; text = [IO.File]::ReadAllText("$work\build.txt") }
}
function Direct() { if (Test-Path "$exe.link.direct") { return @([IO.File]::ReadAllText("$exe.link.direct") -split "`n") } else { return @() } }
# (what is kept: the key, the command, the arguments with the objects' place)
function Kept($direct) { return $direct.Count -gt 3 -and $direct[0] -match '^[0-9a-f]{64}$' -and $direct[1] -match 'link\.exe" @"' -and $direct -contains '<objects>' }
function Objects() { return @(Get-Content "$exe.link.exe.rsp" | Where-Object { $_ -match 'main\.exe\.\d+\.o"$' }) }

$first = Build '--modules 4' # (not quiet: its toolchain line names the clang)
$direct = Direct
if ($first.code -ne 0 -or $first.printed -ne 'linked 42') { Fail "the first build: exit $($first.code), printed '$($first.printed)'`n$($first.text)" }
elseif (-not (Kept $direct) -or -not (Test-Path "$exe.link.exe.rsp"))
{ Fail "no link.exe command kept beside the executable: [$(($direct | Select-Object -First 3) -join ' | ')]" }
else
{
  # The driver was asked without the objects: link.exe's arguments are what it gives for the whole response.
  $clang = if ($first.text -match '(?m)^toolchain: (.+?) \(') { $Matches[1] } else { '' }
  $ErrorActionPreference = 'Continue'
  $job = @(cmd /c "`"$clang`" -### @`"$exe.link.rsp`" 2>&1" | Where-Object { $_ -like ' "*' })
  $ErrorActionPreference = 'Stop'
  $mine = (' ' + ($direct[1] -split ' @"')[0] + ' ' + ((Get-Content "$exe.link.exe.rsp") -join ' ')).Replace('\', '\\')
  if (-not $clang -or $job.Count -ne 1 -or $job[0].TrimEnd() -ne $mine)
  { Fail "link.exe's arguments are not the driver's for the whole response (clang '$clang'):`n  kept:   $mine`n  driver: $($job -join "`n          ")" }
  if ((Objects).Count -eq 0) { Fail "EXE.link.exe.rsp names no object" }
}

# Another toolchain environment (PATH, which a VsDevCmd changes with the rest): the driver is asked
# again, the program links.
$savedPath = $env:PATH
$env:PATH = "$savedPath;$work"
$again = Build
$env:PATH = $savedPath
$changed = Direct
if ($again.code -ne 0 -or $again.printed -ne 'linked 42' -or -not (Kept $changed) -or $changed[0] -eq $direct[0])
{ Fail "with PATH changed: exit $($again.code), printed '$($again.printed)', key $(if ($changed.Count) { $changed[0] }) (was $($direct[0]))" }

# A stale translation (another key) is replaced, never run.
[IO.File]::WriteAllText("$exe.link.direct", ('0' * 64) + "`n`"`"no-such-linker.exe`" @`"nothing.rsp`"`"`n`"-nologo`"`n<objects>`n")
$stale = Build
$replaced = Direct
if ($stale.code -ne 0 -or $stale.printed -ne 'linked 42' -or -not (Kept $replaced) -or $replaced[0] -ne $direct[0])
{ Fail "a stale EXE.link.direct: exit $($stale.code), printed '$($stale.printed)', key $(if ($replaced.Count) { $replaced[0] })`n$($stale.text)" }

# Other objects (more modules): the answer kept is theirs too, the driver not asked (the file is as it was),
# and link.exe gets the objects there are now.
$before = (Objects).Count
$keptAt = (Get-Item "$exe.link.direct").LastWriteTimeUtc
$more = Build '--quiet --modules 8'
$same = Direct
if ($more.code -ne 0 -or $more.printed -ne 'linked 42' -or -not (Kept $same) -or $same[0] -ne $direct[0] -or (Get-Item "$exe.link.direct").LastWriteTimeUtc -ne $keptAt)
{ Fail "with more modules: exit $($more.code), printed '$($more.printed)', key $(if ($same.Count) { $same[0] }) (was $($direct[0])), the kept answer written $(if ((Get-Item "$exe.link.direct").LastWriteTimeUtc -ne $keptAt) { 'again' } else { 'once' })`n$($more.text)" }
elseif ((Objects).Count -le $before -or (Objects).Count -ne @(Get-Content "$exe.link.rsp" | Where-Object { $_ -match '\.o"$' }).Count)
{ Fail "with more modules: link.exe is given $((Objects).Count) object(s), $before before, $(@(Get-Content "$exe.link.rsp" | Where-Object { $_ -match '\.o"$' }).Count) in the response" }

# A release build links through lld (ThinLTO): no translation of its own.
cmd /c rd /s /q "$work\out"
New-Item -ItemType Directory -Force "$work\out" | Out-Null
$release = Build '--quiet --modules 4 --release'
if ($release.code -ne 0 -or $release.printed -ne 'linked 42' -or (Test-Path "$exe.link.direct"))
{ Fail "a release build: exit $($release.code), printed '$($release.printed)', $(if (Test-Path "$exe.link.direct") { 'a' } else { 'no' }) link.exe command" }

if ($failed -eq 0)
{
  'ok   directlink'
  cmd /c rd /s /q "$work"
}
exit $failed
