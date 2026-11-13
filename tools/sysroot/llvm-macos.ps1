# AddressSanitizer's runtime for macOS arm64 (kpp --sanitize address --target macos; docs/build.md,
# "Sanitizers"): LLVM 19.1.7's libclang_rt.asan_osx_dynamic.dylib, which no clang kpp builds with has
# (Visual Studio's and llvm-mingw's resource directories hold no darwin\), from LLVM's own macOS arm64
# release (kovarex, 2026-11-13: "as little friction as possible (as long as it is not gigabytes)": a
# release of kpp carries the one file, 1.1 MB; the archive, 1.4 GB, is downloaded once where kpp is built
# or packaged, and deleted once the file is out of it).
# Downloads the pinned archive, checks its SHA-256, takes that one file out of it (streamed: the archive's
# 9,000 other files are never written) into -Out, checks the file's own SHA-256, and prints -Out:
# lib\clang\19\lib\darwin beside kpp, where kpp looks for it (compiler\build's beside a build of kpp; a
# release's, which tools\release\package.ps1 copies). Needs Git for Windows' xz (Windows' tar cannot
# decompress xz).
#   powershell -ExecutionPolicy Bypass -File tools\sysroot\llvm-macos.ps1 [-Out compiler\build\lib\clang\19\lib\darwin]
# The pinned SHA-256 is the one LLVM attests for the archive (LLVM-19.1.7-macOS-ARM64.tar.xz.jsonl on the
# release's page, signed for llvm/llvm-project's release-binaries.yml at llvmorg-19.1.7): `gh attestation
# verify LLVM-19.1.7-macOS-ARM64.tar.xz --repo llvm/llvm-project --bundle LLVM-19.1.7-macOS-ARM64.tar.xz.jsonl`
# passed, 2026-11-13. The dylib is arm64's alone, for macOS 11.0 and later, as kpp's macos target; LLVM's
# x86-64 release has one of its own, which needs macOS 13.7 (not taken: macos-x64 stays an error).
param(
  [string]$Out = "$PSScriptRoot\..\..\compiler\build\lib\clang\19\lib\darwin",
  [string]$Download = "$PSScriptRoot\..\..\compiler\build"
)
$ErrorActionPreference = 'Stop'
$name = 'LLVM-19.1.7-macOS-ARM64'
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/$name.tar.xz"
$size = 1413914424
$sha256 = 'D93BF12952D89FE4EC7501C40475718B722407DA6A8D651F05C995863468E570'
$runtime = 'libclang_rt.asan_osx_dynamic.dylib'
$runtimeSha256 = '545CD7DB5458A1958C1CCF2D58D90CB4AE46BAD17611CEE13B2F313A78AB5E8E'

New-Item -ItemType Directory -Force $Out, $Download | Out-Null
$Out = (Resolve-Path $Out).Path
$file = Join-Path $Out $runtime
if ((Test-Path $file) -and (Get-FileHash $file -Algorithm SHA256).Hash -eq $runtimeSha256) { $Out; exit 0 }

$Download = (Resolve-Path $Download).Path
$archive = Join-Path $Download "$name.tar.xz"
if (-not (Test-Path $archive) -or (Get-Item $archive).Length -ne $size)
{
  # (Windows' own curl, there since Windows 10 1803: Invoke-WebRequest takes minutes over 1.4 GB)
  & "$env:SystemRoot\System32\curl.exe" --fail --location --silent --show-error --output $archive $url
  if ($LASTEXITCODE -ne 0) { throw "downloading $url failed (curl exit $LASTEXITCODE)" }
}
$got = (Get-FileHash $archive -Algorithm SHA256).Hash
if ($got -ne $sha256)
{
  Remove-Item $archive
  throw "$archive had SHA-256 $got, not the pinned $sha256 (deleted)"
}

# The one file, streamed out of xz into Windows' own tar, which stops once it has it (-q); xz then
# finds its output closed, which is no error here (its stderr goes).
$xz = Join-Path (Split-Path (Split-Path (& git --exec-path))) 'bin\xz.exe'
if (-not (Test-Path $xz)) { throw "no xz.exe at $xz (Git for Windows' mingw64\bin)" }
$work = Join-Path $Download "$name.unpacked"
if (Test-Path $work) { & cmd /c rd /s /q $work }
New-Item -ItemType Directory -Force $work | Out-Null
$member = "$name/lib/clang/19/lib/darwin/$runtime"
& cmd /c "`"$xz`" -dc `"$archive`" 2>nul | `"$env:SystemRoot\System32\tar.exe`" -xqf - -C `"$work`" $member"
$taken = Join-Path $work ($member -replace '/', '\')
if (-not (Test-Path $taken)) { throw "$archive gave no $member" }
$got = (Get-FileHash $taken -Algorithm SHA256).Hash
if ($got -ne $runtimeSha256) { throw "$taken has SHA-256 $got, not the pinned $runtimeSha256" }
Move-Item -Force $taken $file
& cmd /c rd /s /q $work
Remove-Item $archive
$Out
