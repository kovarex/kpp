# What a spec run leaves (spec\run_spec.py): each snippet is built in a directory of its own, deleted
# once the snippet passes and kept when it fails, and a failure still fails the run. On specfiles\, a
# snippet that passes and one whose output differs, into build\tests\specfiles, where a file left by
# an earlier layout and a directory of a snippet that is gone are planted first: a whole run clears
# both. A kept build is at most 60 files (--modules 4: about 12 of objects and IR; kppc's default made
# about 385 a snippet, 2.6 million files in one worktree, 2026-11-03). run_tests.cmd runs it.
$ErrorActionPreference = 'Stop'
$out = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\specfiles'))
$failed = 0
function Fail([string]$what) { "FAIL specfiles: $what"; $script:failed++ }

if (Test-Path $out) { cmd /c rd /s /q $out }
New-Item -ItemType Directory -Force (Join-Path $out 'gone.snippet') | Out-Null
Set-Content (Join-Path $out 'gone.snippet\left.o') ''
Set-Content (Join-Path $out 'check.passes.exe.0.o') ''

$log = & python -X utf8 -I (Join-Path $PSScriptRoot 'spec\run_spec.py') --snippets (Join-Path $PSScriptRoot 'specfiles') --out $out
$code = $LASTEXITCODE
$last = @($log)[-1]
if ($code -ne 1 -or $last -notlike 'spec: 1 FAILED: check/fails *') { Fail "the run: exit $code, '$last', not exit 1 and 'spec: 1 FAILED: check/fails ...'" }
if (-not ($log -match '^FAIL +check/fails +-- line 1: got `fails 42`, expected `fails 43`$')) { Fail "no FAIL line for check/fails: $($log -join ' | ')" }
if (Test-Path (Join-Path $out 'check.passes')) { Fail 'check/passes passed and its build directory is still there' }
if (-not (Test-Path (Join-Path $out 'check.fails\check.fails.exe'))) { Fail 'check/fails failed and its build (check.fails\check.fails.exe) is gone' }
foreach ($left in 'gone.snippet', 'check.passes.exe.0.o')
{
  if (Test-Path (Join-Path $out $left)) { Fail "the whole run left $left, which no snippet of it built" }
}
$kept = @(Get-ChildItem -Recurse -File $out).Count
if ($kept -gt 60) { Fail "the run left $kept files in $out (check/fails' build: at most 60)" }

if ($failed) { exit 1 }
"ok   specfiles (a passing snippet's build deleted, a failing one's kept: $kept files, and the run failed)"
