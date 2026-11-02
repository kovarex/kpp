# LLVM's libraries and headers for the back end inside kppc (src\backend.cpp, docs/codegen-speed.md):
# the official clang+llvm 19.1.7 release for Windows, downloaded once, its SHA-256 checked, and of it
# only what kppc links unpacked into -Out\<the archive's SHA-256> (lib\LLVM*.lib, include\llvm and
# include\llvm-c: 256 MB of what the 845 MB archive holds). Prints that directory, which
# compiler\build.cmd takes as KPP_LLVM_DIR. A directory already there is used as it is, so a runner
# that keeps -Out between runs downloads once per pinned release.
#   powershell -File compiler\llvm.ps1 [-Out build\llvm]
param([string]$Out = "$PSScriptRoot\build\llvm")
$ErrorActionPreference = 'Stop'
$name = 'clang+llvm-19.1.7-x86_64-pc-windows-msvc'
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/$name.tar.xz"
$sha256 = 'B4557B4F012161F56A2F5D9E877AB9635CAFD7A08F7AFFE14829BD60C9D357F0'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$root = Join-Path $Out $sha256
if (Test-Path "$root\include\llvm\IR\Module.h") { $root; exit 0 }
$archive = Join-Path $Out "$name.tar.xz"
if (-not (Test-Path $archive) -or (Get-FileHash $archive -Algorithm SHA256).Hash -ne $sha256)
{
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  $ProgressPreference = 'SilentlyContinue' # (Invoke-WebRequest's progress bar makes it many times slower)
  Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
}
$got = (Get-FileHash $archive -Algorithm SHA256).Hash
if ($got -ne $sha256) { throw "$archive has SHA-256 $got, not the pinned $sha256" }
$unpacking = "$root.unpacking"
if (Test-Path $unpacking) { cmd /c rd /s /q $unpacking }
New-Item -ItemType Directory $unpacking | Out-Null
# Python's tarfile (spec.cmd needs Python already), one pass over the archive: Windows' own tar reads
# xz only through an xz program it starts, and hung doing so with no output.
$unpack = @'
import sys, tarfile
archive, out, name = sys.argv[1:4]
safe = {'filter': 'data'} if hasattr(tarfile, 'data_filter') else {}
with tarfile.open(archive, 'r|xz') as t:
    for m in t:
        top, _, rest = m.name.partition('/')
        wanted = rest.startswith(('include/llvm/', 'include/llvm-c/')) or (rest.startswith('lib/LLVM') and rest.endswith('.lib') and rest.count('/') == 1)
        if top != name or not wanted or not m.isfile() or '..' in rest.split('/'):
            continue
        m.name = rest
        t.extract(m, out, **safe)
'@
$unpack | python -I - $archive $unpacking $name
if ($LASTEXITCODE -ne 0) { throw "Python could not unpack $archive" }
if (-not (Test-Path "$unpacking\include\llvm\IR\Module.h") -or -not (Test-Path "$unpacking\lib\LLVMCore.lib")) { throw "$archive held no LLVM headers and libraries" }
Rename-Item $unpacking (Split-Path $root -Leaf)
Remove-Item $archive
$root
