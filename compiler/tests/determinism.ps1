# The compiler's output must not depend on the thread count or on timing (docs/compile-speed.md,
# "Parallel front end"). Builds a few test programs at -j 1, -j 8 and -j 32 and compares the IR
# byte for byte and the report (timings masked); then, when programs\factorio\kpp exists (after
# convert.cmd), the headline tree at -j 1, -j 8 and twice at -j 32: its report and its whole IR.
# run_tests.cmd runs it: with -Full (run_tests --full, the batch run on master) all of that; without, the
# quick gate's (coordinator, 2026-11-05): the test programs at -j 1 and -j 32, and the headline tree's
# front end at -j 8 and -j 32 (its -j 1 build was 61 of the check's 149 s on a loaded machine). The
# builds compared with each other run at once, each in its own process: what they are compared on is
# their own output, which another process's load must not change either.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [switch]$Full)
$threadCounts = if ($Full) { @('1', '8', '32') } else { @('1', '32') }
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\determinism'
New-Item -ItemType Directory -Force $work | Out-Null
$failed = 0

# The report without what may differ from run to run by design: timings and rates, the thread
# count, wait counts, the number of interned types (a cache: equivalent paths taken in another
# order intern different intermediate types), C import temp files (named with the process id), and
# whether the C imports came from the cache (the first run after a rebuild or with a fresh TMP fills it).
function Normalize([string[]]$lines)
{
  $lines | Where-Object { $_ -notmatch '^check:\s+[0-9]+ distinct types' } | ForEach-Object {
    $_ -replace '[0-9]+\.[0-9]+ s\b', '_ s' -replace '\([0-9.]+ MB/s[^)]*\)', '(_)' -replace '\([0-9.]+ M lines/s\)', '(_)' `
       -replace '^threads:.*', 'threads: _' -replace '\(([0-9]+) requests, ([0-9]+) waits\)', '(_ requests, _ waits)' `
       -replace 'kpp-imports\\[0-9a-f]+\.[0-9]+\.c', 'kpp-imports\_.c' -replace ' \(cached\)$', ''
  }
}

# A kppc run started, its stdout and stderr to files of its label. Waited for by its own process
# (WaitForExit), never by Start-Process -Wait, which also waits for what it started (link.exe's
# vctip.exe lingers: tests\concurrency.ps1).
function Start-Run([string[]]$arguments, [string]$label)
{
  $out = Join-Path $work "$label.out.txt"
  $err = Join-Path $work "$label.err.txt"
  $p = Start-Process -FilePath $Kppc -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
  # (its handle taken at once: WaitForExit on a process that ended before anyone held its handle
  # throws "The parameter is incorrect", which run_tests' quick run hit, 2026-11-05)
  $null = $p.Handle
  return [pscustomobject]@{ label = $label; process = $p; outFile = $out; errFile = $err }
}

function Wait-Run($started)
{
  $started.process.WaitForExit()
  return [pscustomobject]@{ label = $started.label; out = @(Normalize (Get-Content $started.outFile)); err = @(Normalize (Get-Content $started.errFile)) }
}

function Compare-Runs($runs, [string]$what, [string[]]$irFiles)
{
  $ok = $true
  for ($k = 1; $k -lt $runs.Count; $k++)
  {
    foreach ($part in @('out', 'err'))
    {
      $d = Compare-Object $runs[0].$part $runs[$k].$part -SyncWindow 0
      if ($d)
      {
        $ok = $false
        Write-Host "FAIL determinism ${what}: $($runs[0].label) and $($runs[$k].label) differ ($part)"
        $d | Select-Object -First 4 | ForEach-Object { Write-Host "  $($_.SideIndicator) $($_.InputObject)" }
      }
    }
    if ($irFiles -and (Get-FileHash $irFiles[0]).Hash -ne (Get-FileHash $irFiles[$k]).Hash)
    {
      $ok = $false
      Write-Host "FAIL determinism ${what}: the IR of $($runs[0].label) and $($runs[$k].label) differs"
    }
  }
  return $ok
}

# The test programs, and --debug-info's lines (each module numbers its metadata in the order of its
# definitions): every build started at once, then each group compared.
$groups = [ordered]@{}
foreach ($name in @('containers', 'templates', 'format'))
{
  $src = Join-Path $PSScriptRoot "run\$name.kpp"
  $groups[$name] = @(foreach ($j in $threadCounts)
  {
    $ir = Join-Path $work "$name.j$j.ll"
    [pscustomobject]@{ run = Start-Run @('-j', $j, '--emit-llvm', $ir, $src) "$name -j $j"; ir = $ir }
  })
}
$src = Join-Path $PSScriptRoot 'debuginfo\lines.kpp'
$groups['debuginfo'] = @(foreach ($j in $threadCounts)
{
  $ir = Join-Path $work "debuginfo.j$j.ll"
  [pscustomobject]@{ run = Start-Run @('-j', $j, '--debug-info', '--emit-llvm', $ir, $src) "debuginfo -j $j"; ir = $ir }
})
foreach ($name in $groups.Keys)
{
  $runs = @($groups[$name] | ForEach-Object { Wait-Run $_.run })
  $irs = @($groups[$name] | ForEach-Object { $_.ir })
  if (Compare-Runs $runs $name $irs) { "ok   determinism $name" } else { $failed++ }
}

# --debug-info's variables and types in a build of 8 modules (debuginfo\values.kpp: each class defined
# in its home module, declared in the others; the types appended in their keys' order): every
# module's text, by the hash kppc keeps beside its object, the same at -j 1, -j 8 and -j 32.
$src = Join-Path $PSScriptRoot 'debuginfo\values.kpp'
$started = @(foreach ($j in $threadCounts)
{
  $exe = Join-Path $work "values.j$j.exe"
  Remove-Item "$exe*" -ErrorAction SilentlyContinue
  [pscustomobject]@{ j = $j; exe = $exe; run = Start-Run @('--quiet', '-j', $j, '--debug-info', '--modules', '8', '--exe', $exe, $src) "values -j $j" }
})
$hashes = @{}
foreach ($s in $started)
{
  $s.run.process.WaitForExit()
  $hashes[$s.j] = @(Get-ChildItem "$($s.exe).*.hash" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { "$($_.Name.Substring($s.exe.Length - $work.Length - 1)) $(Get-Content -Raw $_.FullName)".Trim() }) -join "`n"
}
if ($hashes['1'] -and @($threadCounts | Where-Object { $hashes[$_] -ne $hashes['1'] }).Count -eq 0) { "ok   determinism debuginfo modules" }
else
{
  "FAIL determinism debuginfo modules: the module texts' hashes differ (or are missing)"
  foreach ($j in $threadCounts) { "  -j ${j}:"; $hashes[$j] -split "`n" | ForEach-Object { "    $_" } }
  $failed++
}

# The names in a program's IR, of a build started with Start-Run, once it has ended: every match of the pattern, each once.
function NamesIn($started, [string]$ir, [string]$pattern)
{
  $started.process.WaitForExit()
  if (-not (Test-Path $ir)) { return @() }
  return @(Select-String -Path $ir -Pattern $pattern -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { $_.Value } | Sort-Object -Unique)
}

# A lambda's closure is named by its file's name under its root and its number in the file
# (`main::__lambda_<tag>_0`, ParsedFile::lambdaTag): the same with another file before it in the
# program and with the file somewhere else (by its index, two standard library files added once
# renamed every later file's lambdas, in the IR, the PDB and every module's hash).
$lambdas = Join-Path $work 'lambdas'
New-Item -ItemType Directory -Force (Join-Path $lambdas 'elsewhere') | Out-Null
$program = "int main()`n{`n  auto twice = [](int v) { return v * 2; };`n  return twice(21) - 42;`n}`n"
[IO.File]::WriteAllText((Join-Path $lambdas 'main.kpp'), $program)
[IO.File]::WriteAllText((Join-Path $lambdas 'elsewhere\main.kpp'), $program)
[IO.File]::WriteAllText((Join-Path $lambdas 'before.kpp'), "int before()`n{`n  auto f = [](int v) { return v + 1; };`n  return f(1);`n}`n")

# A file's private entities (design.md 15) are named by the file's name and the same tag
# (`main.kpp@<tag>::helper`, filePrivatePrefix): the same with another file before it and with the
# file somewhere else (by the hash of its absolute path, every checkout named them apart, in the IR,
# the PDB and the modules' hashes).
$privates = Join-Path $work 'privates'
New-Item -ItemType Directory -Force (Join-Path $privates 'elsewhere') | Out-Null
$program = "private int helper(int v) { return v * 2; }`nprivate struct Box { int value = 0; int twice() const { return value * 2; } };`ntemplate<typename T> T same(T v) { return v; }`nint main()`n{`n  Box b;`n  b.value = helper(21);`n  return same(b).twice() - 84;`n}`n"
[IO.File]::WriteAllText((Join-Path $privates 'main.kpp'), $program)
[IO.File]::WriteAllText((Join-Path $privates 'elsewhere\main.kpp'), $program)
[IO.File]::WriteAllText((Join-Path $privates 'before.kpp'), "private int helper(int v) { return v + 1; }`nint before() { return helper(1); }`n")

# The six builds of both checks at once.
$names = [ordered]@{}
foreach ($check in @(@{ dir = $lambdas; kind = 'lambdas' }, @{ dir = $privates; kind = 'privates' }))
{
  foreach ($case in @(@{ label = 'alone'; sources = @('main.kpp') }, @{ label = 'after'; sources = @('before.kpp', 'main.kpp') }, @{ label = 'moved'; sources = @('elsewhere\main.kpp') }))
  {
    $ir = Join-Path $check.dir "$($case.label).ll"
    Remove-Item $ir -ErrorAction SilentlyContinue
    $sources = @($case.sources | ForEach-Object { Join-Path $check.dir $_ })
    $names["$($check.kind) $($case.label)"] = [pscustomobject]@{ ir = $ir; run = Start-Run (@('--quiet', '--emit-llvm', $ir) + $sources) "$($check.kind) $($case.label)" }
  }
}
$closure = 'main::__lambda_[0-9a-f]+_[0-9]+'
$alone = NamesIn $names['lambdas alone'].run $names['lambdas alone'].ir $closure
$after = NamesIn $names['lambdas after'].run $names['lambdas after'].ir $closure
$moved = NamesIn $names['lambdas moved'].run $names['lambdas moved'].ir $closure
if ($alone.Count -eq 1 -and "$alone" -eq "$after" -and "$alone" -eq "$moved") { "ok   determinism lambda names ($alone)" }
else { "FAIL determinism lambda names: alone '$alone', after another file '$after', elsewhere '$moved'"; $failed++ }
$private = '(same<)?main\.kpp@[0-9a-f]+::(helper|Box)'
$alone = NamesIn $names['privates alone'].run $names['privates alone'].ir $private
$after = NamesIn $names['privates after'].run $names['privates after'].ir $private
$moved = NamesIn $names['privates moved'].run $names['privates moved'].ir $private
if ($alone.Count -eq 3 -and "$alone" -eq "$after" -and "$alone" -eq "$moved") { "ok   determinism private names ($alone)" }
else { "FAIL determinism private names: alone '$alone', after another file '$after', elsewhere '$moved'"; $failed++ }

$tree = Join-Path $PSScriptRoot '..\..\programs\factorio\kpp'
if (Test-Path (Join-Path $tree 'src'))
{
  $factorio = Join-Path $PSScriptRoot '..\..\programs\factorio'
  $common = @('--std', "$PSScriptRoot\..\..\stdlib",
    '--cinclude', 'C:\Factorio\libraries\SDL3\include', '--cinclude', 'C:\Factorio\libraries\SDL_Mixer\include',
    '--cinclude', 'C:\Factorio\libraries\Lua', '--cinclude', 'C:\Factorio\libraries', '--cinclude', 'C:\Factorio\libraries\curl\include',
    '--cinclude', 'C:\Factorio\libraries\glad\include', '--cinclude', 'C:\Factorio\libraries\trio', '--cinclude', "$factorio\native\include",
    '--config', 'finalRelease=false', # (the game's FINAL_RELEASE code: link-settings.cmd's CINC)
    '--errors', '0', '--unresolved', '0', "$tree\CommonUtil", "$tree\Agui", "$tree\cxxopts", "$tree\StackWalker", "$tree\Lua", "$tree\src")
  # The whole IR too (--emit-llvm), byte for byte: a name the code generator gives one of two
  # classes that print alike must not depend on which thread reached it first. The quick gate's: the
  # front end's report alone, at -j 8 and -j 32.
  $started = @()
  $irs = @()
  $n = 0
  foreach ($j in $(if ($Full) { @('1', '8', '32', '32') } else { @('8', '32') }))
  {
    $n++
    $ir = Join-Path $work "headline.run$n.ll"
    $started += Start-Run ($(if ($Full) { @('-j', $j, '--emit-llvm', $ir) } else { @('-j', $j) }) + $common) "headline -j $j (run $n)"
    $irs += $ir
  }
  $runs = @($started | ForEach-Object { Wait-Run $_ })
  if (Compare-Runs $runs "headline" $(if ($Full) { $irs } else { $null })) { "ok   determinism headline ($(if ($Full) { 'front end and IR' } else { 'front end at -j 8 and -j 32; run_tests --full for -j 1 and the IR' }))" } else { $failed++ }
  $irs | ForEach-Object { Remove-Item $_ -ErrorAction SilentlyContinue }
}
else { "skip determinism headline front end: programs\factorio\kpp missing (run convert.cmd)" }

if ($failed -gt 0) { exit 1 }
