# kpp --check (an editor's check, kpp --lsp) of a program the front end found errors in (docs/ide.md,
# "Errors as you type"). Without a kept codegen session to follow the edit (a check of its own, a
# server's first build), the front end's errors are the answer and no code is generated: codegen keeps
# no session across some errors, so while many persisted every check generated the whole program again
# (the game: 17-22 s a check with 264 errors, 6-9 s with 1,602, its front end 0.4 s). Through a compile
# server whose kept session follows the edit, codegen runs as before and its own errors show beside the
# front end's (a member function used as a value is codegen's). run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\checkerrors'
function StopLeftovers()
{
  $root = [IO.Path]::GetFullPath($work)
  # (a process's Path read once: one that exits between two reads has none the second time)
  Get-Process | Where-Object { $path = $_.Path; $path -and $path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
}
if (Test-Path $work) { StopLeftovers; cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force "$work\src", "$work\tmp" | Out-Null
$env:TMP = "$work\tmp"
$env:TEMP = "$work\tmp"
$std = (Resolve-Path "$PSScriptRoot\..\..\stdlib").Path
$KppExe = (Resolve-Path $KppExe).Path

# Two files, two modules (a session keeps its texts only then).
[IO.File]::WriteAllText("$work\src\shape.kpp", "struct Shape { int width = 0; int height = 0; int size() const { return width; } };`nint area(const Shape& s) { return s.width * s.height; }`n")
$main = "int main()`n{`n  Shape square;`n  square.width = 3;`n  square.height = 3;`n  return area(square) == 9 ? 0 : 1;`n}`n"
$frontEnd = $main.Replace('  square.height = 3;', '  sqaure.height = 3;') # the checker's: 'sqaure' is not declared
$codegens = $main.Replace('  square.width = 3;', '  square.size = 3;')   # codegen's: a member function is no value
$both = $frontEnd.Replace('  square.width = 3;', '  square.size = 3;')
function Write-Main([string]$text) { [IO.File]::WriteAllText("$work\src\main.kpp", $text) }

$arguments = @('--quiet', '--check', '--std', $std, "$work\src")
$failed = 0
# One check: its exit code and which of the two errors and the "no code generated" line it printed.
# (Through cmd: PowerShell's own redirection of a native command's stderr wraps long lines.)
function Check([string]$label, [string]$client, [int]$exit, [bool]$checkers, [bool]$codegen, [bool]$noCode)
{
  $ErrorActionPreference = 'Continue'
  cmd /c "`"$KppExe`" $client --quiet --check --std `"$std`" `"$work\src`" > `"$work\out.txt`" 2> `"$work\err.txt`""
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  $text = [IO.File]::ReadAllText("$work\out.txt") + [IO.File]::ReadAllText("$work\err.txt")
  $got = @($code, $text.Contains("'sqaure' is not declared"), $text.Contains('is a member function, not a value'), $text.Contains('no code generated'))
  $want = @($exit, $checkers, $codegen, $noCode)
  if ("$got" -ne "$want")
  {
    "FAIL checkerrors: $label (exit, the checker's error, codegen's, no code generated): $got, $want expected"
    ($text -split "`n" | Select-Object -First 6) | ForEach-Object { "  $_" }
    $script:failed++
  }
}

try
{
  # A check of its own has no session: the front end's errors alone.
  Write-Main $both
  Check 'a check of its own, both errors' '' 1 $true $false $true
  Write-Main $codegens
  Check "a check of its own, codegen's error alone" '' 1 $false $true $false
  # A server's session follows the edit: codegen runs, both errors show.
  Write-Main $main
  Check 'the server, its first check clean' '--server' 0 $false $false $false
  Write-Main $both
  Check 'the server, both errors typed (its session follows)' '--server' 1 $true $true $false
  Write-Main $main
  Check 'the server, both fixed' '--server' 0 $false $false $false
  # A server whose first check has the errors: no session, the front end's errors alone, then
  # codegen's once they are fixed.
  $ErrorActionPreference = 'Continue'
  & $KppExe --stop-server @arguments > $null 2> $null
  $ErrorActionPreference = 'Stop'
  Write-Main $both
  Check 'a new server, both errors' '--server' 1 $true $false $true
  Check 'a new server, both errors again' '--server' 1 $true $false $true
  Write-Main $codegens
  Check "a new server, the checker's error fixed" '--server' 1 $false $true $false
  Write-Main $main
  Check 'a new server, both fixed' '--server' 0 $false $false $false
}
finally
{
  $ErrorActionPreference = 'Continue'
  & $KppExe --stop-server @arguments > $null 2> $null
  $ErrorActionPreference = 'Stop'
  StopLeftovers
}
if ($failed) { exit 1 }
"ok   checkerrors: a check without a session to follow the edit stops at the front end's errors; a server's session still shows codegen's"
cmd /c rd /s /q "$work"
exit 0
