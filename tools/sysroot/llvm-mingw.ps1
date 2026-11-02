# The bundled toolchain (design.md 17): llvm-mingw, clang and lld of LLVM 19.1.7 with MinGW-w64's
# headers and import libraries for the UCRT (ucrtbase.dll, on every Windows 10 and 11), which builds
# Windows programs in the GNU dialect (kppc --target windows-gnu) with nothing of Visual Studio's.
# Downloads the pinned release once, checks its SHA-256, and unpacks it into -Out; prints the
# toolchain's root (bin\clang.exe under it). tests\toolchain.ps1 and tools\release\package.ps1 use it.
#   powershell -File tools\sysroot\llvm-mingw.ps1 [-Out build\llvm-mingw]
param([string]$Out = "$PSScriptRoot\..\..\build\llvm-mingw")
$ErrorActionPreference = 'Stop'
$name = 'llvm-mingw-20250114-ucrt-x86_64'
$url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20250114/$name.zip"
$sha256 = '9395d95c089611e6cd728c9a07219ffc5ac706e0c719833a2f7e64a706a81541'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$root = Join-Path $Out $name
if (Test-Path "$root\bin\clang.exe") { $root; exit 0 }
$zip = Join-Path $Out "$name.zip"
if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha256)
{
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  $ProgressPreference = 'SilentlyContinue' # (Invoke-WebRequest's progress bar makes it many times slower)
  Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
}
$got = (Get-FileHash $zip -Algorithm SHA256).Hash
if ($got -ne $sha256) { throw "$zip has SHA-256 $got, not the pinned $sha256" }
Expand-Archive -Path $zip -DestinationPath $Out -Force
Remove-Item $zip
if (-not (Test-Path "$root\bin\clang.exe")) { throw "$zip held no $name\bin\clang.exe" }
$root
