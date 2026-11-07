# KPP_OBJECT_CACHE: module objects shared by every output directory (main.cpp, objectcache), keyed by
# the module's text and number, the optimisation level, the flags and the compiler. A program built
# into a second directory takes every module's object from it and runs the same; an edited program
# compiles what changed and runs its new text, never an object of the old one; a release build takes
# none of the debug build's objects; builds that share one cache at once all build and run, and leave
# no file of a writer's half way. run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\objectcache'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force "$work\src" | Out-Null
$KppExe = (Resolve-Path $KppExe).Path
$failed = 0
function Fail([string]$what) { "FAIL objectcache: $what"; $script:failed++ }

[IO.File]::WriteAllText("$work\src\shape.kpp", "struct Shape { int width = 0; int height = 0; };`nint area(const Shape& s) { return s.width * s.height; }`n")
$main = "int main()`n{`n  Shape square;`n  square.width = 3;`n  square.height = 3;`n  Vector<int> sides;`n  sides.push_back(square.width);`n  println(`"area `", area(square), `" sides `", sides.size());`n  return 0;`n}`n"
[IO.File]::WriteAllText("$work\src\main.kpp", $main)

# One build into out\NAME.exe with the cache dir: its exit code, the clang line's module count and how
# many came from the cache (0 when it says none), and what the program printed.
function Build([string]$name, [string]$cache, [string]$flags = '')
{
  New-Item -ItemType Directory -Force "$work\out" | Out-Null
  $env:KPP_OBJECT_CACHE = $cache
  $ErrorActionPreference = 'Continue'
  cmd /c "`"$KppExe`" $flags --modules 4 --exe `"$work\out\$name.exe`" `"$work\src`" > `"$work\$name.txt`" 2>&1"
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  Remove-Item Env:KPP_OBJECT_CACHE
  $text = [IO.File]::ReadAllText("$work\$name.txt")
  $modules = if ($text -match '(\d+) modules on') { [int]$Matches[1] } else { -1 }
  $shared = if ($text -match '(\d+) of them from KPP_OBJECT_CACHE') { [int]$Matches[1] } else { 0 }
  $printed = if ($code -eq 0) { (& "$work\out\$name.exe" | Out-String).Trim() } else { '' }
  return [pscustomobject]@{ code = $code; modules = $modules; shared = $shared; printed = $printed; text = $text }
}

$cache = "$work\cache"
$first = Build 'first' $cache
if ($first.code -ne 0 -or $first.printed -ne 'area 9 sides 1' -or $first.modules -lt 2)
{ Fail "the first build: exit $($first.code), $($first.modules) modules, printed '$($first.printed)'`n$($first.text)" }
# (a build may take the objects of its own modules alike: modules with nothing in them)
$second = Build 'second' $cache
if ($second.code -ne 0 -or $second.printed -ne 'area 9 sides 1' -or $second.shared -ne $second.modules)
{ Fail "a second output directory: exit $($second.code), $($second.shared) of $($second.modules) modules from the cache, printed '$($second.printed)'`n$($second.text)" }

[IO.File]::WriteAllText("$work\src\main.kpp", $main.Replace('square.height = 3;', 'square.height = 5;'))
$edited = Build 'edited' $cache
if ($edited.code -ne 0 -or $edited.printed -ne 'area 15 sides 1' -or $edited.shared -ge $edited.modules)
{ Fail "the program edited: exit $($edited.code), $($edited.shared) of $($edited.modules) modules from the cache, printed '$($edited.printed)' ('area 15 sides 1' expected)`n$($edited.text)" }

$release = Build 'release' $cache '--release'
if ($release.code -ne 0 -or $release.printed -ne 'area 15 sides 1' -or $release.shared -ne 0)
{ Fail "a release build after the debug ones: exit $($release.code), $($release.shared) modules from the cache (none expected), printed '$($release.printed)'`n$($release.text)" }
$release2 = Build 'release2' $cache '--release'
if ($release2.code -ne 0 -or $release2.printed -ne 'area 15 sides 1' -or $release2.shared -ne $release2.modules)
{ Fail "a second release build: exit $($release2.code), $($release2.shared) of $($release2.modules) modules from the cache, printed '$($release2.printed)'`n$($release2.text)" }

# Six builds at once into six directories, sharing a cache none of them found anything in.
$together = "$work\together"
New-Item -ItemType Directory -Force "$work\out" | Out-Null
$env:KPP_OBJECT_CACHE = $together
$running = foreach ($i in 1..6)
{
  Start-Process -FilePath $KppExe -ArgumentList '--quiet', '--modules', '4', '--exe', "`"$work\out\together$i.exe`"", "`"$work\src`"" -PassThru -NoNewWindow `
    -RedirectStandardOutput "$work\together$i.txt" -RedirectStandardError "$work\together$i.err.txt"
}
Remove-Item Env:KPP_OBJECT_CACHE
foreach ($p in $running) { $null = $p.Handle }
$i = 0
foreach ($p in $running)
{
  ++$i
  $p.WaitForExit()
  $printed = if ($p.ExitCode -eq 0) { (& "$work\out\together$i.exe" | Out-String).Trim() } else { '' }
  if ($p.ExitCode -ne 0 -or $printed -ne 'area 15 sides 1')
  { Fail "build $i of six at once: exit $($p.ExitCode), printed '$printed'`n$([IO.File]::ReadAllText("$work\together$i.err.txt"))" }
}
$parts = @(Get-ChildItem $together -Filter '*.part' -ErrorAction SilentlyContinue)
if ($parts.Count -ne 0) { Fail "six builds at once left $($parts.Count) files half written in the cache" }

if ($failed -eq 0)
{
  'ok   objectcache'
  cmd /c rd /s /q "$work"
}
exit $failed
