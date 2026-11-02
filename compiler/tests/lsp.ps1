# The language server (docs/ide.md, "The language server as built"): each tests\lsp\NAME.jsonl is a
# scripted editor session, one JSON-RPC message per line, sent to `kppc --lsp --check-each-change`
# on a copy of its program; every message the server sends back must be the line of
# tests\lsp\NAME.expected in the same place. $ROOT stands for the copy's URI both ways.
# session: tests\lsp\program, the requests and diagnostics. cells: tests\lsp\cells, a build.kpp's
# targets and configurations, switched between (docs/editors.md, "Cells"). Both are started with --stdio,
# as VS Code starts it. failed: a check that fails without a diagnostic is shown (window/showMessage).
# -Update writes the expected files from this run (read them before committing). run_tests.cmd runs it.
param([string]$Kppc = "$PSScriptRoot\..\build\kppc.exe", [switch]$Update)
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\lsp'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$Kppc = (Resolve-Path $Kppc).Path
$failed = 0

foreach ($session in @(@{ Name = 'session'; Program = 'program'; Args = '--stdio' }, @{ Name = 'cells'; Program = 'cells'; Args = '--stdio' },
                      @{ Name = 'failed'; Program = 'program'; Args = '--no-such-option' }))
{
  $name = $session.Name
  $program = Join-Path $work $session.Program
  if (Test-Path $program) { Remove-Item -Recurse -Force $program }
  Copy-Item "$PSScriptRoot\lsp\$($session.Program)" $program -Recurse

  # The URI as the server writes it (lsp.cpp, pathToUri): file:///c%3A/dir/dir, the drive lowercase.
  $root = 'file:///' + $program.Substring(0, 1).ToLower() + '%3A' + ($program.Substring(2) -replace '\\', '/' -replace ' ', '%20')

  $request = New-Object System.IO.MemoryStream
  foreach ($line in Get-Content "$PSScriptRoot\lsp\$name.jsonl")
  {
    if ($line.Trim() -eq '' -or $line.StartsWith('//')) { continue }
    $body = [Text.Encoding]::UTF8.GetBytes($line.Replace('$ROOT', $root))
    $head = [Text.Encoding]::ASCII.GetBytes("Content-Length: $($body.Length)`r`n`r`n")
    $request.Write($head, 0, $head.Length)
    $request.Write($body, 0, $body.Length)
  }

  $inPath = Join-Path $work "$name.in"
  $outPath = Join-Path $work "$name.out"
  $logPath = Join-Path $work "$name.log"
  [IO.File]::WriteAllBytes($inPath, $request.ToArray())
  Push-Location $work
  $ErrorActionPreference = 'Continue'
  cmd /c "`"$Kppc`" --lsp $($session.Args) --check-each-change < `"$inPath`" > `"$outPath`" 2> `"$logPath`""
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
    if ($text -notmatch '^Content-Length: ([0-9]+)\r\n\r\n') { throw "lsp.ps1: $($name): no message header at byte $at" }
    $at += $Matches[0].Length
    $length = [int]$Matches[1]
    $got += [Text.Encoding]::UTF8.GetString($raw, $at, $length).Replace($root, '$ROOT')
    $at += $length
  }
  $gotPath = Join-Path $work "$name.txt"
  [IO.File]::WriteAllLines($gotPath, [string[]]$got)
  $expectedPath = "$PSScriptRoot\lsp\$name.expected"
  if ($Update) { Copy-Item $gotPath $expectedPath; Write-Host "lsp.ps1: wrote $expectedPath"; continue }

  $expected = @(Get-Content $expectedPath)
  $bad = 0
  if ($exitCode -ne 0) { Write-Host "FAIL lsp $($name): exit $exitCode (shutdown, then exit, is 0)"; $bad++ }
  for ($i = 0; $i -lt [Math]::Max($expected.Count, $got.Count); $i++)
  {
    if ($i -ge $got.Count -or $i -ge $expected.Count -or $got[$i] -ne $expected[$i])
    {
      Write-Host "FAIL lsp $($name): message $($i + 1) differs"
      Write-Host "  expected: $(if ($i -lt $expected.Count) { $expected[$i] } else { '(none)' })"
      Write-Host "  got:      $(if ($i -lt $got.Count) { $got[$i] } else { '(none)' })"
      $bad++
      break
    }
  }
  if ($bad)
  {
    Write-Host "  the session: $gotPath, the server's log: $logPath"
    $failed++
    continue
  }
  Write-Host "ok   lsp $name ($($got.Count) messages)"
}
if ($failed) { exit 1 }
exit 0
