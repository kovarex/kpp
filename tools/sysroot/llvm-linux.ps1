# ThreadSanitizer's and AddressSanitizer's runtimes for Linux x86-64 (kpp --sanitize thread|address --target
# linux; docs/build.md, "Sanitizers"): LLVM 19.1.7's libclang_rt.tsan.a and libclang_rt.asan.a (with their
# .syms, the dynamic lists clang links with, asan_static and asan-preinit), which no clang kpp builds with on
# Windows has (Visual Studio's and llvm-mingw's resource directories hold no x86_64-unknown-linux-gnu\), nor
# the llvm-mingw clang that Factorio's kpp CI links with on Linux, from LLVM's own Linux x86-64 release
# (kovarex, 2026-11-14: a TSan build of the game, as Factorio's CI has; macOS's ASan runtime came the same way,
# "as little friction as possible (as long as it is not gigabytes)": a release of kpp carries the six files,
# 11.6 MB; the archive, 1.65 GB, is downloaded once where kpp is built or packaged, and deleted once the files
# are out of it). llvm-linux.sh does the same on Linux.
# Downloads the pinned archive, checks its SHA-256, takes the files out of it (streamed: the archive's other
# files are never written) into -Out, checks each file's own SHA-256, and prints -Out:
# lib\clang\19\lib\x86_64-unknown-linux-gnu beside kpp, where kpp looks for them (compiler\build's beside a
# build of kpp; a release's, which tools\release\package.ps1 copies). Needs Git for Windows' xz.
#   powershell -ExecutionPolicy Bypass -File tools\sysroot\llvm-linux.ps1 [-Out compiler\build\lib\clang\19\lib\x86_64-unknown-linux-gnu]
# The pinned SHA-256 is the one LLVM attests for the archive (LLVM-19.1.7-Linux-X64.tar.xz.jsonl on the
# release's page, its in-toto subject, for llvm/llvm-project's release-tasks.yml at llvmorg-19.1.7),
# checked 2026-11-14.
param(
  [string]$Out = "$PSScriptRoot\..\..\compiler\build\lib\clang\19\lib\x86_64-unknown-linux-gnu",
  [string]$Download = "$PSScriptRoot\..\..\compiler\build"
)
$ErrorActionPreference = 'Stop'
$name = 'LLVM-19.1.7-Linux-X64'
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/$name.tar.xz"
$size = 1653440720
$sha256 = '4A5EC53951A584ED36F80240F6FBF8FDD46B4CF6C7EE87CC2D5018DC37CAF679'
$runtimes = [ordered]@{
  'libclang_rt.tsan.a' = 'C31E2457E7389691BEE42A04637D8B69211EA9F88471903B671C6CF45ABD9ED3'
  'libclang_rt.tsan.a.syms' = '717915FF75E775A40E2DECE6C60A06845E7B190DA4E2FFB35C1EB86F5D7A3256'
  'libclang_rt.asan.a' = 'FDEBDB409E72F542BE566FE47806612F8648DBC466EDA2F97041EFFA76E1187A'
  'libclang_rt.asan.a.syms' = '8AB0595DCEF4F27CC853C7E44AFBEEB7E837E70544365B1956AD445D268791B8'
  'libclang_rt.asan_static.a' = '88E6B3165BA95EEBC7577E913B9367987A9958A477D7EF3F47A0DF1C696B4B13'
  'libclang_rt.asan-preinit.a' = '668906F73A80793641F8CFD14E767AD0E47EAA4E037743AC253D9BC4503B3DAB'
}

New-Item -ItemType Directory -Force $Out, $Download | Out-Null
$Out = (Resolve-Path $Out).Path
$missing = @($runtimes.Keys | Where-Object { -not (Test-Path (Join-Path $Out $_)) -or (Get-FileHash (Join-Path $Out $_) -Algorithm SHA256).Hash -ne $runtimes[$_] })
if ($missing.Count -eq 0) { $Out; exit 0 }

$Download = (Resolve-Path $Download).Path
$archive = Join-Path $Download "$name.tar.xz"
if (-not (Test-Path $archive) -or (Get-Item $archive).Length -ne $size)
{
  # (Windows' own curl, there since Windows 10 1803: Invoke-WebRequest takes minutes over 1.65 GB)
  & "$env:SystemRoot\System32\curl.exe" --fail --location --silent --show-error --output $archive $url
  if ($LASTEXITCODE -ne 0) { throw "downloading $url failed (curl exit $LASTEXITCODE)" }
}
$got = (Get-FileHash $archive -Algorithm SHA256).Hash
if ($got -ne $sha256)
{
  Remove-Item $archive
  throw "$archive had SHA-256 $got, not the pinned $sha256 (deleted)"
}

# The files, streamed out of xz into Windows' own tar (its stderr goes: xz finds its output closed once tar
# has them all, which is no error here).
$xz = Join-Path (Split-Path (Split-Path (& git --exec-path))) 'bin\xz.exe'
if (-not (Test-Path $xz)) { throw "no xz.exe at $xz (Git for Windows' mingw64\bin)" }
$work = Join-Path $Download "$name.unpacked"
if (Test-Path $work) { & cmd /c rd /s /q $work }
New-Item -ItemType Directory -Force $work | Out-Null
$prefix = "$name/lib/clang/19/lib/x86_64-unknown-linux-gnu"
$members = ($runtimes.Keys | ForEach-Object { "$prefix/$_" }) -join ' '
& cmd /c "`"$xz`" -dc `"$archive`" 2>nul | `"$env:SystemRoot\System32\tar.exe`" -xf - -C `"$work`" $members"
foreach ($runtime in $runtimes.Keys)
{
  $taken = Join-Path $work ("$prefix/$runtime" -replace '/', '\')
  if (-not (Test-Path $taken)) { throw "$archive gave no $prefix/$runtime" }
  $got = (Get-FileHash $taken -Algorithm SHA256).Hash
  if ($got -ne $runtimes[$runtime]) { throw "$taken has SHA-256 $got, not the pinned $($runtimes[$runtime])" }
  Move-Item -Force $taken (Join-Path $Out $runtime)
}
& cmd /c rd /s /q $work
Remove-Item $archive
$Out
