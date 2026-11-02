# The C import cache (%TEMP%\kpp-imports, main.cpp's importCHeaders) keys an import on the toolchain
# clang makes it with: the same headers imported in another environment (INCLUDE under VsDevCmd,
# another Visual Studio's MSVC) are another entry, never the first environment's import. Here two
# INCLUDE directories each have a kpp_cache_probe.h of their own, with a TMP of the test's own.
# run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\importcache'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$failed = 0
function Fail([string]$what) { "FAIL import cache: $what"; $script:failed++ }

foreach ($n in 1, 2)
{
  New-Item -ItemType Directory -Force "$work\env$n" | Out-Null
  Set-Content -Encoding ascii "$work\env$n\kpp_cache_probe.h" "#define KPP_CACHE_PROBE $n"
}
Set-Content -Encoding ascii "$work\probe.kpp" @'
extern "C" import "kpp_cache_probe.h";
int main()
{
  println(KPP_CACHE_PROBE);
  return 0;
}
'@
New-Item -ItemType Directory -Force "$work\tmp" | Out-Null
$env:TMP = "$work\tmp"
$env:TEMP = "$work\tmp"

# env 1, env 2, env 1 again: each prints its own header's value; the third from the cache.
$runs = @(@(1, 'fills'), @(2, 'fills'), @(1, 'cached'))
for ($k = 0; $k -lt $runs.Count; $k++)
{
  $n = $runs[$k][0]
  $env:INCLUDE = "$work\env$n"
  $exe = "$work\probe$k.exe"
  $report = & $Kppc --exe $exe "$work\probe.kpp" 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { Fail "run $k (INCLUDE env$n) did not compile: $report"; continue }
  $out = (& $exe | Out-String).Trim()
  if ($out -ne "$n") { Fail "run $k (INCLUDE env$n) printed '$out', not $n (another environment's import)" }
  $cached = $report -match 'imports:.*\(cached\)'
  if ($runs[$k][1] -eq 'cached' -and -not $cached) { Fail "run $k (INCLUDE env$n) did not take env$n's import from the cache" }
  if ($runs[$k][1] -eq 'fills' -and $cached) { Fail "run $k (INCLUDE env$n) took an import from the cache" }
}
$entries = @(Get-ChildItem "$work\tmp\kpp-imports" -Filter *.i).Count
if ($entries -ne 2) { Fail "$entries cache entries, not 2 (one per environment)" }

if ($failed -eq 0) { "ok   import cache: one entry per toolchain environment" }
exit $failed
