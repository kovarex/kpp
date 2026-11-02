# kpp's name is lower case everywhere (kovarex, 2026-11-02): docs, comments, messages, the installer,
# release titles, the VS Code extension. A capitalised "Kpp" as a word of its own in a tracked text
# file fails, except in code style: an identifier such as KppDescription is no word of its own, and
# the allowed lines below are identifiers or quote the spelling itself.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..\..").Path
$allowed = @(
  '^compiler/tests/project\.ps1:\d+:.*\b(function Kpp\(|Kpp\s*[@($])',  # the test's PowerShell function Kpp
  '^compiler/tests/lowercase\.ps1:',                                    # this check
  '^docs/library-registry\.md:\d+:.*"KPP" and "Kpp"'                   # the trademark search's spellings
)
$hits = @(git -C $repository grep -I -n -w -e 'Kpp' | Where-Object {
  $line = $_
  -not ($allowed | Where-Object { $line -cmatch $_ })
})
if ($hits.Count)
{
  Write-Output "FAIL lower case: kpp is written in lower case (CLAUDE.md), $($hits.Count) line(s):"
  $hits | Select-Object -First 30 | ForEach-Object { Write-Output "  $_" }
  exit 1
}
Write-Output 'ok   lower case'
