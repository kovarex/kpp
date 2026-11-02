# The compile server must build exactly what a new kppc process builds (docs/compile-speed.md,
# "The compile server"). A copy of tests\incremental\ is edited step by step (a function body, a
# signature, a return type a data member is declared with, a class's layout (built from the kept program), a file added, the file removed); after each edit it is built through
# one long-lived server and by a new process, and the IR, stdout and stderr (timings masked) must
# be identical, and the program the server built must print what the step expects. run_tests.cmd
# runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\incremental'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$src = Join-Path $work 'src'
New-Item -ItemType Directory -Force $src | Out-Null
Copy-Item "$PSScriptRoot\incremental\*.kpp" $src
# A scoped library of the program (design.md 16), its names written ilib::name: the last steps edit it.
$lib = Join-Path $work 'lib'
New-Item -ItemType Directory -Force $lib | Out-Null
Set-Content (Join-Path $lib 'ilib.kpp') "int libValue() { return 1; }`nstruct LibBox { int v = 2; int get() const { return v + libValue(); } };"
$libRoot = @('--library-root', 'ilib', $lib) # (before the sources: the server's log names a build by its last argument)
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0
$started = Get-Date

function Normalize([string[]]$lines)
{
  # (and without the interned types' count: a cache, larger in a server that built earlier versions)
  $lines | Where-Object { $_ -notmatch '^check:\s+[0-9]+ distinct types' } | ForEach-Object { $_ -replace '[0-9]+\.[0-9]+ s\b', '_ s' -replace '\([0-9.]+ [MG]?B/s[^)]*\)', '(_)' -replace '\([0-9.]+ M lines/s\)', '(_)' -replace '\(([0-9]+) requests, ([0-9]+) waits\)', '(_ requests, _ waits)' }
}

function Build([string[]]$arguments, [string]$label)
{
  $out = Join-Path $work "$label.out.txt"
  $err = Join-Path $work "$label.err.txt"
  # Not Start-Process -Wait: it waits for the client's whole process tree, the server included.
  $ErrorActionPreference = 'Continue'
  & $Kppc @arguments > $out 2> $err
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  return [pscustomobject]@{ exit = $code; out = @(Normalize (Get-Content $out)); err = @(Normalize (Get-Content $err)) }
}

# This run's servers' log lines since a time. Other worktrees run this test at the same time, with
# servers of their own logging to the same directory: ours are those whose builds name $src last.
function ServerLog([datetime]$since)
{
  $lines = @(Get-ChildItem (Join-Path $env:TEMP 'kppc-server\*.log') | Where-Object { $_.LastWriteTime -ge $since } |
    ForEach-Object { Get-Content $_.FullName } | Where-Object { $_.Length -gt 23 -and [datetime]::ParseExact($_.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff', $null) -ge $since })
  $ids = @($lines | Where-Object { $_.Contains("the last $src") } | ForEach-Object { if ($_ -match '\[(\d+)\]') { $Matches[1] } } | Sort-Object -Unique)
  return @($lines | Where-Object { $_ -match '\[(\d+)\]' -and $ids -contains $Matches[1] })
}

function Edit([string]$file, [string]$old, [string]$new)
{
  $path = Join-Path $src $file
  $text = [IO.File]::ReadAllText($path)
  if (-not $text.Contains($old)) { throw "incremental.ps1: '$old' not found in $file" }
  [IO.File]::WriteAllText($path, $text.Replace($old, $new))
}

$steps = @(
  @{ name = 'first build'; expect = '6 40 40 3 8 10 6 5 8 40 3 8 6 40 13 19 44 111 12 12 7'; edit = {} },
  @{ name = 'body edit'; expect = '6 44 44 3 8 10 6 5 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'return v * 10;' 'return v * 11;' } },
  @{ name = 'trivial getter made two statements (callers in another file)'; expect = '7 44 44 3 8 10 6 5 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int area() const { return width * height; }' 'int area() const { int a = height * width; return a + 1; }' } },
  @{ name = 'template body edit (its instance checked again)'; expect = '7 44 44 3 8 11 6 5 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'T r = v + v; return r;' 'T r = v + v; return r + 1;' } },
  @{ name = 'class template member body edit (its instance checked again)'; expect = '7 44 44 3 8 11 7 5 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'T r = v * 3; return r;' 'T r = v * 3; return r + 1;' } },
  @{ name = 'constexpr body edit (what the evaluator ran it for checked again)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int r = 5; return r;' 'int r = 6; return r;' } },
  @{ name = 'constexpr body edit failing a static_assert in another file'; expect = '(build exit 1)'; edit = { Edit 'shapes.kpp' 'int r = 6; return r;' 'int r = 12; return r;' } },
  @{ name = 'constexpr body edit back'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int r = 12; return r;' 'int r = 6; return r;' } },
  @{ name = 'a body edit of a function that is no constexpr, which a static_assert in another file runs (it fails)'; expect = '(build exit 1)'; codegenFull = 'a static_assert \(main.kpp\) read a changed name'; edit = { Edit 'shapes.kpp' 'int plainLimit() { int r = 7; return r; }' 'int plainLimit() { int r = 7; return r + 5; }' } },
  @{ name = 'that body edit back'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; codegenFull = 'a static_assert \(main.kpp\) read a changed name'; edit = { Edit 'shapes.kpp' 'int plainLimit() { int r = 7; return r + 5; }' 'int plainLimit() { int r = 7; return r; }' } },
  @{ name = 'an undeclared name typed in a body (a checker error: the file is walked again, its walk''s errors taken back)'; expect = '(build exit 1)'; edit = { Edit 'shapes.kpp' 'int scale(int v) { return v * 11; }' 'int scale(int v) { undeclaredName = 1; return v * 11; }' } },
  @{ name = 'the error fixed'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int scale(int v) { undeclaredName = 1; return v * 11; }' 'int scale(int v) { return v * 11; }' } },
  @{ name = 'a line half typed in a body (it does not parse: no code generated), and a body edit in another file'; expect = '(build exit 1)'; edit = { Edit 'shapes.kpp' 'int scale(int v) { return v * 11; }' 'int scale(int v) { v. return v * 11; }'; Edit 'lambdas.kpp' 'return stepFunctions[0](3) + stepFunctions[1](3); }' 'return stepFunctions[0](3) + stepFunctions[1](3) + 1; }' } },
  @{ name = 'the line taken out (codegen emits the other file''s edit too)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 13 7'; edit = { Edit 'shapes.kpp' 'int scale(int v) { v. return v * 11; }' 'int scale(int v) { return v * 11; }' } },
  @{ name = 'that body edit back'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' 'return stepFunctions[0](3) + stepFunctions[1](3) + 1; }' 'return stepFunctions[0](3) + stepFunctions[1](3); }' } },
  # A lambda's closure is named by its file's number, not its place (an editor's typing moves them all the time):
  # a line added above one, in its function or above that function, emits what changed alone; so does the fix of
  # a codegen error typed above it (the error's build stopped before the lambda; F's VS Code test, 11.3 s, 2026-11-02).
  @{ name = 'a line added above a lambda in its function (it moves; its closure keeps its name)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' '  auto add = [base]' "  int pad = 0;`r`n  auto add = [base]" } },
  @{ name = 'a codegen error typed above that lambda'; expect = '(build exit 1)'; edit = { Edit 'lambdas.kpp' '  int pad = 0;' '  int* pad = 5;' } },
  @{ name = 'the error taken out (the lambda back at its place)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' "  int* pad = 5;`r`n" '' } },
  # A generic lambda's call operator instance belongs to its function's walk: an edit of the function makes it
  # anew and drops the old (not a build from scratch: "the new code instantiates what the old did not").
  @{ name = 'a line added above a generic lambda in its function (its call operator''s instance made anew)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' '  auto scaled = [](auto v)' "  int pad2 = 0;`r`n  auto scaled = [](auto v)" } },
  @{ name = 'that line taken out'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' "  int pad2 = 0;`r`n" '' } },
  # A lambda passed to a function template: the new closure makes the template's instance for it, the old
  # instance nobody asks for any more goes (new instances in general, not a build from scratch).
  @{ name = 'a line added above a lambda passed to a template (its instance made anew, the old dropped)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' '  auto twice = [](int v)' "  int pad3 = 0;`r`n  auto twice = [](int v)" } },
  @{ name = 'that line taken out again'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' "  int pad3 = 0;`r`n" '' } },
  @{ name = 'a line added above a lambda held ten holders deep (the instances for its old closure dropped)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' '  auto zero = [](int v)' "  int pad4 = 0;`r`n  auto zero = [](int v)" } },
  @{ name = 'that line taken out as well'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' "  int pad4 = 0;`r`n" '' } },
  @{ name = 'a body edit above a kept function with a lambda (which moves)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' 'int firstHelper() { int r = 2; return r; }' 'int firstHelper() { int r = 2; r = r + 0; return r; }' } },
  @{ name = 'that body edit back'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'lambdas.kpp' 'int firstHelper() { int r = 2; r = r + 0; return r; }' 'int firstHelper() { int r = 2; return r; }' } },
  @{ name = 'a lambda added before another in its file (which moves and is renumbered)'; expect = '7 44 44 3 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; codegenFull = 'a local type moved'; edit = { Edit 'lambdas.kpp' 'int firstHelper() { int r = 2; return r; }' 'int firstHelper() { auto two = []() { return 2; }; int r = two(); return r; }' } },
  @{ name = 'signature edit'; expect = '7 44 44 103 8 11 7 6 8 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int combine(int a, int b) { return a + b; }' 'int combine(int a, int b, int c = 100) { return a + b + c; }' } },
  @{ name = 'return type edit (a data member is declared with it)'; expect = '7 44 44 103 8 11 7 6 16 44 3 8 6 44 13 19 44 111 12 12 7'; full = 'a declaration typed by its walk'; edit = { Edit 'shapes.kpp' 'int measure()' 'long long measure()' } },
  @{ name = 'overload added'; expect = '7 44 7 103 8 11 7 6 16 7 3 8 6 7 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int scale(int v) { return v * 11; }' "int scale(int v) { return v * 11; }`nint scale(double v) { return 7; }" } },
  @{ name = 'overload removed'; expect = '7 44 44 103 8 11 7 6 16 44 3 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' "`nint scale(double v) { return 7; }" '' } },
  @{ name = 'signature edit of a member another class shares the name of'; expect = '7 44 44 103 8 11 7 6 16 44 5 8 6 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'struct Gauge { int reading() const { return 3; } };' 'struct Gauge { int reading(int scale = 1) const { return 5 * scale; } };' } },
  @{ name = 'signature edit read by an instance codegen made (dropped, made anew)'; expect = '7 44 44 103 8 11 7 6 16 44 5 8 15 44 13 19 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int base() const { return 2; }' 'int base(int k = 0) const { return 5 + k; }' } },
  @{ name = 'return type edit failing a static_assert at namespace scope in another file'; expect = '(build exit 1)'; codegenFull = 'a static_assert \(main.kpp\) read a changed name'; edit = { Edit 'shapes.kpp' 'int unitWidth() { return 1; }' 'long long unitWidth() { return 1; }' } },
  @{ name = 'return type edit back'; expect = '7 44 44 103 8 11 7 6 16 44 5 8 15 44 13 19 44 111 12 12 7'; codegenFull = 'a static_assert \(main.kpp\) read a changed name'; edit = { Edit 'shapes.kpp' 'long long unitWidth() { return 1; }' 'int unitWidth() { return 1; }' } },
  @{ name = 'layout edit'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 44 111 12 12 7'; edit = { Edit 'shapes.kpp' '  int height = 3;' "  int height = 3;`n  int depth = 4;" } },
  @{ name = 'data member added that a trait in another file reads (its static_assert fails)'; expect = '(build exit 1)'; codegenFull = 'a static_assert \(main.kpp\) read a changed name'; edit = { Edit 'shapes.kpp' 'struct Plain { int a = 0; };' 'struct Plain { int a = 0; String s; };' } },
  @{ name = 'data member removed again'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 44 111 12 12 7'; codegenFull = 'a static_assert \(main.kpp\) read a changed name'; edit = { Edit 'shapes.kpp' 'struct Plain { int a = 0; String s; };' 'struct Plain { int a = 0; };' } },
  @{ name = 'data member added to a class with a declaration of several declarators'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'struct Route { Route(); int start = 1, goal = 2; };' 'struct Route { Route(); int start = 1, goal = 2; int cost = 3; };' } },
  @{ name = 'a declaration of several data members added'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 44 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int cost = 3; };' 'int cost = 3; int x = 4, y = 5; };' } },
  @{ name = 'both removed again'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 44 111 12 12 7'; edit = { Edit 'shapes.kpp' ' int cost = 3; int x = 4, y = 5;' '' } },
  @{ name = 'return type edit read only by a deduced auto local in another file'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 84 111 12 12 7'; edit = { Edit 'shapes.kpp' 'int unitDepth()' 'long long unitDepth()' } },
  @{ name = 'return type edit read only by a local whose class template arguments are deduced'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 88 111 12 12 7'; full = 'an old body.s instance nobody else asks for'; edit = { Edit 'shapes.kpp' 'int unitHeight()' 'long long unitHeight()' } },
  @{ name = 'an overload added that calls a recursive function (both read its name)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 88 111 12 12 7'; edit = { Edit 'locals.kpp' 'int depth(int n) { return n > 0 ? depth(n - 1) + 1 : 0; }' "int depth(int n) { return n > 0 ? depth(n - 1) + 1 : 0; }`nint depth(double v) { return depth(int(v)) + 100; }" } },
  @{ name = 'that overload removed'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 88 111 12 12 7'; edit = { Edit 'locals.kpp' "`nint depth(double v) { return depth(int(v)) + 100; }" '' } },
  @{ name = 'a base member brought in by a using-declaration: signature edit (its caller in another file)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 88 311 12 12 7'; codegenFull = 'a using-declaration in UsingDerived'; edit = { Edit 'members.kpp' '  int f(int v) { return 1; }' '  int f(int v, int w = 0) { return 3; }' } },
  @{ name = 'a base member called through a derived class and a template: signature edit'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 88 344 12 12 7'; edit = { Edit 'members.kpp' '  int m(int v) { return 1; }' '  int m(int v, int w = 7) { return 4; }' } },
  @{ name = 'a global''s initializer reads an edited function (its constructor emitted again)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 88 344 15 12 7'; edit = { Edit 'shapes.kpp' 'int offsetOf(int v) { return v + 1; }' 'int offsetOf(int v) { return v + 2; }' } },
  @{ name = 'a codegen error typed into a body (codegen keeps its session across it)'; expect = '(build exit 1)'; edit = { Edit 'holders.kpp' 'Frame f; return' 'Frame f; int* bad = 5; return' } },
  @{ name = 'another body edited while the error stays'; expect = '(build exit 1)'; edit = { Edit 'locals.kpp' '+ depth(0); }' '+ depth(1); }' } },
  @{ name = 'the error fixed (the modules changed meanwhile assembled)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Edit 'holders.kpp' 'Frame f; int* bad = 5; return' 'Frame f; return' } },
  @{ name = 'a call of a function declared nowhere typed into a body (the checker''s error; codegen''s failure on the call follows from it, not shown)'; expect = '(build exit 1)'; edit = { Edit 'locals.kpp' '+ depth(1); }' '+ depth(1) + lateHelper(0); }' } },
  @{ name = 'that function defined in a new file (no edit of the body that calls it)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Set-Content (Join-Path $src 'late.kpp') 'int lateHelper(int v) { return v; }' } },
  @{ name = 'the call and the file taken out'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Edit 'locals.kpp' '+ depth(1) + lateHelper(0); }' '+ depth(1); }'; Remove-Item (Join-Path $src 'late.kpp') } },
  @{ name = 'new file'; expect = '7 44 1 103 12 11 7 6 16 1 5 8 15 1 17 23 89 344 15 12 7'; edit = { Set-Content (Join-Path $src 'extra.kpp') 'int scale(double v) { return 1; }' } },
  @{ name = 'file removed'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Remove-Item (Join-Path $src 'extra.kpp') } },
  # A name private to its file (design.md 15): the file's own overload, which no other file sees,
  # made global (every file's calls of scale(double) take it) and private again.
  @{ name = 'a file with a private overload added (its own file''s alone)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Set-Content (Join-Path $src 'privates.kpp') "private int scale(double v) { return 9; }`nint privateUser() { return scale(1.5); }" } },
  @{ name = 'the private overload made global'; expect = '7 44 9 103 12 11 7 6 16 9 5 8 15 9 17 23 89 344 15 12 7'; full = 'a change outside function bodies'; edit = { Edit 'privates.kpp' 'private int scale(double v)' 'int scale(double v)' } },
  @{ name = 'made private again'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; full = 'a change outside function bodies'; edit = { Edit 'privates.kpp' 'int scale(double v)' 'private int scale(double v)' } },
  @{ name = 'the file of private names removed'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Remove-Item (Join-Path $src 'privates.kpp') } },
  @{ name = 'a file naming a scoped library''s names added'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Set-Content (Join-Path $src 'libuser.kpp') "int libUser() { ilib::LibBox b; return b.get() + ilib::libValue(); }" } },
  @{ name = 'a scoped library''s function body edited'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; edit = { Edit '..\lib\ilib.kpp' 'int libValue() { return 1; }' 'int libValue() { return 4; }' } },
  @{ name = 'a top-level function added to the scoped library (a whole index: its namespace)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; full = 'an added or removed function is a top-level one of a scoped library'; edit = { Edit '..\lib\ilib.kpp' 'int libValue() { return 4; }' "int libValue() { return 4; }`nint libThird() { return 3; }" } },
  @{ name = 'that function made private to its library file'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 7'; full = 'a change outside function bodies'; edit = { Edit '..\lib\ilib.kpp' 'int libThird()' 'private int libThird()' } },
  @{ name = 'a constexpr member a constant global''s initializer runs (codegen ran it: its copy dropped and made anew; codegen in full: the global is a constant, no constructor)'; expect = '7 44 44 103 12 11 7 6 16 44 5 8 15 44 17 23 89 344 15 12 8'; codegenFull = 'the global stepOrigin read an edited function'; edit = { Edit 'shapes.kpp' 'return Step(this->v + o.v); }' 'return Step(this->v + o.v + 1); }' } }
)
$n = 0
foreach ($step in $steps)
{
  $n++
  $stepStart = Get-Date
  & $step.edit
  $ir = Join-Path $work "server.$n.ll"
  $fresh = Join-Path $work "fresh.$n.ll"
  $exe = Join-Path $work 'server.exe'
  # The server's IR is written to one path every step (its arguments, so its server, stay the same).
  $s = Build ($libRoot + @('--server', '--verify-incremental', '--emit-llvm', (Join-Path $work 'server.ll'), $src)) "server.$n"
  Copy-Item (Join-Path $work 'server.ll') $ir
  $f = Build ($libRoot + @('--emit-llvm', (Join-Path $work 'fresh.ll'), $src)) "fresh.$n"
  Copy-Item (Join-Path $work 'fresh.ll') $fresh
  $ok = $true
  if ($s.exit -ne $f.exit) { $ok = $false; "FAIL incremental ${n} ($($step.name)): exit $($s.exit) through the server, $($f.exit) fresh" }
  foreach ($part in 'out', 'err')
  {
    $d = Compare-Object $f.$part $s.$part -SyncWindow 0
    if ($d) { $ok = $false; "FAIL incremental ${n} ($($step.name)): $part differs"; $d | Select-Object -First 4 | ForEach-Object { "  $($_.SideIndicator) $($_.InputObject)" } }
  }
  if ((Get-FileHash $ir).Hash -ne (Get-FileHash $fresh).Hash) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the IR differs" }
  # The same request again, nothing changed: the server sends the kept build's output again (step
  # 3a) and, with --verify-incremental, checks it against a build from scratch.
  $r = Build ($libRoot + @('--server', '--verify-incremental', '--emit-llvm', (Join-Path $work 'server.ll'), $src)) "replay.$n"
  if ($r.exit -ne $f.exit) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the replay's exit $($r.exit), fresh $($f.exit)" }
  foreach ($part in 'out', 'err')
  {
    $d = Compare-Object $f.$part $r.$part -SyncWindow 0
    if ($d) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the replay's $part differs"; $d | Select-Object -First 4 | ForEach-Object { "  $($_.SideIndicator) $($_.InputObject)" } }
  }
  if ((Get-FileHash (Join-Path $work 'server.ll')).Hash -ne (Get-FileHash $fresh).Hash) { $ok = $false; "FAIL incremental ${n} ($($step.name)): the IR after the replay differs" }
  $b = Build ($libRoot + @('--server', '--verify-incremental', '--exe', $exe, $src)) "exe.$n"
  $printed = if ($b.exit -eq 0) { (& $exe) -join ' ' } else { "(build exit $($b.exit))" }
  if ($printed -ne $step.expect) { $ok = $false; "FAIL incremental ${n} ($($step.name)): printed '$printed', expected '$($step.expect)'" }
  # A step builds from the kept program unless it names why not (full: the front end's reason,
  # codegenFull: codegen's), as the servers' log gives it: a silent fallback to a build from scratch
  # (the wide edit's, 2026-11-02: 1.9 s became 20 to 60 s) fails here, whatever the output.
  foreach ($line in (ServerLog $stepStart))
  {
    if ($line -match 'not incremental: (.*)')
    {
      if (-not ($step.full -and $Matches[1] -match $step.full)) { $ok = $false; "FAIL incremental ${n} ($($step.name)): built from scratch: $($Matches[1])" }
    }
    # (no kept session: the --emit-llvm builds', whose server keeps none, by design)
    elseif ($line -match '(codegen in full: .*|rebuildLLVM declined .*)' -and $line -notmatch 'codegen in full: no kept session')
    {
      if (-not ($step.codegenFull -and $Matches[1] -match $step.codegenFull)) { $ok = $false; "FAIL incremental ${n} ($($step.name)): codegen generated the program in full: $($Matches[1])" }
    }
  }
  if ($ok) { "ok   incremental $n ($($step.name))" } else { $failed++ }
}
# Each step's repeat was a replay, checked against a build from scratch (the server's log says so).
# Only this run's servers' lines: other worktrees run this test at the same time, with servers of
# their own (their build directories differ), logging to the same directory.
$ours = Get-ChildItem (Join-Path $env:TEMP 'kppc-server\*.log') | Where-Object { $_.LastWriteTime -ge $started } |
  ForEach-Object { Get-Content $_.FullName } | Where-Object { $_.Length -gt 23 -and [datetime]::ParseExact($_.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff', $null) -ge $started }
$pids = @($ours | Where-Object { $_.Contains("the last $src") } | ForEach-Object { if ($_ -match '\[(\d+)\]') { $Matches[1] } } | Sort-Object -Unique)
$ours = @($ours | Where-Object { $_ -match '\[(\d+)\]' -and $pids -contains $Matches[1] })
$log = $ours | Where-Object { $_ -match 'verify-incremental: the same as from scratch' }
if (@($log).Count -lt $steps.Count) { "FAIL incremental: $(@($log).Count) replays verified, $($steps.Count) expected (the server's log, %TEMP%\kppc-server)"; $failed++ }
else { "ok   incremental replays ($($steps.Count), each the same as from scratch)" }
# The body edits (a plain function's, twice in one file, a template's, a class template member's, a
# constexpr function's) were built from the kept program, the changed bodies checked again, and verified.
$fast = $ours | Where-Object { $_ -match 'incremental: 1 file\(s\) changed \(0 with whole functions\)' }
# Codegen followed through its kept session (step 5b) on the executable's builds, verified: the body
# edits, the template's and the class template member's included (their instances emitted again),
# the signature edits, the layout edit (Shape's: what used its layout, or Frame's and Tagged<Shape>'s
# holding it, emitted again; holders.kpp is not walked again), a global's constructor (its
# initializer reads an edited function), and the edits instances read
# (viaScale<double> and viaLambda's operator for the overload added and removed, scaled<int> dropped
# for Panel::base: every instance of their templates emitted again).
$rebuilt = $ours | Where-Object { $_ -match 'codegen: [0-9]+ module\(s\) assembled again' }
if (@($rebuilt).Count -lt 13) { "FAIL incremental: codegen rebuilt from its kept session $(@($rebuilt).Count) times, expected at least 13 (the server's log)"; $failed++ }
else { "ok   incremental codegen rebuilt from its kept session ($(@($rebuilt).Count))" }
# The codegen error typed into frameSize's body, and the edit while it stays: codegen kept its session
# across them on the executable's builds (and assembled what changed meanwhile once it was fixed).
$keptAcross = @($ours | Where-Object { $_ -match 'codegen: the session kept across [0-9]+ error' })
if ($keptAcross.Count -lt 2) { "FAIL incremental: codegen kept its session across errors $($keptAcross.Count) times, expected at least 2 (the server's log)"; $failed++ }
else { "ok   incremental codegen kept its session across errors ($($keptAcross.Count))" }
$whole = $ours | Where-Object { $_ -match 'with whole functions' -and $_ -notmatch '\(0 with whole functions\)' }
# Instances checked again because they read a change: the overload added and removed (viaScale<double>
# and viaLambda's generic lambda read scale) and the edit of Panel::base (scaled<int>, made by codegen: dropped and made anew),
# through each of two servers; not readTwice<Meter> for the edit of Gauge::reading (lookups in a
# class are recorded with the class).
$instances = @($ours | Where-Object { $_ -match 'instance\(s\) that read a change checked again' })
if ($instances.Count -ne 8) { "FAIL incremental: $($instances.Count) builds checked instances that read a change again, 8 expected (the overload steps, Panel::base and EdgeBase::m)"; $failed++ }
else { "ok   incremental instances checked again only where they read the change (8)" }
# The layout edit (a data member added to Shape) built from the kept program through each server.
$layouts = @($ours | Where-Object { $_ -match 'class layout\(s\) changed' })
# (and the member added to Plain, which a trait in main.kpp reads by position, and the members added
# beside Route's declaration of two declarators and removed: 10)
if ($layouts.Count -lt 10) { "FAIL incremental: $($layouts.Count) layout edits built from the kept program, 10 expected"; $failed++ }
else { "ok   incremental layout edits built from the kept program ($($layouts.Count))" }
# A body edit never generates the program in full for a global's initializer that only looked up a
# free function of the edited member's name (areaOfFour's area(4) and Shape::area: a lookup by name
# alone does not reach a member).
# (and no signature or data member edit for it either: Shape's layout edit, depth, beside the free
# depth(int) localSizes calls)
$byName = @($ours | Where-Object { $_ -match 'codegen in full: .* read an edited function|codegen in full: the initializer of areaOfFour' })
if ($byName.Count -ne 0) { "FAIL incremental: $($byName.Count) body edits generated the program in full for a global that read the edited name: $($byName[0])"; $failed++ }
else { "ok   incremental body edits not in full for a global that read only a free function's name" }
$dropped = @($ours | Where-Object { $_ -match 'made by codegen dropped' })
if ($dropped.Count -ne 4) { "FAIL incremental: $($dropped.Count) builds dropped instances codegen made, 4 expected (Panel::base and Step::operator+ through each server)"; $failed++ }
else { "ok   incremental instances codegen made dropped and made anew (4)" }
# (12: the signature edits, the overload added and removed and unitDepth's return type edit (read by an
# auto local), through each of two servers; the overload steps change what an instance,
# viaScale<double>, calls: it is checked again, not built from scratch)
if (@($whole).Count -lt 12) { "FAIL incremental: the signature edits and the overload added and removed were not built from the kept program ($(@($whole).Count) such builds, 12 expected)"; $failed++ }
else { "ok   incremental whole-function edits built from the kept program ($(@($whole).Count))" }
# (and the edits with errors, through each server: the undeclared name typed and fixed, the line that
# does not parse and the line taken out)
if (@($fast).Count -lt 18) { "FAIL incremental: $(@($fast).Count) body edits built from the kept program, 18 expected (9 through each of two servers) (the server's log, %TEMP%\kppc-server)"; $failed++ }
else { "ok   incremental body edits built from the kept program ($(@($fast).Count))" }
& $Kppc --stop-server --emit-llvm (Join-Path $work 'server.ll') $src | Out-Null
& $Kppc --stop-server --verify-incremental --exe $exe $src | Out-Null
if ($failed -gt 0) { exit 1 }
