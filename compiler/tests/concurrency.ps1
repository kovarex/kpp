# The compiler's output must not depend on timing under load either: builds a few programs heavy in
# virtual calls, vtables and exceptions at -j 32 many times, 16 processes at once, and compares every
# IR byte for byte with the program's -j 1 build. The thread-count check (determinism.ps1) runs each
# build once; a race in codegen's sharing between threads that shows in 1 or 2 builds of 100 (a
# layout published before its vtables were made: a virtual call emitted as a direct one,
# docs/compile-speed.md, "Codegen's scaling") needs hundreds. run_tests.cmd runs it.
# One program is written here: 1,000 functions each returning a lambda, each beside a function that
# holds the closure and calls it, so that a lambda's writer and its caller are emitted at once on two
# threads. A closure copied while its writer was recorded into it crashed kppc (0xC0000005) in 6 to 8
# of 160 builds (codegen's ClosureWriter; once in a full spec run, language/lambda_returned_by_value).
# Built half as many times as the others (it costs about twice their CPU each): 80 such builds miss a
# crash of that rate about once in 300 runs.
# Where a definition goes must not depend on timing either, which --emit-llvm (one module) cannot show.
# What a first use emits, a generic lambda's call operator for its arguments, a lambda's function-pointer
# thunk, a lambda's call operator its closure's first writer emits, went to that user's module, and users
# in two modules emitted at once (instances of one level) moved it between runs (the game's drawEntities
# comparator, object 175 or 216; codegen's NameHome). So one program is built with 8 modules at -j 1 and
# twice at -j 32, and every module's text (its hash file) must be the -j 1 build's. It has 1,000 of each:
# - a generic lambda, called by two members of a class template's instance and converted to a function
#   pointer by two more;
# - a class template's lambda in a default member initializer, written by its two constructors;
# - a function template's lambda as a default argument, written by two callers.
# In each, the users are instances that sort side by side in their level, so their threads emit them at
# once.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [int]$Copies = 160)
$ErrorActionPreference = 'Stop'
$Kppc = (Resolve-Path $Kppc).Path # (the processes start in .NET's current directory, not PowerShell's)
$work = Join-Path $PSScriptRoot '..\build\tests\concurrency'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
$programs = @('run\exceptions.kpp', 'run\downcast.kpp', 'run\containers.kpp', 'spec\classes\catch_by_second_base.kpp',
  'spec\library\enable_shared_from_this.kpp', 'spec\classes\brace_init_assign.kpp')
$sources = @($programs | ForEach-Object { Join-Path $PSScriptRoot $_ })
$lambdas = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt 1000; $i++)
{
  [void]$lambdas.AppendLine("auto make$i(int s) { return [s](int x) { return x + s + $i; }; }")
  [void]$lambdas.AppendLine("int use$i() { auto f = make$i(1); return f(2) + f(3); }")
}
[void]$lambdas.AppendLine("int main()`n{`n  long long t = 0;")
for ($i = 0; $i -lt 1000; $i++) { [void]$lambdas.AppendLine("  t += use$i();") }
[void]$lambdas.AppendLine("  println(t);`n  return 0;`n}")
$programs += 'returned lambdas (1,000, written here)'
$sources += Join-Path $work 'returned_lambdas.kpp'
$copiesOf = @($Copies) * ($programs.Count - 1) + [Math]::Max(1, [int]($Copies / 2))
[IO.File]::WriteAllText($sources[-1], $lambdas.ToString())

function Start-Build([string]$src, [string]$ir, [string]$j)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $Kppc
  $info.Arguments = "--quiet -j $j --emit-llvm `"$ir`" `"$src`""
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $false
  $info.RedirectStandardError = $false
  return [System.Diagnostics.Process]::Start($info)
}

$failed = 0
$reference = @{}
$n = 0
foreach ($p in $programs)
{
  $n++
  $ir = Join-Path $work "p$n.reference.ll"
  $proc = Start-Build $sources[$n - 1] $ir '1'
  $proc.WaitForExit()
  if ($proc.ExitCode -ne 0 -or -not (Test-Path $ir)) { Write-Host "FAIL concurrency: $p does not build (exit $($proc.ExitCode))"; exit 1 }
  $reference[$n] = (Get-FileHash $ir).Hash
}

# Every program's copies interleaved, 16 processes running at a time.
$queue = New-Object System.Collections.Generic.Queue[object]
for ($c = 1; $c -le $Copies; $c++)
{
  for ($k = 1; $k -le $programs.Count; $k++) { if ($c -le $copiesOf[$k - 1]) { $queue.Enqueue(@($k, $c)) } }
}
$running = New-Object System.Collections.Generic.List[object]
$done = New-Object System.Collections.Generic.List[object]
while ($queue.Count -gt 0 -or $running.Count -gt 0)
{
  while ($queue.Count -gt 0 -and $running.Count -lt 16)
  {
    $k, $c = $queue.Dequeue()
    $ir = Join-Path $work "p$k.c$c.ll"
    $running.Add(@((Start-Build $sources[$k - 1] $ir '32'), $k, $ir))
  }
  $finished = @($running | Where-Object { $_[0].HasExited })
  if ($finished.Count -eq 0) { Start-Sleep -Milliseconds 5; continue }
  foreach ($r in $finished) { [void]$running.Remove($r); $done.Add($r) }
}
$differ = @{}
foreach ($r in $done)
{
  $proc, $k, $ir = $r
  if ($proc.ExitCode -ne 0 -or -not (Test-Path $ir) -or (Get-FileHash $ir).Hash -ne $reference[$k])
  {
    $differ[$k] = 1 + [int]$differ[$k]
  }
  else { Remove-Item $ir }
}
for ($k = 1; $k -le $programs.Count; $k++)
{
  if ($differ[$k])
  {
    $failed++
    Write-Host "FAIL concurrency $($programs[$k - 1]): $($differ[$k]) of $($copiesOf[$k - 1]) builds at -j 32 under load differ from -j 1 (kept in $work)"
  }
}
$generic = New-Object System.Text.StringBuilder
[void]$generic.AppendLine("template<typename F> struct Caller`n{`n  F f;")
[void]$generic.AppendLine("  int a() { return f(1, 2); }`n  int b() { return f(3, 4); }")
[void]$generic.AppendLine("  int c() { int (*p)(int, int) = f; return p(5, 6); }`n  int d() { int (*p)(int, int) = f; return p(7, 8); }`n};")
[void]$generic.AppendLine("template<typename F> Caller<F> caller(F f) { return Caller<F>{f}; }")
[void]$generic.AppendLine("template<int N> struct Holder`n{`n  Function<int(int)> f = [](int x) { return x + N; };`n  int value = 0;")
[void]$generic.AppendLine("  Holder() {}`n  Holder(int v) : value(v) {}`n};")
[void]$generic.AppendLine("template<int N> int applyN(int x, Function<int(int)> f = [](int v) { return v * 3 + N; }) { return f(x); }")
[void]$generic.AppendLine("template<int N> struct UseDefault`n{`n  int a() { return applyN<N>(1); }`n  int b() { return applyN<N>(2); }`n};")
for ($i = 0; $i -lt 1000; $i++)
{
  [void]$generic.AppendLine("int writer$i() { auto c = caller([](auto a, auto b) { return a * b + $i; }); return c.a() + c.b() + c.c() + c.d(); }")
  [void]$generic.AppendLine("int holders$i() { Holder<$i> a; Holder<$i> b(5); return a.f(1) + b.f(2) + b.value; }")
  [void]$generic.AppendLine("int defaults$i() { UseDefault<$i> u; return u.a() + u.b(); }")
}
[void]$generic.AppendLine("int main()`n{`n  long long t = 0;")
for ($i = 0; $i -lt 1000; $i++) { [void]$generic.AppendLine("  t += writer$i() + holders$i() + defaults$i();") }
[void]$generic.AppendLine("  println(t);`n  return 0;`n}")
$genericSource = Join-Path $work 'generic_lambdas.kpp'
[IO.File]::WriteAllText($genericSource, $generic.ToString())
# (The returned lambdas too, with modules: a caller emits the operator on demand when it needs it before
# the writer has, and the writer's own emission puts it where the caller would.)
foreach ($placed in @(@('lambdas', $genericSource), @('returned lambdas', $sources[-1])))
{
  $moduleTexts = @{}
  foreach ($run in 'j1', 'a', 'b')
  {
    $dir = Join-Path $work "placement.$($placed[0] -replace ' ', '_').$run"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $j = if ($run -eq 'j1') { '1' } else { '32' }
    $proc = Start-Process -FilePath $Kppc -ArgumentList @('--quiet', '-j', $j, '--modules', '8', '--exe', "`"$(Join-Path $dir 'p.exe')`"", "`"$($placed[1])`"") -NoNewWindow -PassThru -Wait
    $hashes = @(Get-ChildItem $dir -Filter 'p.exe.*.hash' | Sort-Object Name)
    if ($proc.ExitCode -ne 0 -or $hashes.Count -eq 0) { Write-Host "FAIL concurrency: the $($placed[0])' program does not build with modules (exit $($proc.ExitCode))"; exit 1 }
    $moduleTexts[$run] = ($hashes | ForEach-Object { $_.Name + '=' + (Get-Content $_.FullName -Raw) }) -join ';'
  }
  if ($moduleTexts['a'] -ne $moduleTexts['j1'] -or $moduleTexts['b'] -ne $moduleTexts['j1'])
  {
    $failed++
    Write-Host "FAIL concurrency: a module of the $($placed[0])' program is another text at -j 32 than at -j 1: where a first use's definition went depends on timing (kept in $work)"
  }
}
if ($failed -gt 0) { exit 1 }
"ok   concurrency: $($programs.Count) programs built $Copies times each (the generated one $($copiesOf[-1])) at -j 32 under load, the IR always the -j 1 build's; 3,000 lambdas' and 1,000 returned lambdas' modules the same at -j 1 and -j 32"
