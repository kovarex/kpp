# The bundled toolchain (design.md 17): llvm-mingw, clang and lld of LLVM 19.1.7 with MinGW-w64's
# headers and import libraries for the UCRT (ucrtbase.dll, on every Windows 10 and 11), which builds
# Windows programs in the GNU dialect (kpp --target windows-gnu) with nothing of Visual Studio's.
# Its bin\ld.lld.exe is LLVM's own, from LLVM 19.1.7's Windows release (the clang+llvm archive that
# compiler\llvm.ps1 pins too), not llvm-mingw's (kovarex, 2026-11-27): LLVM's Windows release allocates
# with rpmalloc, llvm-mingw's build of LLVM from the Windows heap, on which a ThinLTO link's threads
# wait. The game's release build took 175 s with llvm-mingw's lld and 69 s with one built with rpmalloc
# (docs/build.md, "The bundled toolchain's lld"). The one file is every lld: ld.lld (the GNU and MinGW
# drivers), lld-link, and ld64.lld (Mach-O) under that name.
# Downloads the pinned releases once, checks their SHA-256, unpacks llvm-mingw into -Out, puts LLVM's
# ld.lld.exe into its bin\ (taken out of the 845 MB archive, which is deleted then, as
# tools\sysroot\llvm-linux.ps1 does; a tree unpacked before gets the file the same way), and prints
# the toolchain's root (bin\clang.exe under it). tests\toolchain.ps1 and tools\release\package.ps1
# use it. The LLVM archive needs Git for Windows' xz.
#   tools\sysroot\llvm-mingw.cmd [-Out build\llvm-mingw]
# (llvm-mingw.cmd runs it with -ExecutionPolicy Bypass: Windows' default policy forbids scripts.)
param([string]$Out = "$PSScriptRoot\..\..\build\llvm-mingw")
$ErrorActionPreference = 'Stop'
$name = 'llvm-mingw-20250114-ucrt-x86_64'
$url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20250114/$name.zip"
$sha256 = '9395d95c089611e6cd728c9a07219ffc5ac706e0c719833a2f7e64a706a81541'
# LLVM 19.1.7's Windows release (compiler\llvm.ps1's archive) and the ld.lld.exe in it.
$llvm = 'clang+llvm-19.1.7-x86_64-pc-windows-msvc'
$llvmUrl = "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/$llvm.tar.xz"
$llvmSize = 845236708
$llvmSha256 = 'B4557B4F012161F56A2F5D9E877AB9635CAFD7A08F7AFFE14829BD60C9D357F0'
$lldSize = 52868096
$lldSha256 = '5DB15319C74B694483A7C1F8B6A31E9D468C5D4243C700656CBC9F6D6754F9F4'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$root = Join-Path $Out $name
$lld = "$root\bin\ld.lld.exe"
# (the size tells LLVM's ld.lld.exe from llvm-mingw's, 4.5 MB; its SHA-256 was checked when it was put there)
if ((Test-Path "$root\bin\clang.exe") -and (Test-Path $lld) -and (Get-Item $lld).Length -eq $lldSize) { $root; exit 0 }
if (-not (Test-Path "$root\bin\clang.exe"))
{
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
}

# LLVM's ld.lld.exe, streamed out of the archive (xz into Windows' own tar: the archive's other files are
# never written; xz's stderr goes, as it finds its output closed once tar has the file).
$archive = Join-Path $Out "$llvm.tar.xz"
if (-not (Test-Path $archive) -or (Get-Item $archive).Length -ne $llvmSize)
{
  # (Windows' own curl, there since Windows 10 1803: Invoke-WebRequest takes minutes over 845 MB)
  & "$env:SystemRoot\System32\curl.exe" --fail --location --silent --show-error --output $archive $llvmUrl
  if ($LASTEXITCODE -ne 0) { throw "downloading $llvmUrl failed (curl exit $LASTEXITCODE)" }
}
$got = (Get-FileHash $archive -Algorithm SHA256).Hash
if ($got -ne $llvmSha256)
{
  Remove-Item $archive
  throw "$archive had SHA-256 $got, not the pinned $llvmSha256 (deleted)"
}
$xz = Join-Path (Split-Path (Split-Path (& git --exec-path))) 'bin\xz.exe'
if (-not (Test-Path $xz)) { throw "no xz.exe at $xz (Git for Windows' mingw64\bin)" }
$work = Join-Path $Out "$llvm.unpacked"
if (Test-Path $work) { & cmd /c rd /s /q $work }
New-Item -ItemType Directory -Force $work | Out-Null
& cmd /c "`"$xz`" -dc `"$archive`" 2>nul | `"$env:SystemRoot\System32\tar.exe`" -xf - -C `"$work`" $llvm/bin/ld.lld.exe"
$taken = "$work\$llvm\bin\ld.lld.exe"
if (-not (Test-Path $taken)) { throw "$archive gave no $llvm/bin/ld.lld.exe" }
$got = (Get-FileHash $taken -Algorithm SHA256).Hash
if ($got -ne $lldSha256) { throw "$taken has SHA-256 $got, not the pinned $lldSha256" }
Move-Item -Force $taken $lld
& cmd /c rd /s /q $work
Remove-Item $archive
$root
