# The language server (docs/ide.md, "The language server as built"): tests\lsp\session.jsonl is a
# scripted editor session, one JSON-RPC message per line, sent to `kppc --lsp --check-each-change`
# on a copy of tests\lsp\program; every message the server sends back must be the line of
# tests\lsp\session.expected in the same place. $ROOT stands for the copy's URI both ways.
# -Update writes the expected file from this run (read it before committing it). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [switch]$Update)
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\lsp'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$program = Join-Path $work 'program'
New-Item -ItemType Directory -Force $program | Out-Null
Copy-Item "$PSScriptRoot\lsp\program\*.kpp" $program
$program = (Resolve-Path $program).Path
$Kppc = (Resolve-Path $Kppc).Path

# The URI as the server writes it (lsp.cpp, pathToUri): file:///c%3A/dir/dir, the drive lowercase.
$root = 'file:///' + $program.Substring(0, 1).ToLower() + '%3A' + ($program.Substring(2) -replace '\\', '/' -replace ' ', '%20')

$request = New-Object System.IO.MemoryStream
foreach ($line in Get-Content "$PSScriptRoot\lsp\session.jsonl")
{
  if ($line.Trim() -eq '' -or $line.StartsWith('//')) { continue }
  $body = [Text.Encoding]::UTF8.GetBytes($line.Replace('$ROOT', $root))
  $head = [Text.Encoding]::ASCII.GetBytes("Content-Length: $($body.Length)`r`n`r`n")
  $request.Write($head, 0, $head.Length)
  $request.Write($body, 0, $body.Length)
}

$inPath = Join-Path $work 'session.in'
$outPath = Join-Path $work 'session.out'
$logPath = Join-Path $work 'server.log'
[IO.File]::WriteAllBytes($inPath, $request.ToArray())
Push-Location $work
$ErrorActionPreference = 'Continue'
cmd /c "`"$Kppc`" --lsp --check-each-change < `"$inPath`" > `"$outPath`" 2> `"$logPath`""
$exitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Pop-Location

# The messages it sent, one per line, the root's URI as $ROOT.
$raw = [IO.File]::ReadAllBytes($outPath)
$got = @()
$at = 0
while ($at -lt $raw.Length)
{
  $text = [Text.Encoding]::ASCII.GetString($raw, $at, [Math]::Min(64, $raw.Length - $at))
  if ($text -notmatch '^Content-Length: ([0-9]+)\r\n\r\n') { throw "lsp.ps1: no message header at byte $at" }
  $at += $Matches[0].Length
  $length = [int]$Matches[1]
  $got += [Text.Encoding]::UTF8.GetString($raw, $at, $length).Replace($root, '$ROOT')
  $at += $length
}
$gotPath = Join-Path $work 'session.txt'
[IO.File]::WriteAllLines($gotPath, [string[]]$got)
$expectedPath = "$PSScriptRoot\lsp\session.expected"
if ($Update) { Copy-Item $gotPath $expectedPath; Write-Host "lsp.ps1: wrote $expectedPath"; exit 0 }

$expected = @(Get-Content $expectedPath)
$failed = 0
if ($exitCode -ne 0) { Write-Host "FAIL lsp: exit $exitCode (shutdown, then exit, is 0)"; $failed++ }
for ($i = 0; $i -lt [Math]::Max($expected.Count, $got.Count); $i++)
{
  if ($i -ge $got.Count -or $i -ge $expected.Count -or $got[$i] -ne $expected[$i])
  {
    Write-Host "FAIL lsp: message $($i + 1) differs"
    Write-Host "  expected: $(if ($i -lt $expected.Count) { $expected[$i] } else { '(none)' })"
    Write-Host "  got:      $(if ($i -lt $got.Count) { $got[$i] } else { '(none)' })"
    $failed++
    break
  }
}
if ($failed)
{
  Write-Host "  the session: $gotPath, the server's log: $logPath"
  exit 1
}
Write-Host "ok   lsp ($($got.Count) messages)"
