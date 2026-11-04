# A glibc x86-64 sysroot for kppc --target linux (docs/design.md, "Targets and hosts", Q6): the oldest
# system a kpp program runs on is glibc 2.31 (kovarex, 2026-11-04), so the sysroot is Debian 11's
# (bullseye, 11.11, its last point release): the C headers, the C runtime's start files and libraries,
# Linux's headers, and gcc 10's runtime (crtbegin, libgcc, libgcc_s), from six pinned packages.
# A program linked against it needs glibc 2.31 or newer; one linked against the copy of WSL's Ubuntu
# (linux-from-wsl.sh) needs that Ubuntu's glibc (2.39).
# Downloads the pinned .deb files once, checks their SHA-256, and unpacks them into -Out in Debian's
# own layout (lib\, lib64\, usr\include\, usr\lib\), with every symbolic link replaced by a copy of
# its target (a Windows linker does not follow them), and the packages' copyright files beside them;
# prints the sysroot. Needs Git for Windows' xz (Windows' tar reads a .deb but cannot decompress xz).
#   powershell -ExecutionPolicy Bypass -File tools\sysroot\linux.ps1 [-Out compiler\build\sysroots\linux-x86_64]
# The pins come from bullseye's signed index: InRelease (Date: Sat, 09 Aug 2025 09:16:30 UTC, Version
# 11.11), whose signature checked good with Debian's "Archive Automatic Signing Key (11/bullseye)"
# (A7236886F3CCCAAD148A27F80E98404D386FA1D9, from https://ftp-master.debian.org/keys/archive-key-11.asc),
# lists main/binary-amd64/Packages.xz with SHA-256 1c3405444297ead864711c18e64bce4810db63b48340a2f9d81182d9f235ad71,
# whose entries give each package's SHA-256 below (checked 2026-11-04).
param(
  [string]$Out = "$PSScriptRoot\..\..\compiler\build\sysroots\linux-x86_64",
  [string]$Download = "$PSScriptRoot\..\..\compiler\build\sysroots"
)
$ErrorActionPreference = 'Stop'
$mirrors = 'https://deb.debian.org/debian', 'https://archive.debian.org/debian'
$packages = @(
  @{ file = 'pool/main/g/glibc/libc6_2.31-13+deb11u11_amd64.deb'; size = 2822708; sha256 = '05F7264DA867B37F4C5CE49266B558EA1E81E05A9464F623152FCA70F3550282' },
  @{ file = 'pool/main/g/glibc/libc6-dev_2.31-13+deb11u11_amd64.deb'; size = 2361268; sha256 = 'E7F7B45D9C5CFCF37609F0B6EFD3C645272C812144703AF89DFD32218FCB0FD3' },
  @{ file = 'pool/main/l/linux/linux-libc-dev_5.10.223-1_amd64.deb'; size = 1755736; sha256 = 'E3BE603BD12377BB90FCFB0F2048CA3A81C886E49D6F09945B82159D1C54F7B2' },
  @{ file = 'pool/main/g/gcc-10/libgcc-10-dev_10.2.1-6_amd64.deb'; size = 2327620; sha256 = '3B711374092B84E732C4E73CF33E02CFF02E34A5F105EB6467DE8D9038E4CD9D' },
  @{ file = 'pool/main/g/gcc-10/libgcc-s1_10.2.1-6_amd64.deb'; size = 41400; sha256 = 'E478F2709D8474165BB664DE42E16950C391F30EAA55BC9B3573281D83A29DAF' },
  # (for gcc's copyright file alone: libgcc-s1's and libgcc-10-dev's documentation links to it)
  @{ file = 'pool/main/g/gcc-10/gcc-10-base_10.2.1-6_amd64.deb'; size = 201332; sha256 = 'BE65535E94F95FBF04B104E8AB36790476F063374430F7DFC6C516CBE2D2CD1E' }
)
$identity = 'Debian 11.11 ' + (($packages | ForEach-Object { $_.sha256 }) -join ' ')

New-Item -ItemType Directory -Force $Out, $Download | Out-Null
$Out = (Resolve-Path $Out).Path
$stamp = Join-Path $Out 'kpp-sysroot.txt'
if ((Test-Path $stamp) -and (Get-Content $stamp -Raw).Trim() -eq $identity) { $Out; exit 0 }

# The packages, each checked; a mirror that does not have one (bullseye moves to archive.debian.org)
# is passed over.
$debs = @()
$ProgressPreference = 'SilentlyContinue' # (Invoke-WebRequest's progress bar makes it many times slower)
foreach ($p in $packages)
{
  $deb = Join-Path (Resolve-Path $Download).Path (Split-Path $p.file -Leaf)
  $good = { (Test-Path $deb) -and (Get-Item $deb).Length -eq $p.size -and (Get-FileHash $deb -Algorithm SHA256).Hash -eq $p.sha256 }
  foreach ($mirror in $mirrors)
  {
    if (& $good) { break }
    try { Invoke-WebRequest -Uri "$mirror/$($p.file)" -OutFile $deb -UseBasicParsing } catch { }
  }
  if (-not (& $good)) { throw "$($p.file): no mirror gave the file of SHA-256 $($p.sha256)" }
  $debs += $deb
}

# xz from Git for Windows (mingw64\bin beside git-core), then Windows' own tar (bsdtar) on the .deb (an
# ar archive) and on its data tar.
$xz = Join-Path (Split-Path (Split-Path (& git --exec-path))) 'bin\xz.exe'
if (-not (Test-Path $xz)) { throw "no xz.exe at $xz (Git for Windows' mingw64\bin)" }
$tar = "$env:SystemRoot\System32\tar.exe"

# (Only a directory this script made, one linux-from-wsl.sh made (a glibc sysroot: its libc.so.6 and
# stdio.h, no stamp) or an empty one is cleared: a wrong -Out is refused.)
$fromWsl = (Test-Path (Join-Path $Out 'usr\lib\x86_64-linux-gnu\libc.so.6')) -and (Test-Path (Join-Path $Out 'usr\include\stdio.h'))
if ((Get-ChildItem $Out -Force | Measure-Object).Count -and -not (Test-Path $stamp) -and -not $fromWsl) { throw "$Out is not empty and is no sysroot this script made" }
Get-ChildItem $Out -Force | ForEach-Object { if ($_.PSIsContainer) { & cmd /c rd /s /q $_.FullName } else { Remove-Item -Force $_.FullName } }

$work = Join-Path (Resolve-Path $Download).Path 'debian-11-unpack'
$links = @{}
foreach ($deb in $debs)
{
  if (Test-Path $work) { & cmd /c rd /s /q $work }
  New-Item -ItemType Directory $work | Out-Null
  & $tar -xf $deb -C $work data.tar.xz
  if ($LASTEXITCODE -ne 0) { throw "$deb has no data.tar.xz" }
  $data = Join-Path $work 'data.tar'
  & cmd /c "`"$xz`" -dc `"$work\data.tar.xz`" > `"$data`""
  if ($LASTEXITCODE -ne 0) { throw "xz could not decompress $deb's data.tar.xz" }
  # (Links come out as errors or as empty files: they are replaced below. On a case-insensitive file
  # system a few of Linux's netfilter headers differ from others only in case: the later one is lost,
  # as linux-from-wsl.sh skips them.)
  $ErrorActionPreference = 'Continue'
  & $tar -xf $data -C $Out ./lib ./lib64 ./usr/include ./usr/lib ./usr/share/doc 2>$null
  $ErrorActionPreference = 'Stop'
  foreach ($line in & $tar -tvf $data)
  {
    if ($line -match '^l\S+\s.*?\s\./(\S+) -> (\S+)$') { $links[$Matches[1]] = @{ target = $Matches[2]; hard = $false } }
    elseif ($line -match '^h\S+\s.*?\s\./(\S+) link to \./(\S+)$') { $links[$Matches[1]] = @{ target = $Matches[2]; hard = $true } }
  }
}
& cmd /c rd /s /q $work
# (of the packages' documentation, their copyright files alone)
Get-ChildItem (Join-Path $Out 'usr\share\doc') -Recurse -File | Where-Object { $_.Name -ne 'copyright' } | Remove-Item -Force

# Every link becomes a copy of what it points to (relative to the link's own directory, or to the
# sysroot for an absolute target), in an order that resolves chains of links.
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
    # (A link to something outside the unpacked trees, or to nothing, is left out.)
    $left.Remove($path) | Out-Null
    $done++
  }
  if (-not $done) { throw "links that point at each other: $($left -join ', ')" }
}

# Debian 11's libraries are in /lib (its /usr is not merged yet): they are copied into usr\lib too,
# where a merged-/usr system has them and where kpp's tools look (targets.ps1, run_spec.py); the linker
# scripts (libc.so: /lib/x86_64-linux-gnu/libc.so.6) still find theirs in lib\.
$lib = Join-Path $Out 'lib\x86_64-linux-gnu'
$usrLib = Join-Path $Out 'usr\lib\x86_64-linux-gnu'
foreach ($f in Get-ChildItem $lib -File)
{
  if (-not (Test-Path (Join-Path $usrLib $f.Name))) { Copy-Item $f.FullName (Join-Path $usrLib $f.Name) }
}

foreach ($need in 'usr\lib\x86_64-linux-gnu\crt1.o', 'usr\lib\x86_64-linux-gnu\crti.o', 'usr\lib\x86_64-linux-gnu\libc.so',
                 'usr\lib\x86_64-linux-gnu\libc_nonshared.a', 'usr\lib\x86_64-linux-gnu\libm.so', 'lib\x86_64-linux-gnu\libc.so.6', 'usr\lib\x86_64-linux-gnu\libc.so.6',
                 'lib\x86_64-linux-gnu\libm.so.6', 'lib\x86_64-linux-gnu\libgcc_s.so.1', 'lib64\ld-linux-x86-64.so.2',
                 'usr\lib\gcc\x86_64-linux-gnu\10\crtbegin.o', 'usr\lib\gcc\x86_64-linux-gnu\10\libgcc.a', 'usr\lib\gcc\x86_64-linux-gnu\10\libgcc_eh.a',
                 'usr\include\stdio.h', 'usr\include\x86_64-linux-gnu\bits\types.h', 'usr\include\linux\errno.h',
                 'usr\share\doc\libc6\copyright', 'usr\share\doc\gcc-10-base\copyright', 'usr\share\doc\linux-libc-dev\copyright')
{
  if (-not (Test-Path (Join-Path $Out $need)) -or (Get-Item (Join-Path $Out $need)).Length -eq 0) { throw "the sysroot has no $need" }
}
Set-Content -Path $stamp -Value $identity -Encoding ascii
$Out
