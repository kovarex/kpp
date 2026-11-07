# A project with a build.kpp (docs/build.md, "build.kpp"; src/Project/, projectlib/Project.kpp).
# `kpp new` makes one and `kpp --run` prints its greeting. Then a build.kpp with two executables from
# one src\ (the second leaves out the first's main and has its own in tests\), a --set setting
# reaching a program through a C include directory, and the release build. A build.kpp that does not
# compile, and one whose build() calls C, fail with an error. The executables are built through
# the compile server once (stopped with kpp --stop-server) and otherwise with --no-server.
# run_tests.cmd runs it. Its sections run at once, each in a process of its own (-Section NAME runs
# one): their projects are directories of their own under $work, so the test takes its longest
# section's time, not their sum (kovarex, 2026-11-04: the gate must be fast).
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [string]$Section = '')
$ErrorActionPreference = 'Stop'
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\project'))
# The compile servers of this test's projects, stopped at its start and its end (testservers.ps1).
. "$PSScriptRoot\testservers.ps1"
function StopTestServers() { Stop-TestServers $work }
if (-not $Section)
{
  StopTestServers
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }
  New-Item -ItemType Directory -Force $work | Out-Null
}
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

# The sections, each started as `project.ps1 -Section NAME`, its output kept and shown in this order,
# its exit code its failures; one that stops on an error of its own (not a Fail) is a failure too.
$sections = 'basic', 'clibs', 'libraries', 'syslibs', 'matrix', 'outside', 'runfile', 'measure', 'paths', 'readonly', 'linkinputs'
function Run([string]$name) { return $Section -eq $name }
if (-not $Section)
{
  $running = foreach ($name in $sections)
  {
    $out = Join-Path $work "section-$name.out.txt"
    $err = Join-Path $work "section-$name.err.txt"
    $p = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Kppc', "`"$Kppc`"", '-Section', $name `
           -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -NoNewWindow
    $null = $p.Handle # (so that ExitCode is kept)
    [pscustomobject]@{ name = $name; process = $p; out = $out; err = $err }
  }
  $started = Get-Date
  $times = @()
  foreach ($r in $running)
  {
    $r.process.WaitForExit()
    $times += "$($r.name) $([int]($r.process.ExitTime - $started).TotalSeconds) s"
    $printed = @(Get-Content $r.out -ErrorAction SilentlyContinue)
    $printed
    $code = $r.process.ExitCode
    if ($code -ne 0 -and -not ($printed -match '^FAIL project: ')) { Fail "section $($r.name) stopped (exit $code):`n$((Get-Content $r.err -ErrorAction SilentlyContinue) -join "`n")" }
    else { $failed += $code }
  }
  StopTestServers
  Write-Output "project: sections at once, each done after $($times -join ', ')"
  if ($failed -eq 0) { Write-Output 'ok   project' }
  exit $failed
}

if (Run 'basic')
{
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
      $savedLink = $env:LINK
      if ($removed[1] -eq $pdb) { $env:LINK = '/time+' } # (link.exe's phases in the link's output, kpp's)
      $output = Kpp @('--run')
      $env:LINK = $savedLink
      $missing = @($program, "$program.0.o", $pdb) | Where-Object { -not (Test-Path -LiteralPath $_) }
      if ($code -ne 0 -or ($output -join "`n") -notmatch 'Hello from kpp' -or $missing)
      { Fail "$($removed[0]) deleted under the compile server, then kpp --run: exit $code, missing [$($missing -join ', ')]`n$($output -join "`n")"; break }
      # Without its .pdb the link starts from nothing (Project/Build.cpp: the .ilk and the executable go too).
      # Given the rest of its incremental state, link.exe said "program database ... missing; performing
      # full link" and linked over it, and that link crashed now and then (LNK1000 in
      # IMAGE::BuildImage.FinalPhase, CI runs 41825684415 and 41927566409).
      if ($removed[1] -eq $pdb)
      {
        $linkSaid = @($output | Where-Object { $_ -match 'Linker: (Pass 1|IncrPass)|performing full link' })
        if (-not ($linkSaid -match 'Pass 1') -or ($linkSaid -match 'IncrPass|performing full link'))
        { Fail "the .pdb deleted, the relink was not one from nothing (link.exe: $($linkSaid -join '; '))" }
      }
    }
    # Visual Studio's builds and a terminal's (2026-11-04): VS's pass --msvc-diagnostics, and both
    # write build\debug. Each build's program prints what its source says, whichever built last: when
    # they had a server each, one linked the other's objects as its own (source "one", the program
    # "two"). They share one server now (server.cpp, serverKey), which the loop below checks too.
    $main = Get-Content -Raw 'src\main.kpp'
    foreach ($step in @(@('one', '--msvc-diagnostics'), @('one', ''), @('two', '--msvc-diagnostics'), @('one', ''), @('three', ''), @('two', '--msvc-diagnostics')))
    {
      [IO.File]::WriteAllText((Join-Path (Get-Location) 'src\main.kpp'), ($main -replace 'Hello from kpp', "Hello $($step[0])"))
      $output = Kpp (@($step[1] | Where-Object { $_ }) + @('--run'))
      if ($code -ne 0 -or ($output -join "`n") -notmatch "(?m)^Hello $($step[0])$")
      { Fail "two servers (the source says `"Hello $($step[0])`", built $(if ($step[1]) { 'with' } else { 'without' }) --msvc-diagnostics): exit $code`n$($output -join "`n")"; break }
    }
    # One server for both: its diagnostics are in each client's form, `main.kpp(N)` for VS and
    # `main.kpp:N` for a terminal, the same error kept by the server (the second and third builds
    # are its replays: no input changed).
    [IO.File]::WriteAllText((Join-Path (Get-Location) 'src\main.kpp'), $main + "`nint broken = noSuchName;`n")
    foreach ($step in @('--msvc-diagnostics', '', '--msvc-diagnostics'))
    {
      $output = (Kpp (@('--debug') + @($step | Where-Object { $_ }))) -join "`n"
      $place = if ($step) { '\(\d+\)' } else { ':\d+' }
      if ($code -eq 0 -or $output -notmatch "main\.kpp$place`: error: .*noSuchName" -or $output -match "[\x01\x02\x03]")
      { Fail "one server's error in the client's form (kpp $step): exit $code`n$output"; break }
    }
    [IO.File]::WriteAllText((Join-Path (Get-Location) 'src\main.kpp'), $main)
    Kpp @('--stop-server') | Out-Null
    # `kpp` alone (2026-11-04). Input from a pipe asks nothing and says so, listing the commands
    # (exit 2). The pipe a Cygwin or MSYS terminal gives its programs (Git Bash's mintty:
    # \msys-<hash>-pty0-from-master) is a terminal: the menu asks, and 0 quits.
    $ErrorActionPreference = 'Continue'
    $output = ('' | & $Kppc 2>&1 | ForEach-Object { "$_" }) -join "`n"
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 2 -or $output -notmatch 'kpp debug' -or $output -notmatch 'nothing was asked') { Fail "kpp alone, input from a pipe: exit $code`n$output" }
    $pipeName = 'msys-{0:x16}-pty0-from-master' -f $PID # (one per run: two worktrees' gates at once found one name's instances busy)
    $pipe = New-Object IO.Pipes.NamedPipeServerStream($pipeName, [IO.Pipes.PipeDirection]::Out)
    $connected = $pipe.WaitForConnectionAsync()
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = "$env:SystemRoot\System32\cmd.exe"
    $info.Arguments = "/d /s /c `"`"$Kppc`" < \\.\pipe\$pipeName`""
    $info.WorkingDirectory = (Get-Location).Path
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $menu = [Diagnostics.Process]::Start($info)
    $read = $menu.StandardOutput.ReadToEndAsync()
    if ($connected.Wait(30000))
    {
      $answer = [Text.Encoding]::ASCII.GetBytes("0`r`n")
      $pipe.Write($answer, 0, $answer.Length)
      $pipe.Flush()
    }
    $pipe.Dispose()
    if (-not $menu.WaitForExit(60000)) { $menu.Kill() }
    if ($menu.ExitCode -ne 0 -or $read.Result -notmatch '1  build, debug' -or $read.Result -match 'nothing was asked') { Fail "kpp alone, input from a mintty-like pipe: exit $($menu.ExitCode)`n$($read.Result)" }
    # A build with an option of its own has a server of its own: kpp --stop-server stops it with the
    # others and says which (2026-11-02).
    Kpp @('--verbose', '--run') | Out-Null
    $output = Kpp @('--stop-server')
    if ($code -ne 0 -or ($output -join "`n") -notmatch 'kpp: stopped the compile server of .*\(started with --verbose\)') { Fail "kpp --stop-server after kpp --verbose --run: exit $code`n$($output -join "`n")" }
    $output = Kpp @('--stop-server')
    if ($code -ne 0 -or ($output -join "`n") -notmatch 'kpp: no compile server of .* runs') { Fail "kpp --stop-server again: exit $code`n$($output -join "`n")" }
    # A project inside this one's directory (as the other sections' projects are under this one's,
    # running at once: one of their servers was stopped here, 2026-11-05) keeps its server: its files
    # are under this directory but not under this project's build\.
    New-Item -ItemType Directory -Force 'inner' | Out-Null
    Push-Location 'inner'
    Kpp @('new') | Out-Null
    Kpp @('--run') | Out-Null
    Pop-Location
    $output = Kpp @('--stop-server')
    if ($code -ne 0 -or ($output -join "`n") -notmatch 'kpp: no compile server of .* runs') { Fail "kpp --stop-server with a project inside this one, whose server runs: exit $code`n$($output -join "`n")" }
    Push-Location 'inner'
    $output = Kpp @('--stop-server')
    Pop-Location
    if ($code -ne 0 -or ($output -join "`n") -notmatch 'kpp: stopped the compile server of .*inner\.exe') { Fail "the inner project's kpp --stop-server: exit $code`n$($output -join "`n")" }
  }
  finally { Pop-Location }
}

# C libraries of a project (its own directory, so that it runs beside basic).
if (Run 'clibs')
{
  $clibs = Join-Path $work 'clibs'
  New-Item -ItemType Directory -Force "$clibs\src" | Out-Null
  Push-Location $clibs
  try
  {

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
    # A configuration with C flags of its own and whole-program optimization, as factorio-in-kpp's final
    # (build.kpp's cFlag, kppc --lto full): its C library is compiled with them (-DPLUS=2 after the
    # library's PLUS=1: tests 43) into a directory of those flags', as bitcode (-flto) that the link
    # optimizes with the program's modules as one; the release build's objects stay where they are,
    # untouched. Every C library is compiled without stack cookies (/GS-, as Factorio's C++ builds).
    $releaseLib = (Get-Item 'build\clib\plus.lib').LastWriteTimeUtc
    $flags = Get-Content 'build\clib\plus\flags.txt' -Raw
    if ($flags -notmatch '/GS-|-fno-stack-protector') { Fail "the C library is compiled with stack cookies: $flags" }
    Set-Content 'build.kpp' ($good -replace '\}\s*$', "  Configuration& whole = project.configuration(`"whole`", `"release`");`n  whole.argument(`"--lto`");`n  whole.argument(`"full`");`n  whole.cFlag(`"-flto`");`n  whole.cFlag(`"-DPLUS=2`");`n}`n")
    $output = Kpp @('game-tests', 'whole', '--no-server', '--set', "INCLUDE_ROOT=$root")
    $tests = if ($code -eq 0) { & '.\build\whole\game-tests.exe' } else { '' }
    $objects = @(Get-ChildItem 'build\clib\flags-*\plus\*.obj' -ErrorAction SilentlyContinue)
    $notBitcode = @($objects | Where-Object { $b = [IO.File]::ReadAllBytes($_.FullName); -not ($b[0] -eq 0x42 -and $b[1] -eq 0x43 -and $b[2] -eq 0xC0 -and $b[3] -eq 0xDE) })
    $link = if (Test-Path 'build\whole\game-tests.exe.link.rsp') { @(Get-Content 'build\whole\game-tests.exe.link.rsp') } else { @() }
    if ($code -ne 0 -or "$tests" -ne 'tests 43') { Fail "kpp game-tests whole (--lto full, cFlag -DPLUS=2): exit $code, printed '$tests'`n$($output -join "`n")" }
    elseif ($objects.Count -eq 0 -or $notBitcode.Count -gt 0) { Fail "the whole configuration's C library objects are not all bitcode (-flto): $($objects.Name -join ', ')" }
    elseif ($link -notcontains '-flto') { Fail "the whole configuration's link is no full LTO: $($link -join ' ')" }
    elseif ((Get-Item 'build\clib\plus.lib').LastWriteTimeUtc -ne $releaseLib) { Fail "the whole configuration's build rebuilt the release build's C library" }
    Set-Content 'build.kpp' $good
    # A C library whose build failed part way under other flags (another toolchain's, here two defines)
    # is compiled again whole under its old ones: plus.c compiled under PLUS=5 and other.c did not, and
    # the old flags' stamp (flags.txt) took plus.c's object as theirs, linked (tests 46).
    Set-Content 'libs\plus\other.c' "#ifdef FAIL_OTHER`n#error other.c fails under FAIL_OTHER`n#endif`nint other_value(void) { return 1; }"
    $output = Kpp @('--release', '--no-server', '--set', "INCLUDE_ROOT=$root")
    if ($code -ne 0) { Fail "kpp --release with other.c: exit $code`n$($output -join "`n")" }
    Set-Content 'build.kpp' ($good -replace 'plus\.define\("PLUS=1"\);', 'plus.define("PLUS=5"); plus.define("FAIL_OTHER");')
    $output = Kpp @('--release', '--no-server', '--set', "INCLUDE_ROOT=$root")
    if ($code -eq 0) { Fail "kpp --release built with other.c failing under FAIL_OTHER`n$($output -join "`n")" }
    Set-Content 'build.kpp' $good
    $output = Kpp @('--release', '--no-server', '--set', "INCLUDE_ROOT=$root")
    $tests = if ($code -eq 0) { & '.\build\release\game-tests.exe' } else { '' }
    if ($code -ne 0 -or "$tests" -ne 'tests 42') { Fail "kpp --release after a build failed under other flags: exit $code, printed '$tests' (46: the failed build's plus.c object linked)`n$($output -join "`n")" }
    Remove-Item 'libs\plus\other.c'
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
}

# kpp libraries (docs/build.md, "kpp libraries"): app uses greet from its libraries\, greet uses text
# next to it there; kpp --test in greet runs its tests and its test roots' with the runner's
# arguments; a minimum version above greet's, a missing library and a minimum kpp above this one
# are errors. greet's option suffix (config::greet::suffix): its default, a configuration's value,
# the command line's, `kpp options` (with greet scoped and text unscoped, design.md 16), and three
# errors: a required option unset, an unknown option, a value not of its type; a library name that is
# no identifier is an error.
if (Run 'libraries')
{
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
    Set-Content 'libraries\greet\src\greet.kpp' "String greeting(StringView name) { return shout(`"hello`") + name + config::greet::suffix; }`ntest greeting`n{`n  check(greeting(`"a`") == `"hello! a`");`n}"
    Set-Content 'libraries\greet\tests\more.kpp' "test greetingTwice`n{`n  check(greeting(`"b`") == `"hello! b`");`n  check(greeting(`"c`") == `"wrong`");`n}"
    $text = "void build(Project& project)`n{`n  Library& text = project.library(`"text`");`n  text.version = `"1.0.0`";`n  text.unscoped = true;`n}"
    Set-Content 'libraries\text\build.kpp' $text
    Set-Content 'libraries\text\src\text.kpp' "String shout(StringView s) { String r = s; r << `"! `"; return r; }"
    $output = Kpp @('--no-server')
    $printed = if ($code -eq 0) { & '.\build\debug\app.exe' } else { '' }
    if ($code -ne 0 -or "$printed" -ne 'hello! app') { Fail "an executable using a library: exit $code, printed '$printed'`n$($output -join "`n")" }
    # Visual Studio's tree: each used library's files under Libraries\NAME (G, 2026-11-05: gorating's
    # src and every library's src were one folder "src"), the program's own under src.
    $output = (Kpp @('--no-server', '--generate-visual-studio-files')) -join "`n"
    $filters = Get-Content 'build\visual-studio\app.vcxproj.filters' -Raw -ErrorAction SilentlyContinue
    foreach ($pair in @(@('src\main.kpp', 'src'), @('libraries\greet\src\greet.kpp', 'Libraries\greet'), @('libraries\text\src\text.kpp', 'Libraries\text')))
    {
      if ($code -ne 0 -or $filters -notmatch ([regex]::Escape("$app\$($pair[0])") + '"><Filter>' + [regex]::Escape($pair[1]) + '</Filter>')) { Fail "Visual Studio's folder of $($pair[0]): not $($pair[1]): exit $code`n$output`n$filters" }
    }
    if ($filters -notmatch 'Filter Include="Libraries"') { Fail "Visual Studio's folder Libraries is missing:`n$filters" }
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
    if ($code -eq 0 -or $output -notmatch 'the library jsno is neither in .*libraries\\jsno nor among kpp''s own libraries') { Fail "a missing library: exit $code`n$output" }
    # kpp's own libraries (2026-11-04: a release brings them): base64, of which the project has no
    # copy, comes from the libraries\ beside stdlib\ (the checkout's, for this kppc); a copy in the
    # project's libraries\ comes first.
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"base64`");`n}"
    Set-Content 'src\main.kpp' "int main()`n{`n  println(base64::base64Encode(`"kpp`"));`n  return 0;`n}"
    $output = (Kpp @('--no-server', '--run')) -join "`n"
    if ($code -ne 0 -or $output -notmatch '(?m)^a3Bw$') { Fail "base64 from kpp's own libraries: exit $code`n$output" }
    New-Item -ItemType Directory -Force 'libraries\base64\src' | Out-Null
    Set-Content 'libraries\base64\build.kpp' "void build(Project& project)`n{`n  Library& base64 = project.library(`"base64`");`n  base64.version = `"9.0.0`";`n  base64.root(`"src`");`n}"
    Set-Content 'libraries\base64\src\Base64.kpp' "String base64Encode(StringView data) { return `"the project's own base64`"; }"
    $output = (Kpp @('--no-server', '--run')) -join "`n"
    if ($code -ne 0 -or $output -notmatch "(?m)^the project's own base64$") { Fail "a project's own base64 before kpp's: exit $code`n$output" }
    cmd /c rd /s /q 'libraries\base64'
  }
  finally { Pop-Location }
}
# A library's system libraries and config declarations, a converted library's shape (a project of
# its own, so that it runs beside the other libraries part).
if (Run 'syslibs')
{
  $sys = Join-Path $work 'syslibs'
  New-Item -ItemType Directory -Force (Join-Path $sys 'src') | Out-Null
  Push-Location $sys
  try
  {
    # A library's system libraries (Library::systemLibrary, 2026-11-04: httpclient's OpenSSL on
    # FreeBSD): sys calls version.dll's GetFileVersionInfoSizeA, which kppc does not link by itself.
    # systemLibrary("version", Platform::Windows) reaches the link of app, which uses sys, and of sys's
    # own tests; one for FreeBSD is left out (no such library here); without them, or with that one
    # for every platform, the link fails.
    New-Item -ItemType Directory -Force 'libraries\sys\src' | Out-Null
    $sysBuild = "void build(Project& project)`n{`n  Library& sys = project.library(`"sys`");`n  sys.version = `"1.0.0`";`n  LINKS`n}"
    Set-Content 'libraries\sys\src\sys.kpp' "extern `"C`" unsigned long GetFileVersionInfoSizeA(const char* name, unsigned long* handle);`nbool versioned() { unsigned long h = 0; return GetFileVersionInfoSizeA(`"kernel32.dll`", &h) > 0; }`ntest kernel32HasAVersion`n{`n  check(versioned());`n}"
    Set-Content 'src\main.kpp' "int main()`n{`n  if (sys::versioned())`n    println(`"versioned`");`n  else`n    println(`"none`");`n  return 0;`n}"
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"sys`");`n}"
    Set-Content 'libraries\sys\build.kpp' ($sysBuild -replace 'LINKS', "sys.systemLibrary(`"version`", Platform::Windows);`n  sys.systemLibrary(`"nosuchlibrary`", Platform::FreeBSD);")
    $output = Kpp @('--no-server', '--run')
    if ($code -ne 0 -or $output[-1] -ne 'versioned') { Fail "a library's system library, kpp --run: exit $code`n$($output -join "`n")" }
    Push-Location 'libraries\sys'
    $output = (Kpp @('--test', '--no-server')) -join "`n"
    Pop-Location
    if ($code -ne 0 -or $output -notmatch '\b1 test, 0 failed') { Fail "a library's system library in its own tests: exit $code`n$output" }
    foreach ($case in @(@('', 'GetFileVersionInfoSizeA'), @("sys.systemLibrary(`"version`");`n  sys.systemLibrary(`"nosuchlibrary`");", 'nosuchlibrary')))
    {
      Set-Content 'libraries\sys\build.kpp' ($sysBuild -replace 'LINKS', $case[0])
      $output = (Kpp @('--no-server')) -join "`n"
      if ($code -eq 0 -or $output -notmatch $case[1]) { Fail "a library's system libraries '$($case[0])': the link did not fail on $($case[1]): exit $code`n$output" }
    }
    # An executable's own, with a platform too (Executable::systemLibrary(NAME, PLATFORM), 2026-11-05):
    # sys names none now; app's version.dll for Windows links, its nosuchlibrary for FreeBSD is left
    # out; for every platform, the link fails on it.
    Set-Content 'libraries\sys\build.kpp' ($sysBuild -replace 'LINKS', '')
    foreach ($case in @(@("app.systemLibrary(`"version`", Platform::Windows);`n  app.systemLibrary(`"nosuchlibrary`", Platform::FreeBSD);", 0),
                        @("app.systemLibrary(`"version`");`n  app.systemLibrary(`"nosuchlibrary`");", 1)))
    {
      Set-Content 'build.kpp' "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"sys`");`n  $($case[0])`n}"
      $output = @(Kpp @('--no-server', '--run'))
      if ($case[1] -eq 0 -and ($code -ne 0 -or $output[-1] -ne 'versioned')) { Fail "an executable's system libraries for a platform, kpp --run: exit $code`n$($output -join "`n")" }
      if ($case[1] -eq 1 -and ($code -eq 0 -or ($output -join "`n") -notmatch 'nosuchlibrary')) { Fail "an executable's system library for every platform: the link did not fail on nosuchlibrary: exit $code`n$($output -join "`n")" }
    }
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"sys`");`n}"
    # `kpp --run` of a program for another platform: an error before the build (kppc --run's too), never a
    # build that then fails to start an ELF. No sysroot needed: nothing is built.
    if (Test-Path 'build\debug\app') { Remove-Item 'build\debug\app' }
    $output = (Kpp @('--no-server', '--target', 'freebsd', '--run')) -join "`n"
    if ($code -ne 2 -or $output -notmatch 'app debug is built for freebsd, which does not run on this machine' -or (Test-Path 'build\debug\app')) { Fail "kpp --target freebsd --run: exit $code`n$output" }
    # Cell::platform (2026-11-05, the game's Linux build, whose configure(cell) gives Windows its cmd
    # steps): the platform the cell is built for, the command line's --target over build.kpp's (a
    # target's argument), which configure(cell) sees and describe.txt keeps. A --target configure(cell)
    # adds itself comes too late for it: an error before the build, never Windows' steps in another's.
    $cellBuild = "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"sys`");`n  app.systemLibrary(`"version`");`n  TARGET`n}`n" +
                 "void configure(Cell& cell)`n{`n  String c = `"echo `";`n  c << StringView(config::platformName(cell.platform)) << `">platform.txt`";`n  cell.target.afterBuild(c);`n  CONFIGURE`n}"
    foreach ($case in @(@('', '', @(), 'windows'), @('', '', @('--target', 'freebsd', '--run'), 'freebsd'),
                        @('app.argument("--target");  app.argument("linux");', '', @('--run'), 'linux'),
                        @('app.argument("--target");  app.argument("linux");', '', @('--target', 'windows'), 'windows'),
                        @('', 'cell.target.argument("--target");  cell.target.argument("freebsd");', @(), 'error')))
    {
      Set-Content 'build.kpp' $cellBuild.Replace('TARGET', $case[0]).Replace('CONFIGURE', $case[1])
      if (Test-Path 'platform.txt') { Remove-Item 'platform.txt' }
      $output = (Kpp (@('--no-server') + $case[2])) -join "`n"
      $described = @(Select-String '^platform ' 'build\kpp\describe.txt' | ForEach-Object { $_.Line }) -join ','
      $what = "cell.platform, build.kpp '$($case[0])$($case[1])', kpp $($case[2] -join ' '): exit $code, describe.txt $described`n$output"
      if ($case[3] -eq 'error')
      {
        if ($code -ne 1 -or $output -notmatch 'app \(debug\) is built for freebsd, but build.kpp''s configure\(cell\) saw cell.platform windows' -or (Test-Path 'platform.txt')) { Fail $what }
      }
      elseif ($described -ne "platform $($case[3]),platform $($case[3])") { Fail $what }
      elseif ($case[3] -eq 'windows' -and ($code -ne 0 -or (Get-Content 'platform.txt') -ne 'windows')) { Fail "$what`nplatform.txt: $(Get-Content 'platform.txt' -ErrorAction SilentlyContinue)" }
    }
    Remove-Item 'platform.txt' -ErrorAction SilentlyContinue
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"sys`");`n}"
    # The other side of the platform: `kpp --target freebsd` links what a library keeps for FreeBSD and
    # leaves out what it keeps for Linux. bsd calls nothing of Windows; with
    # systemLibrary("nosuchlibrary", Platform::FreeBSD) the link fails on it, with Platform::Linux it
    # links. Needs FreeBSD's sysroot beside kppc (tools\sysroot\freebsd.ps1), skipped without it.
    if (-not (Test-Path (Join-Path (Split-Path $Kppc) 'sysroots\freebsd-x86_64\usr\lib\libc.a'))) { "skip project: a library's system library for freebsd (no sysroot beside kppc: tools\sysroot\freebsd.ps1)" }
    else
    {
      New-Item -ItemType Directory -Force 'libraries\bsd\src' | Out-Null
      Set-Content 'libraries\bsd\src\bsd.kpp' "int answer() { return 42; }"
      Set-Content 'src\main.kpp' "int main()`n{`n  println(bsd::answer());`n  return 0;`n}"
      Set-Content 'build.kpp' "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"bsd`");`n}"
      foreach ($platform in @('FreeBSD', 'Linux'))
      {
        Set-Content 'libraries\bsd\build.kpp' "void build(Project& project)`n{`n  Library& bsd = project.library(`"bsd`");`n  bsd.version = `"1.0.0`";`n  bsd.systemLibrary(`"nosuchlibrary`", Platform::$platform);`n}"
        if (Test-Path 'build\debug\app') { Remove-Item 'build\debug\app' }
        $output = (Kpp @('--no-server', '--target', 'freebsd')) -join "`n"
        if ($platform -eq 'FreeBSD' -and ($code -eq 0 -or $output -notmatch 'nosuchlibrary')) { Fail "a library's system library for FreeBSD, kpp --target freebsd: the link did not fail on nosuchlibrary: exit $code`n$output" }
        if ($platform -eq 'Linux' -and ($code -ne 0 -or $output -match 'nosuchlibrary' -or -not (Test-Path 'build\debug\app'))) { Fail "a library's system library for Linux, kpp --target freebsd: not left out: exit $code`n$output" }
      }
    }
    # A library's config:: constants declared in its source (design.md, "config:: defaults in source",
    # 2026-11-04) are its options: the default where nothing sets it, use().config's value, kpp
    # --config's; `kpp options` lists them with the declaration's file and line; a required one nothing
    # sets and a name option() declares too are errors.
    New-Item -ItemType Directory -Force 'libraries\deflate\src' | Out-Null
    Set-Content 'libraries\deflate\build.kpp' "void build(Project& project)`n{`n  Library& deflate = project.library(`"deflate`");`n  deflate.version = `"1.0.0`";`n}"
    Set-Content 'libraries\deflate\src\deflate.kpp' "// How hard it tries.`nconfig i32 level = 6;`nconfig bool checked;`nint levelOf() { return config::deflate::level; }"
    Set-Content 'src\main.kpp' "int main()`n{`n  println(deflate::levelOf(), `" `", config::deflate::checked);`n  return 0;`n}"
    $deflateApp = "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  USE`n}"
    Set-Content 'build.kpp' ($deflateApp -replace 'USE', 'app.use("deflate");')
    $output = (Kpp @('--no-server')) -join "`n"
    if ($code -eq 0 -or $output -notmatch 'needs its option checked \(bool') { Fail "a library's required config declaration nothing sets: exit $code`n$output" }
    Set-Content 'build.kpp' ($deflateApp -replace 'USE', 'app.use("deflate").config("checked", true);')
    $output = Kpp @('--no-server', '--run')
    if ($code -ne 0 -or $output[-1] -ne '6 true') { Fail "a library's config declarations, its default and use().config's value: exit $code`n$($output -join "`n")" }
    $output = Kpp @('--no-server', '--config', 'deflate::level=9', '--run')
    if ($code -ne 0 -or $output[-1] -ne '9 true') { Fail "a library's config declaration, kpp --config deflate::level=9: exit $code`n$($output -join "`n")" }
    # Its default edited (one byte, the file's size the same): kpp reads it again, not its cache of the
    # declarations (build\kpp\uses\deflate\config-deflate.txt, kept by each file's time and size).
    Set-Content 'libraries\deflate\src\deflate.kpp' "// How hard it tries.`nconfig i32 level = 7;`nconfig bool checked;`nint levelOf() { return config::deflate::level; }"
    $output = Kpp @('--no-server', '--run')
    if ($code -ne 0 -or $output[-1] -ne '7 true') { Fail "a library's config default edited, kpp --run: exit $code`n$($output -join "`n")" }
    Set-Content 'libraries\deflate\src\deflate.kpp' "// How hard it tries.`nconfig i32 level = 6;`nconfig bool checked;`nint levelOf() { return config::deflate::level; }"
    $output = (Kpp @('options')) -join "`n"
    if ($code -ne 0 -or $output -notmatch 'deflate::level = 6  i32, default 6; set by the default' -or $output -notmatch 'How hard it tries\. \(.*deflate\.kpp:2\)') { Fail "kpp options, a library's config declaration: exit $code`n$output" }
    Set-Content 'libraries\deflate\build.kpp' "void build(Project& project)`n{`n  Library& deflate = project.library(`"deflate`");`n  deflate.version = `"1.0.0`";`n  deflate.option(`"level`", 3, `"Its level`");`n}"
    $output = (Kpp @('--no-server')) -join "`n"
    if ($code -eq 0 -or $output -notmatch 'deflate\.kpp:2: error: config::deflate::level is declared here and by build\.kpp''s option') { Fail "a library's config declared in source and by option(): exit $code`n$output" }
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
}
# Targets x configurations (docs/build.md, its section on them): two targets in debug, release
# and fastdebug (debug's constants, optimized: no incremental link's .ilk), config constants set for
# the project, a configuration, a target and one cell (configure), the command line's --config over
# them all, `kpp TARGET CONFIGURATION` in either order, the Visual Studio solution's configurations,
# and three errors: a base that is not declared, a target named as a configuration, an unknown name.
if (Run 'matrix')
{
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
}

# Outside a project: an unknown option of kppc is the same error. A copy of kppc elsewhere (an
# installation) prints the version and commit it was built from.
if (Run 'outside')
{
  $output = (Kpp @('--frobnicate', $work)) -join "`n"
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
      $output -notmatch '(?m)^toolchain: none: this kpp has no toolchain of its own' -or $output -notmatch '(?m)^libraries: .*\(none\)$') { Fail "an installation's kpp --version:`n$output" }
}

# `kpp --run` in a project: a path ending in .kpp right after --run, with no target or configuration
# named before it, is a one-file program, run as outside a project (it ran the project with the file
# as an argument: the same command, another program, 2026-11-06), and one that is not there is an
# error. Every other argument goes to the project's program: a file or directory of another kind, a
# .kpp after the first argument, and any .kpp once a name has chosen the cell.
if (Run 'runfile')
{
  $project = Join-Path $work 'runfile'
  New-Item -ItemType Directory -Force "$project\src", "$project\sub", "$project\data" | Out-Null
  Push-Location $project
  try
  {
    $body = '(int argc, char** argv) { print("NAME"); for (int i = 1; i < argc; ++i) print(" ", StringView(argv[i])); println(); return CODE; }'
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n}"
    Set-Content 'src\main.kpp' ('int main' + $body.Replace('NAME', 'project').Replace('CODE', '0'))
    Set-Content 'hello.kpp' ('int main' + $body.Replace('NAME', 'file').Replace('CODE', '7'))
    Set-Content 'input.txt' 'data'
    foreach ($case in @(@('.', @('--run', 'hello.kpp', 'a', 'b'), 'file a b', 7),
                        @('.', @('--release', '--run', 'hello.kpp', 'a'), 'file a', 7),
                        @('sub', @('--run', '..\hello.kpp', 'a', 'b'), 'file a b', 7),
                        @('.', @('--run', 'input.txt', 'x'), 'project input.txt x', 0),
                        @('.', @('--run', 'data', 'x'), 'project data x', 0),
                        @('.', @('--run', 'nothing.txt'), 'project nothing.txt', 0),
                        @('.', @('--run', 'a', 'hello.kpp'), 'project a hello.kpp', 0),
                        @('.', @('app', '--run', 'hello.kpp', 'a'), 'project hello.kpp a', 0),
                        @('.', @('debug', '--run', 'hello.kpp'), 'project hello.kpp', 0),
                        @('.', @('--run'), 'project', 0)))
    {
      Push-Location $case[0]
      $output = @(Kpp (@('--no-server') + $case[1]))
      Pop-Location
      if ($code -ne $case[3] -or $output[-1] -ne $case[2]) { Fail "kpp $($case[1] -join ' ') in $($case[0]) of a project: exit $code, not $($case[3]), and not '$($case[2])' last`n$($output -join "`n")" }
    }
    # (through a compile server, as a command line without --no-server builds; StopTestServers ends it)
    $output = @(Kpp @('--run', 'hello.kpp', 'a', 'b'))
    if ($code -ne 7 -or $output[-1] -ne 'file a b') { Fail "kpp --run hello.kpp a b through a compile server: exit $code`n$($output -join "`n")" }
    $output = (Kpp @('--no-server', '--run', 'missing.kpp', 'a')) -join "`n"
    if ($code -ne 2 -or $output -notmatch 'missing\.kpp not found') { Fail "kpp --run missing.kpp in a project: exit $code`n$output" }
  }
  finally { Pop-Location }
}

# A library in a directory this user cannot write (2026-11-05: an installed kpp's libraries, under
# C:\Program Files\kpp, where describe() wrote the library's description cache, failed and said
# nothing; gorating's `kpp --generate-visual-studio-files` exited 1 silently): its description is kept
# in the using project's build\kpp\uses\NAME, and the program builds and runs. A project whose own
# build directory cannot be written is an error that names it. The directories are denied this user's
# writes by an ACL entry (writing data, adding files and directories, attributes), reset in finally.
if (Run 'readonly')
{
  $readonly = Join-Path $work 'readonly'
  if (Test-Path $readonly) { icacls $readonly /reset /T /Q | Out-Null; cmd /c rd /s /q $readonly }
  New-Item -ItemType Directory -Force "$readonly\app\src", "$readonly\lib\src" | Out-Null
  Push-Location "$readonly\app"
  try
  {
    Set-Content '..\lib\build.kpp' "void build(Project& project)`n{`n  Library& lib = project.library(`"lib`");`n  lib.version = `"1.0.0`";`n}"
    Set-Content '..\lib\src\lib.kpp' "config i32 answer = 42;`nint answer() { return config::lib::answer; }"
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.use(`"lib`", local(`"../lib`"));`n}"
    Set-Content 'src\main.kpp' "int main() { println(lib::answer()); return 0; }"
    $me = "$env:USERDOMAIN\$env:USERNAME"
    icacls "$readonly\lib" /deny "${me}:(OI)(CI)(WD,AD,WEA,WA)" /Q | Out-Null
    $writable = try { Set-Content '..\lib\probe.txt' 'x' -ErrorAction Stop; $true } catch { $false }
    if ($writable) { Fail "a write-denied library directory: this user can still write it (icacls)" }
    $output = @(Kpp @('--no-server', '--run'))
    if ($code -ne 0 -or $output[-1] -ne '42' -or -not (Test-Path 'build\kpp\uses\lib\describe.txt') -or (Test-Path '..\lib\build')) { Fail "a library in a read-only directory, kpp --run: exit $code`n$($output -join "`n")" }
    cmd /c rd /s /q build
    New-Item -ItemType Directory 'build' | Out-Null
    icacls "$readonly\app\build" /deny "${me}:(OI)(CI)(WD,AD,WEA,WA)" /Q | Out-Null
    $output = (Kpp @('--no-server')) -join "`n"
    if ($code -ne 1 -or $output -notmatch [regex]::Escape("error: kpp cannot make $readonly\app\build\kpp for the description of $readonly\app\build.kpp")) { Fail "a project whose build directory cannot be written: exit $code`n$output" }
  }
  finally
  {
    Pop-Location
    icacls $readonly /reset /T /Q | Out-Null
  }
}

# A file the link takes besides kppc's objects, changed alone (2026-11-05, Executable::natvis): kpp
# links again, the compile server having replayed the unchanged build. The .natvis edited between
# two builds through the server: the PDB holds the new rule, never the old one only.
if (Run 'linkinputs')
{
  $inputs = Join-Path $work 'linkinputs'
  New-Item -ItemType Directory -Force "$inputs\src" | Out-Null
  Push-Location $inputs
  try
  {
    Set-Content 'src\main.kpp' "struct Point`n{`n  i32 x = 1;`n};`nint main()`n{`n  Point p;`n  println(p.x);`n  return 0;`n}"
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  Executable& app = project.executable(`"app`", {`"src`"});`n  app.natvis(`"point.natvis`");`n  app.argument(`"--debug-info`");`n}"
    $natvis = '<?xml version="1.0" encoding="utf-8"?><AutoVisualizer xmlns="http://schemas.microsoft.com/vstudio/debugger/natvis/2010"><Type Name="Point"><DisplayString>RULE {x}</DisplayString></Type></AutoVisualizer>'
    foreach ($rule in 'firstrule', 'secondrule')
    {
      Set-Content 'point.natvis' $natvis.Replace('RULE', $rule)
      $output = (Kpp @('debug')) -join "`n"
      $inPdb = (Test-Path 'build\debug\app.pdb') -and (Select-String -Path 'build\debug\app.pdb' -Pattern $rule -SimpleMatch -Quiet)
      if ($code -ne 0 -or -not $inPdb) { Fail "a .natvis edited alone, through the compile server: the PDB has no $rule`: exit $code`n$output" }
      Kpp @('debug') | Out-Null # (nothing changed: the server keeps the build, which the next one replays)
    }
    Kpp @('--stop-server') | Out-Null
  }
  finally { Pop-Location }
}

# kpp measure (docs/build.md, "kpp measure"): build.kpp's edit made, built, put back and built, the
# clean build and each executable's whole build, in two rounds; the table's rows, each the average
# of its rounds; the file as committed afterwards. Then a file with changes of its own: refused, nothing built or changed.
if (Run 'measure')
{
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
}
# The paths a program's text holds (an assertion's message, a CallerLocation's file): a release build
# writes the project's relative to its directory and the standard library's under stdlib/
# (--file-prefix-map), so that its binary holds no path of this machine; a debug build keeps them
# whole, as its IDE and debugger open them. A C library's __FILE__ is relative to the project in every
# configuration (clang's -ffile-prefix-map; the configurations share the library's objects).
if (Run 'paths')
{
  $paths = Join-Path $work 'paths'
  New-Item -ItemType Directory -Force "$paths\src" | Out-Null
  Push-Location $paths
  try
  {
    New-Item -ItemType Directory -Force "$paths\libs\where", "$paths\include" | Out-Null
    Set-Content 'include\where.h' 'const char* c_file(void);'
    Set-Content 'libs\where\where.c' "#include `"where.h`"`nconst char* c_file(void) { return __FILE__; }"
    Set-Content 'build.kpp' "void build(Project& project)`n{`n  CLibrary& where = project.cLibrary(`"where`");`n  where.source(`"libs/where`");`n  where.include(`"include`");`n  Executable& paths = project.executable(`"paths`", {`"src`"});`n  paths.link(`"where`");`n  paths.cInclude(`"include`");`n}"
    Set-Content 'src\main.kpp' "extern `"C`" import `"where.h`";`nvoid where(CallerLocation here = caller) { println(StringView(here.file_name())); }`nint main()`n{`n  println(format(`"{}`", 1)); // (Format.kpp's release_assert in the program)`n  println(StringView(c_file()));`n  where();`n  return 0;`n}"
    $root = (Get-Location).Path
    $stdlib = [IO.Path]::GetFullPath((Join-Path (Split-Path $Kppc) '..\..\stdlib'))
    $output = @(Kpp @('--release', '--no-server', '--run'))
    $printed = if ($output.Count) { $output[-1] } else { '' }
    if ($code -ne 0 -or $printed -ne 'src/main.kpp') { Fail "a release build's CallerLocation file is '$printed', not src/main.kpp`n$($output -join "`n")" }
    $cFile = if ($output.Count -ge 2) { $output[-2] } else { '' }
    if ($cFile -ne 'libs\where\where.c') { Fail "a C library's __FILE__ is '$cFile', not libs\where\where.c" }
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $paths 'build\release\paths.exe')))
    foreach ($held in $root, $stdlib)
    {
      if ($text.IndexOf($held, [StringComparison]::OrdinalIgnoreCase) -ge 0) { Fail "the release build's executable holds the path $held" }
    }
    # A release build has debug information by default, in a file of its own beside the executable,
    # for crash reports (kovarex 2026-11-07, docs/build.md question 1): the PDB, which the executable
    # names without its directory (/PDBALTPATH:%_PDB%), so that the check above holds with it.
    if (-not (Test-Path 'build\release\paths.pdb')) { Fail "the release build left no build\release\paths.pdb" }
    elseif ($text.IndexOf('paths.pdb', [StringComparison]::OrdinalIgnoreCase) -lt 0) { Fail "the release build's executable does not name paths.pdb" }
    $output = @(Kpp @('--debug', '--no-server', '--run'))
    $printed = if ($output.Count) { $output[-1] } else { '' }
    if ($code -ne 0 -or $printed -ne (Join-Path $root 'src\main.kpp')) { Fail "a debug build's CallerLocation file is '$printed', not $(Join-Path $root 'src\main.kpp')`n$($output -join "`n")" }
    # describe.txt's keys are one table (Project/DescribeFile.cpp, 2026-11-05): a line kpp does not know
    # (projectlib and kppc of different releases) is an error naming it, never a setting dropped.
    Add-Content 'build\kpp\describe.txt' 'frobnicate 1'
    $output = (Kpp @('--debug', '--no-server')) -join "`n"
    if ($code -eq 0 -or $output -notmatch 'describe\.txt line \d+: `frobnicate` is no key kpp knows') { Fail "an unknown key in describe.txt: exit $code`n$output" }
    Remove-Item 'build\kpp\describe.key'
  }
  finally { Pop-Location }
}
exit $failed
