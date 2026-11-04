# The compiler's output must not depend on the thread count or on timing (docs/compile-speed.md,
# "Parallel front end"). Builds a few test programs at -j 1, -j 8 and -j 32 and compares the IR
# byte for byte and the report (timings masked); then, when programs\factorio\kpp exists (after
# convert.cmd), the headline tree at -j 1, -j 8 and twice at -j 32: its report and its whole IR.
# run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
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

function Run([string[]]$arguments, [string]$label)
{
  $out = Join-Path $work "$label.out.txt"
  $err = Join-Path $work "$label.err.txt"
  $p = Start-Process -FilePath $Kppc -ArgumentList $arguments -NoNewWindow -PassThru -Wait -RedirectStandardOutput $out -RedirectStandardError $err
  return [pscustomobject]@{ label = $label; out = @(Normalize (Get-Content $out)); err = @(Normalize (Get-Content $err)) }
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

foreach ($name in @('containers', 'templates', 'format'))
{
  $src = Join-Path $PSScriptRoot "run\$name.kpp"
  $runs = @(); $irs = @()
  foreach ($j in @('1', '8', '32'))
  {
    $ir = Join-Path $work "$name.j$j.ll"
    $runs += Run @('-j', $j, '--emit-llvm', $ir, $src) "$name -j $j"
    $irs += $ir
  }
  if (Compare-Runs $runs $name $irs) { "ok   determinism $name" } else { $failed++ }
}

# --debug-info: each module numbers its metadata in the order of its definitions.
$src = Join-Path $PSScriptRoot 'debuginfo\lines.kpp'
$runs = @(); $irs = @()
foreach ($j in @('1', '8', '32'))
{
  $ir = Join-Path $work "debuginfo.j$j.ll"
  $runs += Run @('-j', $j, '--debug-info', '--emit-llvm', $ir, $src) "debuginfo -j $j"
  $irs += $ir
}
if (Compare-Runs $runs 'debuginfo' $irs) { "ok   determinism debuginfo" } else { $failed++ }

# --debug-info's variables and types in a build of 8 modules (debuginfo\values.kpp: each class defined
# in its home module, declared in the others; the types appended in their keys' order): every
# module's text, by the hash kppc keeps beside its object, the same at -j 1, -j 8 and -j 32.
$src = Join-Path $PSScriptRoot 'debuginfo\values.kpp'
$hashes = @{}
foreach ($j in 1, 8, 32)
{
  $exe = Join-Path $work "values.j$j.exe"
  Remove-Item "$exe*" -ErrorAction SilentlyContinue
  & $Kppc --quiet -j $j --debug-info --modules 8 --exe $exe $src 2>&1 | Out-Null
  $hashes[$j] = @(Get-ChildItem "$exe.*.hash" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { "$($_.Name.Substring($exe.Length - $work.Length - 1)) $(Get-Content -Raw $_.FullName)".Trim() }) -join "`n"
}
if ($hashes[1] -and $hashes[1] -eq $hashes[8] -and $hashes[1] -eq $hashes[32]) { "ok   determinism debuginfo modules" }
else
{
  "FAIL determinism debuginfo modules: the module texts' hashes differ (or are missing)"
  foreach ($j in 1, 8, 32) { "  -j ${j}:"; $hashes[$j] -split "`n" | ForEach-Object { "    $_" } }
  $failed++
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
function ClosureNames([string[]]$sources, [string]$label)
{
  $ir = Join-Path $lambdas "$label.ll"
  Remove-Item $ir -ErrorAction SilentlyContinue
  & $Kppc --quiet --emit-llvm $ir @sources 2>&1 | Out-Null
  if (-not (Test-Path $ir)) { return @() }
  return @(Select-String -Path $ir -Pattern 'main::__lambda_[0-9a-f]+_[0-9]+' -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { $_.Value } | Sort-Object -Unique)
}
$alone = ClosureNames @((Join-Path $lambdas 'main.kpp')) 'alone'
$after = ClosureNames @((Join-Path $lambdas 'before.kpp'), (Join-Path $lambdas 'main.kpp')) 'after'
$moved = ClosureNames @((Join-Path $lambdas 'elsewhere\main.kpp')) 'moved'
if ($alone.Count -eq 1 -and "$alone" -eq "$after" -and "$alone" -eq "$moved") { "ok   determinism lambda names ($alone)" }
else { "FAIL determinism lambda names: alone '$alone', after another file '$after', elsewhere '$moved'"; $failed++ }

$tree = Join-Path $PSScriptRoot '..\..\programs\factorio\kpp'
if (Test-Path (Join-Path $tree 'src'))
{
  $factorio = Join-Path $PSScriptRoot '..\..\programs\factorio'
  $common = @('--std', "$PSScriptRoot\..\..\stdlib",
    '--cinclude', 'C:\Factorio\libraries\SDL3\include', '--cinclude', 'C:\Factorio\libraries\SDL_Mixer\include',
    '--cinclude', 'C:\Factorio\libraries\Lua', '--cinclude', 'C:\Factorio\libraries', '--cinclude', 'C:\Factorio\libraries\curl\include',
    '--cinclude', 'C:\Factorio\libraries\glad\include', '--cinclude', 'C:\Factorio\libraries\trio', '--cinclude', "$factorio\native\include",
    '--errors', '0', '--unresolved', '0', "$tree\CommonUtil", "$tree\Agui", "$tree\cxxopts", "$tree\StackWalker", "$tree\Lua", "$tree\src")
  # The whole IR too (--emit-llvm), byte for byte: a name the code generator gives one of two
  # classes that print alike must not depend on which thread reached it first.
  $runs = @()
  $irs = @()
  $n = 0
  foreach ($j in @('1', '8', '32', '32'))
  {
    $n++
    $ir = Join-Path $work "headline.run$n.ll"
    $runs += Run (@('-j', $j, '--emit-llvm', $ir) + $common) "headline -j $j (run $n)"
    $irs += $ir
  }
  if (Compare-Runs $runs "headline" $irs) { "ok   determinism headline (front end and IR)" } else { $failed++ }
  $irs | ForEach-Object { Remove-Item $_ -ErrorAction SilentlyContinue }
}
else { "skip determinism headline front end: programs\factorio\kpp missing (run convert.cmd)" }

if ($failed -gt 0) { exit 1 }
