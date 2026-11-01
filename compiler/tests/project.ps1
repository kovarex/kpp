# A project with a build.kpp (docs/build.md, "build.kpp"; src/project.cpp, projectlib/Project.kpp).
# `kpp new` makes one and `kpp --run` prints its greeting. Then a build.kpp with two executables from
# one src\ (the second leaves out the first's main and has its own in tests\), a --set setting
# reaching a program through a C include directory, and the release build. A build.kpp that does not
# compile, and one whose build() calls C, fail with an error. The executables are built through
# the compile server once (stopped with kpp --stop-server) and otherwise with --no-server.
# run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\project'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0
function Fail([string]$what) { Write-Output "FAIL project: $what"; $script:failed++ }
function Kpp([string[]]$arguments)
{
  $ErrorActionPreference = 'Continue'
  $output = & $Kppc @arguments 2>&1 | ForEach-Object { "$_" }
  $script:code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  return $output
}

Push-Location $work
try
{
  Kpp @('new') | Out-Null
  $output = Kpp @('--run')
  if ($code -ne 0 -or ($output -join "`n") -notmatch 'Hello from Kpp') { Fail "kpp new, then kpp --run: $output" }
  Kpp @('--stop-server') | Out-Null

  New-Item -ItemType Directory -Force 'tests', 'include' | Out-Null
  Set-Content 'src\main.kpp' "int main()`n{`n  println(`"game {}`", answer());`n  return 0;`n}"
  Set-Content 'src\answer.kpp' "int answer() { return GREETING_NUMBER; }"
  Set-Content 'tests\tests.kpp' "int main()`n{`n  println(`"tests {}`", plus_one(answer()));`n  return 0;`n}"
  Set-Content 'include\greeting.h' "#define GREETING_NUMBER 41`nint plus_one(int value);"
  # A C library built from its sources: plus.c, and broken.c left out by exclude.
  New-Item -ItemType Directory -Force 'libs\plus' | Out-Null
  Set-Content 'libs\plus\plus.c' "#include `"greeting.h`"`nint plus_one(int value) { return value + PLUS; }"
  Set-Content 'libs\plus\broken.c' "this is no C"
  Set-Content 'build.kpp' @'
void build(Project& project)
{
  project.cInclude(project.setting("INCLUDE_ROOT", "missing") + "/include");
  CLibrary& plus = project.cLibrary("plus");
  plus.source("libs/plus");
  plus.exclude("libs/plus/broken.c");
  plus.include("include");
  plus.define("PLUS=1");
  project.executable("game", {"src"});
  Executable& tests = project.executable("game-tests", {"src", "tests"});
  tests.exclude("src/main.kpp");
  tests.link("plus");
  tests.systemLibrary("user32"); // a link file before the C include directories in the description
  tests.cInclude("include");
}
'@
  Set-Content 'src\greeting.kpp' 'extern "C" import "greeting.h";'
  $root = (Get-Location).Path
  foreach ($configuration in '--debug', '--release')
  {
    $output = Kpp @($configuration, '--no-server', '--set', "INCLUDE_ROOT=$root")
    if ($code -ne 0) { Fail "kpp $configuration`n$($output -join "`n")"; continue }
    $directory = $configuration.Substring(2)
    $game = & ".\build\$directory\game.exe"
    $tests = & ".\build\$directory\game-tests.exe"
    if ("$game" -ne 'game 41' -or "$tests" -ne 'tests 42') { Fail "$configuration printed '$game' and '$tests'" }
  }

  $good = Get-Content 'build.kpp' -Raw
  Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"x`", {undefinedName});`n}"
  $output = Kpp @('--no-server')
  if ($code -eq 0 -or ($output -join "`n") -notmatch "build\.kpp:3: error: ") { Fail "a build.kpp with an error: exit $code`n$($output -join "`n")" }
  Set-Content 'build.kpp' "extern `"C`" int puts(const char*);`nvoid build(Project& project)`n{`n  puts(`"io`");`n}"
  $output = Kpp @('--no-server')
  if ($code -eq 0 -or ($output -join "`n") -notmatch 'puts, a C function, which has no body to run at compile time') { Fail "a build() calling C: exit $code`n$($output -join "`n")" }
  Set-Content 'build.kpp' $good
}
finally { Pop-Location }

# Kpp libraries (docs/build.md, "Kpp libraries"): app uses greet from its libraries\, greet uses text
# next to it there; kpp --test in greet runs its tests and its test roots' with the runner's
# arguments; a minimum version above greet's, a missing library and a minimum kpp above this one
# are errors.
$app = Join-Path $work 'app'
New-Item -ItemType Directory -Force "$app\src", "$app\libraries\greet\src", "$app\libraries\greet\tests", "$app\libraries\text\src" | Out-Null
Push-Location $app
try
{
  Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"greet`", `"0.2`");`n}"
  Set-Content 'src\main.kpp' "int main()`n{`n  println(`"{}`", greeting(`"app`"));`n  return 0;`n}"
  $greet = @'
void build(Project& project)
{
  Library& greet = project.library("greet");
  greet.version = "0.2.1";
  greet.license = "MIT";
  greet.minimumKpp = "0.1.0";
  greet.test("tests");
  greet.use("text");
}
'@
  Set-Content 'libraries\greet\build.kpp' $greet
  Set-Content 'libraries\greet\src\greet.kpp' "String greeting(StringView name) { return shout(`"hello`") + name; }`ntest `"greeting`"`n{`n  check(greeting(`"a`") == `"hello! a`");`n}"
  Set-Content 'libraries\greet\tests\more.kpp' "test `"greeting twice`"`n{`n  check(greeting(`"b`") == `"hello! b`");`n  check(greeting(`"c`") == `"wrong`");`n}"
  Set-Content 'libraries\text\build.kpp' "void build(Project& project)`n{`n  Library& text = project.library(`"text`");`n  text.version = `"1.0.0`";`n}"
  Set-Content 'libraries\text\src\text.kpp' "String shout(StringView s) { String r = s; r += `"! `"; return r; }"
  $output = Kpp @('--no-server')
  $printed = if ($code -eq 0) { & '.\build\debug\app.exe' } else { '' }
  if ($code -ne 0 -or "$printed" -ne 'hello! app') { Fail "an executable using a library: exit $code, printed '$printed'`n$($output -join "`n")" }
  Push-Location 'libraries\greet'
  $output = (Kpp @('--test', '--no-server')) -join "`n"
  if ($code -ne 1 -or $output -notmatch '2 tests, 1 failed' -or $output -notmatch 'more\.kpp:4:3: check failed') { Fail "kpp --test in a library: exit $code`n$output" }
  $output = (Kpp @('--test', '--no-server', '--', '--filter', 'greet.kpp > greeting')) -join "`n"
  if ($code -ne 0 -or $output -notmatch '\b1 test\b') { Fail "kpp --test -- --filter: exit $code`n$output" }
  Set-Content 'build.kpp' ($greet -replace '0\.1\.0', '99.0.0')
  Pop-Location
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'the library greet needs kpp 99\.0\.0 or later') { Fail "a minimum kpp above this one: exit $code`n$output" }
  Set-Content 'libraries\greet\build.kpp' $greet
  Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"greet`", `"0.3`");`n}"
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'it uses the library greet 0\.3 or later, and .* has 0\.2\.1') { Fail "a minimum version above the library's: exit $code`n$output" }
  Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"jsno`");`n}"
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'the library jsno is not in .*libraries\\jsno') { Fail "a missing library: exit $code`n$output" }
}
finally { Pop-Location }
if ($failed -eq 0) { Write-Output 'ok   project' }
exit $failed
