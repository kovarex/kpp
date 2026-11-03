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
  if ($code -ne 0 -or ($output -join "`n") -notmatch 'Hello from kpp') { Fail "kpp new, then kpp --run: $output" }
  # Outputs deleted under the live compile server (kovarex, 2026-11-02): the unchanged program's
  # build makes them again, never "built" for an executable that is not there. The output directory,
  # then one module's object, the executable, the .pdb.
  $program = (Get-ChildItem 'build\debug\*.exe').FullName
  $pdb = [IO.Path]::ChangeExtension($program, '.pdb')
  foreach ($removed in @(@('the output directory', 'build\debug'), @("one module's object", "$program.0.o"), @('the executable', $program), @('the .pdb', $pdb)))
  {
    if ($removed[1] -eq 'build\debug') { cmd /c rd /s /q build\debug } else { Remove-Item -LiteralPath $removed[1] }
    $output = Kpp @('--run')
    $missing = @($program, "$program.0.o", $pdb) | Where-Object { -not (Test-Path -LiteralPath $_) }
    if ($code -ne 0 -or ($output -join "`n") -notmatch 'Hello from kpp' -or $missing)
    { Fail "$($removed[0]) deleted under the compile server, then kpp --run: exit $code, missing [$($missing -join ', ')]`n$($output -join "`n")"; break }
  }
  # A build with an option of its own has a server of its own: kpp --stop-server stops it with the
  # others and says which (2026-11-02).
  Kpp @('--verbose', '--run') | Out-Null
  $output = Kpp @('--stop-server')
  if ($code -ne 0 -or ($output -join "`n") -notmatch 'kpp: stopped the compile server of .*\(started with --verbose\)') { Fail "kpp --stop-server after kpp --verbose --run: exit $code`n$($output -join "`n")" }
  $output = Kpp @('--stop-server')
  if ($code -ne 0 -or ($output -join "`n") -notmatch 'kpp: no compile server of .* runs') { Fail "kpp --stop-server again: exit $code`n$($output -join "`n")" }

  New-Item -ItemType Directory -Force 'tests', 'include' | Out-Null
  Set-Content 'src\main.kpp' "int main()`n{`n  println(`"game `", answer());`n  return 0;`n}"
  Set-Content 'src\answer.kpp' "int answer() { return GREETING_NUMBER; }"
  Set-Content 'tests\tests.kpp' "int main()`n{`n  println(`"tests `", plus_one(answer()));`n  return 0;`n}"
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
  # A C library's resource script (llvm-rc) naming a file next to it and one outside its directory, as
  # factorio-in-kpp's names its icon in ..\..\data. With the outside one missing the build fails, and
  # what llvm-rc wrote before it failed is no .res that the next build takes as made (it was linked,
  # and lld-link failed on it: "Stream Error"). Then there: the next build compiles the script again,
  # and its .res has that file's bytes; then changed: again.
  Set-Content 'libs\plus\plus.rc' "1 RCDATA `"first.bin`"`n2 RCDATA `"../../data/second.bin`""
  Set-Content -LiteralPath 'libs\plus\first.bin' -Value 'KPP-FIRST' -NoNewline # (named: -NoNewline first swaps path and value)
  Set-Content 'build.kpp' ($good -replace 'plus\.define\("PLUS=1"\);', "plus.define(`"PLUS=1`");`n  plus.resource(`"libs/plus/plus.rc`");")
  $res = 'build\clib\plus\plus.res'
  $output = Kpp @('--debug', '--no-server', '--set', "INCLUDE_ROOT=$root")
  if ($code -eq 0 -or ($output -join "`n") -notmatch 'plus\.rc: error: llvm-rc could not compile it') { Fail "a resource script naming a missing file: exit $code`n$($output -join "`n")" }
  elseif (Test-Path $res) { Fail "a resource script naming a missing file left $res ($((Get-Item $res).Length) bytes) for the next build" }
  New-Item -ItemType Directory -Force 'data' | Out-Null
  foreach ($marker in 'KPP-SECOND-ONE', 'KPP-SECOND-TWO')
  {
    Set-Content -LiteralPath 'data\second.bin' -Value $marker -NoNewline
    $output = Kpp @('--debug', '--no-server', '--set', "INCLUDE_ROOT=$root")
    $bytes = if (Test-Path $res) { [IO.File]::ReadAllText((Resolve-Path $res).Path, [Text.Encoding]::ASCII) } else { '' }
    if ($code -ne 0 -or -not $bytes.Contains($marker)) { Fail "the resource script's file outside its directory, now ${marker}: exit $code, in ${res}: $($bytes.Contains($marker))`n$($output -join "`n")" }
  }
  Set-Content 'build.kpp' $good
  Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"x`", {undefinedName});`n}"
  $output = Kpp @('--no-server')
  if ($code -eq 0 -or ($output -join "`n") -notmatch "build\.kpp:3: error: ") { Fail "a build.kpp with an error: exit $code`n$($output -join "`n")" }
  Set-Content 'build.kpp' "extern `"C`" int puts(const char*);`nvoid build(Project& project)`n{`n  puts(`"io`");`n}"
  $output = Kpp @('--no-server')
  if ($code -eq 0 -or ($output -join "`n") -notmatch 'puts, a C function, which has no body to run at compile time') { Fail "a build() calling C: exit $code`n$($output -join "`n")" }
  Set-Content 'build.kpp' $good
}
finally { Pop-Location }

# kpp libraries (docs/build.md, "kpp libraries"): app uses greet from its libraries\, greet uses text
# next to it there; kpp --test in greet runs its tests and its test roots' with the runner's
# arguments; a minimum version above greet's, a missing library and a minimum kpp above this one
# are errors. greet's option suffix (config::greet::suffix): its default, a configuration's value,
# the command line's, `kpp options` (with greet scoped and text unscoped, design.md 16), and three
# errors: a required option unset, an unknown option, a value not of its type; a library name that is
# no identifier is an error.
$app = Join-Path $work 'app'
New-Item -ItemType Directory -Force "$app\src", "$app\libraries\greet\src", "$app\libraries\greet\tests", "$app\libraries\text\src" | Out-Null
Push-Location $app
try
{
  $appBuild = "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"greet`", `"0.2`");`n  project.configuration(`"release`").config(`"greet::suffix`", `".`");`n}"
  Set-Content 'build.kpp' $appBuild
  Set-Content 'src\main.kpp' "int main()`n{`n  println(greet::greeting(`"app`"));`n  return 0;`n}"
  $greet = @'
void build(Project& project)
{
  Library& greet = project.library("greet");
  greet.version = "0.2.1";
  greet.license = "MIT";
  greet.minimumKpp = "0.1.0";
  greet.test("tests");
  greet.use("text");
  greet.option("suffix", "", "what follows the name");
}
'@
  Set-Content 'libraries\greet\build.kpp' $greet
  Set-Content 'libraries\greet\src\greet.kpp' "String greeting(StringView name) { return shout(`"hello`") + name + config::greet::suffix; }`ntest `"greeting`"`n{`n  check(greeting(`"a`") == `"hello! a`");`n}"
  Set-Content 'libraries\greet\tests\more.kpp' "test `"greeting twice`"`n{`n  check(greeting(`"b`") == `"hello! b`");`n  check(greeting(`"c`") == `"wrong`");`n}"
  $text = "void build(Project& project)`n{`n  Library& text = project.library(`"text`");`n  text.version = `"1.0.0`";`n  text.unscoped = true;`n}"
  Set-Content 'libraries\text\build.kpp' $text
  Set-Content 'libraries\text\src\text.kpp' "String shout(StringView s) { String r = s; r << `"! `"; return r; }"
  $output = Kpp @('--no-server')
  $printed = if ($code -eq 0) { & '.\build\debug\app.exe' } else { '' }
  if ($code -ne 0 -or "$printed" -ne 'hello! app') { Fail "an executable using a library: exit $code, printed '$printed'`n$($output -join "`n")" }
  # greet is scoped: a bare greeting() from the program is an error; text is unscoped (shout bare in greet).
  Set-Content 'src\main.kpp' "int main()`n{`n  println(greeting(`"app`"));`n  return 0;`n}"
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'did you mean greet::greeting') { Fail "a scoped library's name used bare: exit $code`n$output" }
  Set-Content 'src\main.kpp' "int main()`n{`n  println(greet::greeting(`"app`"));`n  return 0;`n}"
  foreach ($case in @(@(@('release'), 'hello! app.'), @(@('--config', 'greet::suffix=?'), 'hello! app?')))
  {
    $output = Kpp ($case[0] + @('--no-server', '--run'))
    if ($code -ne 0 -or $output[-1] -ne $case[1]) { Fail "the option greet::suffix, kpp $($case[0] -join ' '): '$($output[-1])', not '$($case[1])'`n$($output -join "`n")" }
  }
  $output = (Kpp @('options', 'release')) -join "`n"
  if ($code -ne 0 -or $output -notmatch 'greet::suffix = \.  string, default ; set by the configuration release') { Fail "kpp options: exit $code`n$output" }
  if ($output -notmatch 'library greet, scoped: greet::NAME' -or $output -notmatch 'library text, unscoped: its names are bare') { Fail "kpp options, the libraries' scopes:`n$output" }
  Set-Content 'libraries\text\build.kpp' "void build(Project& project)`n{`n  Library& text = project.library(`"te-xt`");`n  text.version = `"1.0.0`";`n}"
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'the library "te-xt" needs a name that is an identifier') { Fail "a library named te-xt: exit $code`n$output" }
  Set-Content 'libraries\text\build.kpp' $text
  foreach ($case in @(
      @('greet.requiredOption("volume", "int", "how loud");', '', 'the library greet needs its option volume \(int: how loud\), which nothing sets for app debug'),
      @('', '  project.config("greet::colour", "red");', 'greet::colour is set by the project \(project.config\), and the library greet has no option colour \(its options: suffix\)'),
      @('greet.option("times", 1, "how often");', '  project.config("greet::times", "often");', 'greet::times is often \(set by the project \(project.config\)\), which is not an int')))
  {
    Set-Content 'libraries\greet\build.kpp' ($greet -replace 'greet.use\("text"\);', "greet.use(`"text`");`n  $($case[0])")
    Set-Content 'build.kpp' ($appBuild -replace '\}$', "$($case[1])`n}")
    $output = (Kpp @('--no-server')) -join "`n"
    if ($code -eq 0 -or $output -notmatch $case[2]) { Fail "a library option's error, '$($case[2])': exit $code`n$output" }
  }
  Set-Content 'libraries\greet\build.kpp' $greet
  Set-Content 'build.kpp' $appBuild
  Push-Location 'libraries\greet'
  $output = (Kpp @('--test', '--no-server')) -join "`n"
  if ($code -ne 1 -or $output -notmatch '2 tests, 1 failed' -or $output -notmatch 'more\.kpp:4:3: check failed') { Fail "kpp --test in a library: exit $code`n$output" }
  $output = (Kpp @('--test', '--no-server', '--', '--filter', 'greet.kpp > greeting')) -join "`n"
  if ($code -ne 0 -or $output -notmatch '\b1 test\b') { Fail "kpp --test -- --filter: exit $code`n$output" }
  Set-Content 'build.kpp' ($greet -replace '0\.1\.0', '99.0.0')
  Pop-Location
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'the library greet needs kpp 99\.0\.0 or later') { Fail "a minimum kpp above this one: exit $code`n$output" }
  # Against this kpp's own version (kppc --version; 0.2.N+dirty counts as 0.2.N): exactly it builds,
  # its next patch is refused. (The version compared was a constant, 0.1.0, until 2026-11-03.)
  if ((& $Kppc --version | Select-Object -First 1) -match '^kpp (\d+)\.(\d+)\.(\d+)')
  {
    $own = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
    $next = "$($Matches[1]).$($Matches[2]).$([int]$Matches[3] + 1)"
    Set-Content 'libraries\greet\build.kpp' ($greet -replace '0\.1\.0', $own)
    $output = (Kpp @('--no-server')) -join "`n"
    if ($code -ne 0) { Fail "a minimum kpp equal to this one ($own): exit $code`n$output" }
    Set-Content 'libraries\greet\build.kpp' ($greet -replace '0\.1\.0', $next)
    $output = (Kpp @('--no-server')) -join "`n"
    if ($code -eq 0 -or $output -notmatch "the library greet needs kpp $([regex]::Escape($next)) or later, and this is kpp $([regex]::Escape($own))") { Fail "a minimum kpp one patch above this one ($next): exit $code`n$output" }
  }
  else { Fail "kppc --version names no version: $(& $Kppc --version | Select-Object -First 1)" }
  Set-Content 'libraries\greet\build.kpp' $greet
  Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"greet`", `"0.3`");`n}"
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'it uses the library greet 0\.3 or later, and .* has 0\.2\.1') { Fail "a minimum version above the library's: exit $code`n$output" }
  Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"jsno`");`n}"
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'the library jsno is not in .*libraries\\jsno') { Fail "a missing library: exit $code`n$output" }
  # A converted library's shape: root(".") (its own build.kpp is not one of its sources) and C
  # headers beside its kpp, which the cell's --ext .kpp leaves out of the library's roots too.
  New-Item -ItemType Directory -Force 'libraries\conv' | Out-Null
  Set-Content 'libraries\conv\build.kpp' "void build(Project& project)`n{`n  Library& conv = project.library(`"conv`");`n  conv.version = `"1.0.0`";`n  conv.root(`".`");`n}"
  Set-Content 'libraries\conv\conv.kpp' "int twice(int x) { return 2 * x; }"
  Set-Content 'libraries\conv\conv.h' "this is a C header, not kpp"
  Set-Content 'src\main.kpp' "int main()`n{`n  println(conv::twice(21));`n  return 0;`n}"
  $convApp = "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"conv`");`n  app.argument(`"--ext`");`n  app.argument(`".kpp`");`n}"
  Set-Content 'build.kpp' $convApp
  $output = Kpp @('--no-server', '--run')
  if ($code -ne 0 -or $output[-1] -ne '42') { Fail "a library rooted at its directory with C headers, --ext .kpp: exit $code`n$($output -join "`n")" }
  Set-Content 'build.kpp' ($convApp -replace '(?m)^  app\.argument.*\r?\n', '')
  $output = (Kpp @('--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'conv\.h') { Fail "without --ext .kpp, the library's C header is a source (an error in conv.h): exit $code`n$output" }
}
finally { Pop-Location }
# Targets x configurations (docs/build.md, its section on them): two targets in debug, release
# and fastdebug (debug's constants, optimized: no incremental link's .ilk), config constants set for
# the project, a configuration, a target and one cell (configure), the command line's --config over
# them all, `kpp TARGET CONFIGURATION` in either order, the Visual Studio solution's configurations,
# and three errors: a base that is not declared, a target named as a configuration, an unknown name.
$matrix = Join-Path $work 'matrix'
New-Item -ItemType Directory -Force "$matrix\src", "$matrix\tests" | Out-Null
Push-Location $matrix
try
{
  Set-Content 'src\main.kpp' "int main()`n{`n  println(`"main `", config::debug, `" `", config::level, `" `", config::steam);`n  return 0;`n}"
  Set-Content 'tests\tests.kpp' "int main()`n{`n  println(`"tests `", config::level, `" `", config::unit);`n  return 0;`n}"
  $matrixBuild = @'
void build(Project& project)
{
  project.config("level", 1);
  project.config("steam", false);
  Configuration& release = project.configuration("release");
  release.config("level", 2);
  Configuration& fast = project.configuration("fastdebug", "debug");
  fast.optimize = true;
  fast.config("level", 3);
  project.executable("main", {"src"});
  Executable& tests = project.executable("checks", {"src", "tests"});
  tests.exclude("src/main.kpp");
  tests.config("unit", true);
  tests.config("level", 4); // the target's, over the configuration's
  // Run and the debugger start a copy in a directory per configuration: {configuration} in debug().
  tests.afterBuild("cmd /c \"if not exist copies\\{configuration} mkdir copies\\{configuration}\" && copy /y \"{exe}\" copies\\{configuration}\\ >nul");
  tests.debug("copies/{configuration}/checks.exe", "", "copies/{configuration}");
}
void configure(Cell& cell)
{
  if (cell.configuration.name == "release" && cell.target.name == "main")
    cell.config("steam", true);
  // Factorio's layout, a name per configuration that is not the configuration's own (bin\Releasex64):
  // made here, for the build step and the debugger alike.
  if (cell.target.name == "main")
  {
    String name = String(cell.configuration.name == "debug" ? "Debug" : cell.configuration.name == "release" ? "Release" : "FastDebug") + "x64";
    cell.target.afterBuild("cmd /c \"if not exist bin\\" + name + " mkdir bin\\" + name + "\" && copy /y \"{exe}\" bin\\" + name + "\\ >nul");
    cell.target.debug("bin/" + name + "/main.exe");
  }
}
'@
  Set-Content 'build.kpp' $matrixBuild
  foreach ($case in @(
      @(@('main'), 'main true 1 false'),
      @(@('main', 'release'), 'main false 2 true'),
      @(@('fastdebug', 'main'), 'main true 3 false'),
      @(@('checks', 'release'), 'tests 4 true'),
      @(@('main', 'release', '--config', 'level=9'), 'main false 9 true')))
  {
    $output = Kpp ($case[0] + @('--no-server', '--run'))
    if ($code -ne 0 -or $output[-1] -ne $case[1]) { Fail "kpp $($case[0] -join ' ') --run printed '$($output[-1])', not '$($case[1])'`n$($output -join "`n")" }
  }
  if (-not (Test-Path 'build\debug\main.ilk') -or (Test-Path 'build\fastdebug\main.ilk')) { Fail 'fastdebug is not optimized: debug has main.ilk, fastdebug must not' }
  $output = Kpp @('--no-server', '--generate-visual-studio-files')
  $solution = Get-Content 'build\visual-studio\matrix.sln' -Raw
  $project = Get-Content 'build\visual-studio\main.vcxproj' -Raw
  if ($code -ne 0 -or $solution -notmatch 'Fastdebug\|x64 = Fastdebug\|x64' -or $project -notmatch 'main fastdebug --msvc-diagnostics') { Fail "the solution's configurations`n$($output -join "`n")" }
  # Each configuration's debugger starts its own copy: configure(Cell&)'s name, debug()'s {configuration}.
  foreach ($copy in 'bin\Debugx64\main.exe', 'bin\Releasex64\main.exe', 'bin\FastDebugx64\main.exe', 'copies\release\checks.exe')
  {
    if (-not (Test-Path $copy)) { Fail "no ${copy}: the after-build step did not copy it" }
  }
  $matrixPath = (Resolve-Path $matrix).Path
  foreach ($expected in @(
      @('main.vcxproj', 'Release', "<LocalDebuggerCommand>$([regex]::Escape("$matrixPath\bin\Releasex64\main.exe"))</LocalDebuggerCommand>"),
      @('main.vcxproj', 'Fastdebug', "<LocalDebuggerCommand>$([regex]::Escape("$matrixPath\bin\FastDebugx64\main.exe"))</LocalDebuggerCommand>"),
      @('checks.vcxproj', 'Release', "<LocalDebuggerCommand>$([regex]::Escape("$matrixPath\copies\release\checks.exe"))</LocalDebuggerCommand>\s*<LocalDebuggerCommandArguments></LocalDebuggerCommandArguments>\s*<LocalDebuggerWorkingDirectory>$([regex]::Escape("$matrixPath\copies\release"))</LocalDebuggerWorkingDirectory>")))
  {
    $text = Get-Content "build\visual-studio\$($expected[0])" -Raw
    $group = [regex]::Match($text, "(?s)<PropertyGroup Condition=`"'\`$\(Configuration\)' == '$($expected[1])'`">.*?</PropertyGroup>").Value
    if ($group -notmatch $expected[2]) { Fail "$($expected[0]) $($expected[1]) does not debug $($expected[2])`n$group" }
  }
  foreach ($case in @(
      @("void build(Project& project)`n{`n  project.configuration(`"asan`", `"nosuch`");`n}", 'the configuration asan is based on nosuch, which build\.kpp does not declare'),
      @("void build(Project& project)`n{`n  project.executable(`"release`", {`"src`"});`n}", 'release is a target and a configuration')))
  {
    Set-Content 'build.kpp' $case[0]
    $output = (Kpp @('--no-server')) -join "`n"
    if ($code -eq 0 -or $output -notmatch $case[1]) { Fail "a build.kpp error, '$($case[1])': exit $code`n$output" }
  }
  Set-Content 'build.kpp' $matrixBuild
  $output = (Kpp @('nosuch', '--no-server')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'describes no target or configuration nosuch \(its configurations: debug release fastdebug\)') { Fail "an unknown name: exit $code`n$output" }

  # In a project's directory, --version and --help print and build nothing, and an option kppc does
  # not have is an error, never passed on or ignored (kovarex's `kpp --version` built the project).
  cmd /c rd /s /q build
  $output = (Kpp @('--version')) -join "`n"
  if ($code -ne 0 -or $output -notmatch '^kpp \d+\.\d+\.\d+(\+dirty)?, commit [0-9a-f]{40}\r?\n' -or $output -notmatch "(?m)^compiler: $([regex]::Escape($Kppc))$" -or
      $output -notmatch '(?m)^toolchain: ' -or $output -notmatch '(?m)^stdlib: .*stdlib$') { Fail "kpp --version in a project: exit $code`n$output" }
  $output = (Kpp @('--help')) -join "`n"
  if ($code -ne 0 -or $output -notmatch '^usage: kppc' -or $output -notmatch '--version') { Fail "kpp --help in a project: exit $code`n$output" }
  foreach ($case in @(
      @(@('--frobnicate'), 'error: unknown option --frobnicate'),
      @(@('main', '--no-server', '--versoin'), 'error: unknown option --versoin'),
      @(@('--release', '--version'), 'error: --version takes no other arguments'),
      @(@('main', '--exe'), 'error: --exe needs a value'),
      @(@('--library-root', 'x'), 'error: --library-root needs two values'),
      @(@('--set'), 'error: --set needs NAME=VALUE')))
  {
    $output = (Kpp $case[0]) -join "`n"
    if ($code -ne 2 -or $output -notmatch [regex]::Escape($case[1])) { Fail "kpp $($case[0] -join ' '): exit $code, not 2 with '$($case[1])'`n$output" }
  }
  if (Test-Path 'build') { Fail 'kpp --version, --help or an option error built the project' }
  # kppc's options with their values go on to the cells' builds: --toolchain's value is no target name.
  $output = (Kpp @('main', '--no-server', '--toolchain', 'nosuch')) -join "`n"
  if ($code -eq 0 -or $output -notmatch [regex]::Escape("--toolchain takes bundled, msvc or a list of them in the order preferred (msvc,bundled), not 'nosuch'")) { Fail "kpp main --toolchain nosuch: exit $code`n$output" }
}
finally { Pop-Location }

# Outside a project: an unknown option of kppc is the same error. A copy of kppc elsewhere (an
# installation) prints the version and commit it was built from.
$output = (Kpp @('--frobnicate', "$work\src")) -join "`n"
if ($code -ne 2 -or $output -notmatch 'error: unknown option --frobnicate') { Fail "kppc --frobnicate: exit $code`n$output" }
$installation = Join-Path $work 'installation'
New-Item -ItemType Directory -Force $installation | Out-Null
Copy-Item $Kppc "$installation\kpp.exe"
$built = (& $Kppc --version | Select-Object -First 1)
# No toolchain of its own and no Visual Studio (KPP_VSWHERE naming a vswhere that is not there):
# --version still prints, and says what is missing.
$saved = $env:KPP_CLANG, $env:KPP_VSWHERE
$env:KPP_CLANG = $null
$env:KPP_VSWHERE = Join-Path $installation 'no-vswhere.exe'
$ErrorActionPreference = 'Continue'
$output = (& "$installation\kpp.exe" --version 2>&1 | ForEach-Object { "$_" }) -join "`n"
$ErrorActionPreference = 'Stop'
$env:KPP_CLANG, $env:KPP_VSWHERE = $saved
if (-not $output.StartsWith("$built`n") -or $output -notmatch '(?m)^stdlib: .*\(missing\)$' -or
    $output -notmatch '(?m)^toolchain: none: this kpp has no toolchain of its own') { Fail "an installation's kpp --version:`n$output" }

# kpp measure (docs/build.md, "kpp measure"): build.kpp's edit made, built, put back and built, the
# clean build and each executable's whole build, in two rounds; the table's rows, each the average
# of its rounds; the file as committed afterwards. Then a file with changes of its own: refused, nothing built or changed.
$measure = Join-Path $work 'measure'
New-Item -ItemType Directory -Force $measure | Out-Null
Push-Location $measure
try
{
  Kpp @('new') | Out-Null
  Set-Content 'build.kpp' @'
void build(Project& project)
{
  project.executable("hello", {"src"});
  project.executable("again", {"src"});
  project.measure("hello").edit("Edit of one function", "src/main.kpp", "Hello from kpp", "Hello again from kpp");
}
'@
  # git's warnings on stderr (CI's core.autocrlf=true: "LF will be replaced by CRLF") are no errors:
  # under ErrorActionPreference Stop, Windows PowerShell made them terminating (CI red, 2026-11-03).
  # Its exit codes are checked instead, and the test's repository keeps its files' line ends.
  $ErrorActionPreference = 'Continue'
  $gitOutput = @(git init -q 2>&1; git config core.autocrlf false 2>&1; git add -A 2>&1;
                 git -c user.name=test -c user.email=test@test commit -q -m measure 2>&1) | ForEach-Object { "$_" }
  $gitCode = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($gitCode -ne 0) { Fail "git for kpp measure's repository: exit $gitCode`n$($gitOutput -join "`n")" }
  # Two rounds: each row of the table is the average of its rounds' times (the lines before it, two
  # decimals: within 0.1 s), one decimal, and the heading says so.
  $output = (Kpp @('measure', '--rounds', '2')) -join "`n"
  if ($code -ne 0 -or $output -notmatch 'average of 2 rounds') { Fail "kpp measure: exit $code, no `"average of 2 rounds`"`n$output" }
  foreach ($row in 'Edit of one function', 'Edit of one function, undone', 'Clean build of hello, libraries already built',
                   'Whole build of hello.exe, every library included', 'Whole build of again.exe, every library included')
  {
    $name = [regex]::Escape($row)
    $rounds = @([regex]::Matches($output, "(?m)^  $name +([\d.]+) s$") | ForEach-Object { [double]$_.Groups[1].Value })
    $table = [regex]::Match($output, "(?m)^\| $name \| (\d+\.\d) s \|$")
    if ($rounds.Count -ne 2 -or -not $table.Success -or [math]::Abs([double]$table.Groups[1].Value - ($rounds[0] + $rounds[1]) / 2) -gt 0.1)
    { Fail "kpp measure: the row `"$row`": rounds [$($rounds -join ', ')], table [$($table.Value)]`n$output"; break }
  }
  $ErrorActionPreference = 'Continue'
  $status = git status --porcelain --untracked-files=no 2>$null
  $ErrorActionPreference = 'Stop'
  if ($status) { Fail "kpp measure left changes: $status" }
  Add-Content 'src\main.kpp' '// a change of its own'
  $before = Get-Content -Raw 'src\main.kpp'
  $output = (Kpp @('measure', '--rounds', '1')) -join "`n"
  if ($code -eq 0 -or $output -notmatch 'main\.kpp has changes of its own' -or (Get-Content -Raw 'src\main.kpp') -ne $before) { Fail "kpp measure with a changed file: exit $code`n$output" }
  Kpp @('--stop-server') | Out-Null
}
finally { Pop-Location }
if ($failed -eq 0) { Write-Output 'ok   project' }
exit $failed
