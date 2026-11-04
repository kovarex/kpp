# kppc --packages (docs/design.md, "Code packages"): every directory under a source root is a package
# named as the directory, a library's root named as the library; a package's README.md gives its
# description (its first paragraph, the headings, pictures and code blocks before it skipped); each
# file lists the names it declares at its top level, an out-of-line member's definition and a second
# overload left out. tests\packages\ against packages.expected.
param([string]$Kppc = "build\kppc.exe")
$ErrorActionPreference = "Stop"
$Kppc = (Resolve-Path $Kppc).Path
$here = $PSScriptRoot
Push-Location $here
try
{
  $output = & $Kppc --packages --no-std --library-root vendor packages\vendorlib packages\shop 2>&1 | Out-String
  $code = $LASTEXITCODE
  $expected = Get-Content -Raw packages.expected
  $normal = { param($t) ($t -replace "`r", "").Trim() }
  if ($code -ne 0 -or (& $normal $output) -ne (& $normal $expected))
  {
    Write-Host "FAIL packages: kppc --packages (exit $code) printed"
    Write-Host $output
    Write-Host "expected (tests\packages.expected)"
    Write-Host $expected
    exit 1
  }
  exit 0
}
finally
{
  Pop-Location
}
