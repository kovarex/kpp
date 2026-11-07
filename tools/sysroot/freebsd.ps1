# A FreeBSD x86-64 sysroot for kpp --target freebsd (docs/design.md, "Targets and hosts", stage 6):
# the C headers, the C runtime's start files and libraries (static and shared) of the FreeBSD
# release kpp.net's host runs (NearlyFreeSpeech.NET: 14.3-RELEASE, kovarex 2026-11-03), taken from
# that release's base.txz.
# Downloads the pinned base.txz once, checks its SHA-256, and unpacks lib\, usr\include\ and usr\lib\
# into -Out, with every symbolic and hard link replaced by a copy of its target (a Windows linker does
# not follow them); prints the sysroot. Needs Git for Windows' xz (Windows' tar cannot decompress xz).
#   powershell -ExecutionPolicy Bypass -File tools\sysroot\freebsd.ps1 [-Out compiler\build\sysroots\freebsd-x86_64]
# The release is on FreeBSD's archive only (download.freebsd.org dropped it), over http: its https
# certificate names another host. The pinned SHA-256 is VERIFIED (2026-11-03, kovarex's OK): FreeBSD's
# signed 14.3 release announcement, read over https (https://www.freebsd.org/releases/14.3R/announce/),
# gives disc1.iso's SHA-256 f564822bc72d420d1e1a6faacb72f6056d828fcf539dfafd52e08503ef5fab68; the ISO
# downloaded had it, and its usr/freebsd-dist/base.txz and MANIFEST are byte-identical to the ones
# downloaded from the archive, whose SHA-256 is the one pinned here.
param(
  [string]$Out = "$PSScriptRoot\..\..\compiler\build\sysroots\freebsd-x86_64",
  [string]$Download = "$PSScriptRoot\..\..\compiler\build\sysroots"
)
$ErrorActionPreference = 'Stop'
$release = '14.3-RELEASE'
$url = "http://ftp-archive.freebsd.org/pub/FreeBSD-Archive/old-releases/amd64/$release/base.txz"
$size = 210362720
$sha256 = 'E38B5CF756D60086A6C2F736EFF19CC7685F7E2313E31D14342FC8DF57200A92'
if (-not $sha256) { throw "freebsd.ps1: base.txz of $release has no pinned SHA-256 yet" }

New-Item -ItemType Directory -Force $Out, $Download | Out-Null
$Out = (Resolve-Path $Out).Path
$stamp = Join-Path $Out 'kpp-sysroot.txt'
# (A sysroot made before it kept the licences, COPYRIGHT and LLVM's, is made again.)
if ((Test-Path $stamp) -and (Get-Content $stamp -Raw).Trim() -eq "FreeBSD $release $sha256" -and (Test-Path (Join-Path $Out 'COPYRIGHT'))) { $Out; exit 0 }

$txz = Join-Path (Resolve-Path $Download).Path "freebsd-$release-base.txz"
if (-not (Test-Path $txz) -or (Get-Item $txz).Length -ne $size -or (Get-FileHash $txz -Algorithm SHA256).Hash -ne $sha256)
{
  $ProgressPreference = 'SilentlyContinue' # (Invoke-WebRequest's progress bar makes it many times slower)
  Invoke-WebRequest -Uri $url -OutFile $txz -UseBasicParsing
}
$got = (Get-FileHash $txz -Algorithm SHA256).Hash
if ($got -ne $sha256) { throw "$txz has SHA-256 $got, not the pinned $sha256" }

# xz from Git for Windows (mingw64\bin beside git-core), then Windows' own tar on the plain tar.
$xz = Join-Path (Split-Path (Split-Path (& git --exec-path))) 'bin\xz.exe'
if (-not (Test-Path $xz)) { throw "no xz.exe at $xz (Git for Windows' mingw64\bin)" }
$tar = [IO.Path]::ChangeExtension($txz, '.tar')
& cmd /c "`"$xz`" -dc `"$txz`" > `"$tar`""
if ($LASTEXITCODE -ne 0) { throw "xz could not decompress $txz" }

# (Only a directory this script made, or an empty one, is cleared: a wrong -Out is refused.)
if ((Get-ChildItem $Out -Force | Measure-Object).Count -and -not (Test-Path $stamp)) { throw "$Out is not empty and is no sysroot this script made" }
Get-ChildItem $Out -Force | ForEach-Object { if ($_.PSIsContainer) { & cmd /c rd /s /q $_.FullName } else { Remove-Item -Force $_.FullName } }
$wanted = 'lib/', 'usr/include/', 'usr/lib/'
$ErrorActionPreference = 'Continue'
# (Links come out as errors or as empty files: they are replaced below. Only the wanted trees.)
# Also the licences a copy of it carries (kpp's release: docs/build.md, "Installing"): FreeBSD's
# COPYRIGHT, and LLVM's for the libc++, libcxxrt and compiler-rt in usr\lib.
& "$env:SystemRoot\System32\tar.exe" -xf $tar -C $Out --exclude 'usr/lib/debug/*' lib usr/include usr/lib COPYRIGHT usr/share/doc/llvm/LICENSE.TXT 2>$null
$ErrorActionPreference = 'Stop'

# Every link in the wanted trees becomes a copy of what it points to (relative to the link's own
# directory, or to the sysroot for an absolute target), in an order that resolves chains of links.
$links = @{}
foreach ($line in & "$env:SystemRoot\System32\tar.exe" -tvf $tar lib usr/include usr/lib)
{
  if ($line -match '^l\S+\s.*?\s(\S+) -> (\S+)$') { $links[$Matches[1]] = @{ target = $Matches[2]; hard = $false } }
  elseif ($line -match '^h\S+\s.*?\s(\S+) link to (\S+)$') { $links[$Matches[1]] = @{ target = $Matches[2]; hard = $true } }
}
function Resolve([string]$path, [hashtable]$link)
{
  if ($link.hard) { return $link.target }
  if ($link.target.StartsWith('/')) { return $link.target.TrimStart('/') }
  $parts = [Collections.Generic.List[string]]::new()
  foreach ($p in ((Split-Path $path -Parent) -replace '\\', '/').Split('/') + $link.target.Split('/'))
  {
    if ($p -eq '' -or $p -eq '.') { continue }
    if ($p -eq '..') { if ($parts.Count) { $parts.RemoveAt($parts.Count - 1) } } else { $parts.Add($p) }
  }
  return $parts -join '/'
}
$left = [Collections.Generic.List[string]]::new([string[]]$links.Keys)
while ($left.Count)
{
  $done = 0
  foreach ($path in @($left))
  {
    $source = Resolve $path $links[$path]
    if ($links.ContainsKey($source) -and $left.Contains($source)) { continue }
    $from = Join-Path $Out $source
    $to = Join-Path $Out $path
    if (Test-Path $to -PathType Container) { & cmd /c rd /s /q $to } elseif (Test-Path $to) { Remove-Item -Force $to }
    if (Test-Path $from -PathType Container) { & robocopy $from $to /E /NFL /NDL /NJH /NJS /NP | Out-Null }
    elseif (Test-Path $from) { Copy-Item $from $to }
    # (A link to something outside the wanted trees, or to nothing, is left out.)
    $left.Remove($path) | Out-Null
    $done++
  }
  if (-not $done) { throw "links that point at each other: $($left -join ', ')" }
}
Remove-Item $tar

# FreeBSD's objects and archives carry zlib-compressed debug sections, which an lld built without
# zlib (Visual Studio's, llvm-mingw's) refuses: their debug information goes (llvm-objcopy, from
# KPP_CLANG's directory, the bundled llvm-mingw or Visual Studio's LLVM). Linker scripts
# (libncurses.a is "INPUT(...)") stay as they are.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2>$null
$objcopy = @(
  $(if ($env:KPP_CLANG) { Join-Path (Split-Path $env:KPP_CLANG) 'llvm-objcopy.exe' }),
  "$PSScriptRoot\..\..\build\llvm-mingw\llvm-mingw-20250114-ucrt-x86_64\bin\llvm-objcopy.exe",
  $(if ($vs) { Join-Path $vs 'VC\Tools\Llvm\x64\bin\llvm-objcopy.exe' })
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $objcopy) { throw 'no llvm-objcopy.exe (KPP_CLANG''s directory, build\llvm-mingw or Visual Studio''s LLVM)' }
foreach ($f in Get-ChildItem (Join-Path $Out 'usr\lib') -File | Where-Object { $_.Extension -in '.a', '.o' })
{
  $magic = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f.FullName), 0, [Math]::Min(8, $f.Length))
  if ($magic -ne "!<arch>`n" -and -not $magic.StartsWith([char]0x7f + 'ELF')) { continue }
  & $objcopy --strip-debug $f.FullName
  if ($LASTEXITCODE -ne 0) { throw "llvm-objcopy --strip-debug $($f.FullName) failed" }
}

foreach ($need in 'usr\lib\crt1.o', 'usr\lib\crti.o', 'usr\lib\crtbeginT.o', 'usr\lib\libc.a', 'usr\lib\libthr.a', 'usr\lib\libm.a', 'usr\lib\libgcc.a', 'usr\include\stdio.h', 'lib\libc.so.7',
                 'COPYRIGHT', 'usr\share\doc\llvm\LICENSE.TXT')
{
  if (-not (Test-Path (Join-Path $Out $need)) -or (Get-Item (Join-Path $Out $need)).Length -eq 0) { throw "the sysroot has no $need" }
}
Set-Content -Path $stamp -Value "FreeBSD $release $sha256" -Encoding ascii
$Out
