# setup-checkout.cmd's copies never reach the main checkout's files (run_tests): on a throwaway main
# checkout (a git repository with data\, shaders\ and src\, two commits; never C:\factorio-in-kpp), a
# new copy and an old one made with junctions to data\ and shaders\ (setup-checkout.cmd before
# 2026-11-03), made safe with --unlink, each go through checkouts, a switch, a reset --hard, a
# sparse-checkout reapply and a status; after each, the main checkout's data\ and shaders\ must be
# whole and its status clean, and the copy must have neither and no junction. Then each copy goes with
# `git worktree remove --force`, and an old one with its junctions still there with rd /s /q and
# `git worktree prune` (worktree remove deletes through the junctions), leaving the main checkout whole. Before --unlink, git
# 2.37 and later took the old copy's files outside its patterns, seen through the junctions, for its
# own, and the first checkout deleted the main checkout's data\ (C:\factorio-in-kpp's, 2026-11-03).
param([string]$Root = (Join-Path $env:TEMP 'kpp-checkout-test'))
$ErrorActionPreference = 'Stop'
$setup = Join-Path $PSScriptRoot '..\setup-checkout.cmd'
$failures = New-Object System.Collections.Generic.List[string]

function Remove-Tree([string]$path)
{
  # (rd: it removes a junction, never what it points to)
  if (Test-Path $path) { cmd /c "rd /s /q `"$path`"" | Out-Null }
}
function RunGit([string]$dir, [string[]]$arguments)
{
  # (git's progress on stderr is no error: its exit code says)
  $ErrorActionPreference = 'Continue'
  $output = & git -C $dir -c user.name=checkout-test -c user.email=checkout-test@kpp.invalid -c core.autocrlf=false @arguments 2>&1
  if ($LASTEXITCODE -ne 0) { throw "git -C $dir $($arguments -join ' '): $output" }
  return $output
}
function Write-File([string]$path, [string]$text)
{
  New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
  [IO.File]::WriteAllText($path, $text)
}
# The main checkout's data\ and shaders\: each file's path and contents.
function Snapshot([string]$main)
{
  $lines = foreach ($d in 'data', 'shaders')
  {
    Get-ChildItem -Recurse -File (Join-Path $main $d) | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($main.Length) + '=' + [IO.File]::ReadAllText($_.FullName) }
  }
  return ($lines -join "`n")
}
function Check([string]$what, [string]$main, [string]$copy, [string]$expected)
{
  if ((Snapshot $main) -ne $expected) { $failures.Add("${what}: the main checkout's data\ or shaders\ changed") }
  $status = RunGit $main @('status', '--porcelain')
  if ($status) { $failures.Add("${what}: the main checkout's status is not clean: $status") }
  foreach ($d in 'data', 'shaders')
  {
    if (Test-Path (Join-Path $copy $d)) { $failures.Add("${what}: the copy has $d\") }
  }
  $links = Get-ChildItem -Recurse -Force -Attributes ReparsePoint $copy -ErrorAction SilentlyContinue
  if ($links) { $failures.Add("${what}: the copy has a junction: $($links[0].FullName)") }
}
# What a thread may do in its copy, each followed by the checks.
function Exercise([string]$what, [string]$main, [string]$copy, [string]$expected, [string]$first, [string]$second)
{
  Check "$what, made" $main $copy $expected
  $steps = @(
    @('checkout', '-q', '--detach', $first),
    @('checkout', '-q', '--detach', $second),
    @('switch', '-q', '-c', "test-$what"),
    @('reset', '-q', '--hard', $first),
    @('sparse-checkout', 'reapply'),
    @('status', '--porcelain'))
  foreach ($step in $steps)
  {
    RunGit $copy $step | Out-Null
    Check "$what, git $($step -join ' ')" $main $copy $expected
  }
}

Remove-Tree $Root
$main = Join-Path $Root 'main'
New-Item -ItemType Directory -Force $main | Out-Null
RunGit $main @('init', '-q', '-b', 'master') | Out-Null
Write-File (Join-Path $main '.gitignore') "/config-path.cfg`n/config/`n"
Write-File (Join-Path $main 'data\core\a.lua') "a = 1`n"
Write-File (Join-Path $main 'data\base\b.lua') "b = 2`n"
Write-File (Join-Path $main 'shaders\s.glsl') "void main() {}`n"
Write-File (Join-Path $main 'src\x.kpp') "int x = 1;`n"
RunGit $main @('add', '.') | Out-Null
RunGit $main @('commit', '-q', '-m', 'first') | Out-Null
$first = (RunGit $main @('rev-parse', 'HEAD')).Trim()
Write-File (Join-Path $main 'src\x.kpp') "int x = 2;`n"
Write-File (Join-Path $main 'data\core\a.lua') "a = 3`n"
RunGit $main @('commit', '-q', '-am', 'second') | Out-Null
$second = (RunGit $main @('rev-parse', 'HEAD')).Trim()
$expected = Snapshot $main

# A new copy.
$copy = Join-Path $Root 'copy'
cmd /c "`"$setup`" `"$copy`" `"$main`" 2>&1" | Out-Null
if ($LASTEXITCODE -ne 0) { $failures.Add("setup-checkout.cmd $copy failed") }
else
{
  $ini = [IO.File]::ReadAllText((Join-Path $copy 'config\config.ini'))
  if ($ini -notmatch "(?m)^read-data=$([regex]::Escape(($main -replace '\\', '/') + '/data'))\r?$") { $failures.Add("the copy's config.ini reads no $main\data: $ini") }
  Exercise 'new' $main $copy $expected $first $second
}

# An old copy, as setup-checkout.cmd made them before: data\ and shaders\ junctions to the main
# checkout's.
function Make-OldCopy([string]$old)
{
  RunGit $main @('worktree', 'add', '--detach', '--no-checkout', $old) | Out-Null
  RunGit $old @('sparse-checkout', 'set', '--no-cone', '/*', '!/data/', '!/shaders/') | Out-Null
  RunGit $old @('checkout', '-q', '--detach', 'HEAD') | Out-Null
  foreach ($d in 'data', 'shaders') { cmd /c "mklink /J `"$old\$d`" `"$main\$d`"" | Out-Null }
}
# One made safe with --unlink.
$old = Join-Path $Root 'old'
Make-OldCopy $old
cmd /c "`"$setup`" --unlink `"$old`" `"$main`" 2>&1" | Out-Null
if ($LASTEXITCODE -ne 0) { $failures.Add("setup-checkout.cmd --unlink $old failed") }
else { Exercise 'unlinked' $main $old $expected $first $second }
# (--unlink again: nothing left to remove, and still safe)
cmd /c "`"$setup`" --unlink `"$old`" `"$main`" 2>&1" | Out-Null
if ($LASTEXITCODE -ne 0) { $failures.Add("setup-checkout.cmd --unlink on a copy without junctions failed") }

# Each copy keeps the files outside its patterns that are on disk skip-worktree (H's second guard).
foreach ($c in $copy, $old)
{
  $setting = (& git -C $c config --worktree --get sparse.expectFilesOutsideOfPatterns 2>$null)
  if ($setting -ne 'true') { $failures.Add("$c has no sparse.expectFilesOutsideOfPatterns=true") }
}

# A copy goes with `git worktree remove --force`: the new one and the unlinked one. An old one with
# its junctions still there goes with `rd /s /q` (the junctions as links) and `git worktree prune`:
# git worktree remove deletes the main checkout's data\ and shaders\ through them (git 2.51,
# 2026-11-03), so it never runs on a copy before --unlink.
function Check-Removed([string]$c)
{
  if (Test-Path $c) { $failures.Add("the removal left $c") }
  if ((Snapshot $main) -ne $expected) { $failures.Add("removing $c changed the main checkout's data\ or shaders\") }
  $status = RunGit $main @('status', '--porcelain')
  if ($status) { $failures.Add("removing ${c}: the main checkout's status is not clean: $status") }
}
foreach ($c in $copy, $old)
{
  RunGit $main @('worktree', 'remove', '--force', $c) | Out-Null
  Check-Removed $c
}
$junctions = Join-Path $Root 'junctions'
Make-OldCopy $junctions
Remove-Tree $junctions
RunGit $main @('worktree', 'prune') | Out-Null
Check-Removed $junctions
if ((RunGit $main @('worktree', 'list')) -match 'junctions') { $failures.Add("git worktree prune left $junctions registered") }

Remove-Tree $Root
if ($failures.Count -gt 0)
{
  foreach ($f in $failures) { "FAIL checkout: $f" }
  exit 1
}
"ok   checkout: a copy and an unlinked old one through checkouts, a switch, a reset --hard, a reapply and git worktree remove (an old one with its junctions: rd and prune); the main checkout's data\ and shaders\ whole"
exit 0
