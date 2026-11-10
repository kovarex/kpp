# The language server (docs/ide.md, "The language server as built"): each tests\lsp\NAME.jsonl is a
# scripted editor session, one JSON-RPC message per line, sent to `kpp --lsp --check-each-change`
# on a copy of its program; every message the server sends back must be the line of
# tests\lsp\NAME.expected in the same place. $ROOT stands for the copy's URI both ways.
# session: tests\lsp\program, the requests and diagnostics. cells: tests\lsp\cells, a build.kpp's
# targets and configurations, switched between (docs/editors.md, "Cells"). Both are started with --stdio,
# as VS Code starts it. failed: a check that fails without a diagnostic is shown (window/showMessage).
# nested: cells opened from below its build.kpp, as Visual Studio opens a generated solution.
# inactive: the code the cell does not build (textDocument/inactiveRegions), debug then release.
# refresh: workspace/semanticTokens/refresh after each check, to a client that takes it. colors:
# Visual Studio's extension's semantic tokens (semanticColors, kpp/semanticTokensChanged), and kpp's own words (words.kpp).
# navigation: tests\lsp\hierarchy, go to implementation and type definition, highlights, folding.
# members: tests\lsp\members, completion after `this.`, `this->` and `This::`, and quick fixes.
# constructors: tests\lsp\constructors, a constructor's references: every construction overload resolution gives
# it, written or not, an instance's (an emplace's) at the program's call that asked for it.
# references: tests\lsp\references, a class used across files, through an alias, a member's qualifier and a
# base's base in an initializer, in a program whose test code is checked (a clean first check, checked again with --tests).
# And a virtual function's: its own, what it overrides and what overrides it, no sibling's and no overload's.
# -Update writes the expected files from this run (read them before committing). run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe", [switch]$Update)
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\lsp'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$KppExe = (Resolve-Path $KppExe).Path
$failed = 0

foreach ($session in @(@{ Name = 'session'; Program = 'program'; Args = '--stdio' }, @{ Name = 'cells'; Program = 'cells'; Args = '--stdio' },
                      @{ Name = 'failed'; Program = 'program'; Args = '--no-such-option' }, @{ Name = 'nested'; Program = 'cells'; Args = '--stdio' },
                      @{ Name = 'inactive'; Program = 'inactive'; Args = '--stdio' }, @{ Name = 'refresh'; Program = 'program'; Args = '--stdio' },
                      @{ Name = 'colors'; Program = 'program'; Args = '--stdio' },
                      @{ Name = 'navigation'; Program = 'hierarchy'; Args = '--stdio' },
                      @{ Name = 'members'; Program = 'members'; Args = '--stdio' },
                      @{ Name = 'constructors'; Program = 'constructors'; Args = '--stdio' },
                      @{ Name = 'references'; Program = 'references'; Args = '--stdio' }))
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
  cmd /c "`"$KppExe`" --lsp $($session.Args) --check-each-change < `"$inPath`" > `"$outPath`" 2> `"$logPath`""
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
