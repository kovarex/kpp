# Packages a built kppc as a Kpp release (docs/build.md, "Installing"): build\release\kpp\ with
# kpp.exe (compiler\build\kppc.exe under the name users type), stdlib\, projectlib\, the TextMate
# grammar and the VS Code extension kpp.vsix (tools/vscode); then build\release\kpp-<Version>.zip,
# and kpp-<Version>-setup.exe when Inno Setup's compiler (ISCC) is found. The CI workflow
# (.github/workflows/release.yml) runs it; it runs the same by hand:
#   powershell -File tools\release\package.ps1 -Version 0.0.0
param([Parameter(Mandatory)][string]$Version, [string]$Commit = '')
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot\..\..").Path
$out = Join-Path $repository 'build\release'
$kpp = Join-Path $out 'kpp'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $kpp | Out-Null

Copy-Item "$repository\compiler\build\kppc.exe" "$kpp\kpp.exe"
Copy-Item -Recurse "$repository\stdlib" "$kpp\stdlib"
Copy-Item -Recurse "$repository\projectlib" "$kpp\projectlib"
New-Item -ItemType Directory -Force "$kpp\grammar" | Out-Null
Copy-Item "$repository\tools\vs\kpp\Syntaxes\kpp.tmLanguage.json" "$kpp\grammar"
@"
Kpp $Version$(if ($Commit) { " ($Commit)" })

kpp.exe is the compiler. In a project's directory (one with build.kpp), run kpp for a menu, or
kpp (every target, debug), kpp release, kpp TARGET CONFIGURATION, kpp TARGET --run,
kpp --generate-visual-studio-files. kpp new makes a project. Documentation: https://github.com/kovarex/kpp/blob/master/docs/build.md

Needs Visual Studio 2022 (any edition) with "C++ Clang Compiler for Windows": kpp compiles and links
with its clang and the Windows SDK.

kpp.vsix is the VS Code extension (colouring, and IntelliSense through kpp --lsp): the installer
offers to install it, or run code --install-extension kpp.vsix. Other editors:
https://github.com/kovarex/kpp/blob/master/docs/editors.md

grammar\kpp.tmLanguage.json colours .kpp files in Visual Studio: copy it into
%USERPROFILE%\.vs\Extensions\kpp\Syntaxes\ and restart Visual Studio.
"@ | Set-Content "$kpp\README.txt"

# The VS Code extension (tools/vscode), versioned as the release, with its dependencies from
# package-lock.json. Needs Node.js (npm), as the CI runner has.
Push-Location "$repository\tools\vscode"
try
{
  $ErrorActionPreference = 'Continue'
  npm ci --no-audit --no-fund 2>&1 | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "npm ci failed ($LASTEXITCODE)" }
  npx vsce package $Version --no-git-tag-version --no-update-package-json --skip-license --out "$kpp\kpp.vsix" 2>&1 | Select-Object -Last 2 | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "vsce failed ($LASTEXITCODE)" }
  $ErrorActionPreference = 'Stop'
}
finally { Pop-Location }

# A smoke test of the package itself: kpp new and kpp --run in a project outside the repository.
$smoke = Join-Path $out 'smoke'
New-Item -ItemType Directory -Force $smoke | Out-Null
Push-Location $smoke
try
{
  & "$kpp\kpp.exe" new | Out-Null
  $output = & "$kpp\kpp.exe" --no-server --run 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch 'Hello from Kpp') { throw "the packaged kpp did not build a new project:`n$($output -join "`n")" }
}
finally { Pop-Location }
Remove-Item -Recurse -Force $smoke

Compress-Archive -Path "$kpp\*" -DestinationPath "$out\kpp-$Version.zip"
Write-Output "wrote $out\kpp-$Version.zip"

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
  Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc)
{
  & $iscc /Q "/DVersion=$Version" "/DSource=$kpp" "/O$out" "$PSScriptRoot\kpp.iss"
  if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
  Write-Output "wrote $out\kpp-$Version-setup.exe"
}
else { Write-Output 'no Inno Setup: no installer (the zip is complete)' }
