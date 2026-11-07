# kpp --lto full and --opt-level 3 (docs/build.md, "Whole-program optimization"): an optimized build is
# one optimization over the whole program in lld's link (full LTO, as FinalRelease's /GL and /LTCG),
# its code generated in one partition per thread, at -O2 or -O3, of four modules or one; ThinLTO at -O3.
# Builds a spec whose functions cross modules (language/static_local_constant_initialization) for
# Windows (Visual Studio's clang, lld-link) and, where run_tests' llvm-mingw is, windows-gnu (lld's
# MinGW driver, which has no partitions), runs each, and checks the link's options; --lto and
# --opt-level's other values are errors. run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\lto'
if (Test-Path $work) { cmd /c rd /s /q "$work" }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$KppExe = (Resolve-Path $KppExe).Path
$source = (Resolve-Path "$PSScriptRoot\spec\language\static_local_constant_initialization.kpp").Path
$expected = (Get-Content "$PSScriptRoot\spec\language\static_local_constant_initialization.expected" -Raw).Trim()
$failed = @()
# A native command's stderr is its output here, not an error that stops the script.
function Native([string[]]$arguments)
{
  $ErrorActionPreference = 'Continue'
  $output = & $KppExe @arguments 2>&1 | ForEach-Object { "$_" }
  $script:code = $LASTEXITCODE
  return ($output -join "`n")
}
$savedClang = $env:KPP_CLANG
$targets = @(, @('windows', ''))
$mingw = "$PSScriptRoot\..\..\build\llvm-mingw\llvm-mingw-20250114-ucrt-x86_64\bin\clang.exe"
if (Test-Path $mingw) { $targets += , @('windows-gnu', (Resolve-Path $mingw).Path) }
try
{
  foreach ($target in $targets)
  {
    if ($target[1]) { $env:KPP_CLANG = $target[1] } else { Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue }
    foreach ($case in @(@('full', '2', '4'), @('full', '3', '4'), @('full', '2', '1'), @('thin', '3', '4')))
    {
      $name = "$($target[0]) --lto $($case[0]) --opt-level $($case[1]) --modules $($case[2])"
      $exe = "$work\$($target[0])-$($case[0])-O$($case[1])-m$($case[2]).exe"
      $output = Native @('--quiet', '--release', '--target', $target[0], '--lto', $case[0], '--opt-level', $case[1], '--modules', $case[2], '--exe', $exe, $source)
      if ($code -ne 0 -or -not (Test-Path $exe)) { $failed += "${name}: the build failed (exit $code)`n$output"; continue }
      $printed = (& $exe | Out-String).Trim()
      if ($printed -ne $expected) { $failed += "${name}: the program printed '$printed', not '$expected'" }
      # The link's options, in its response file (one module's are on clang's one command line).
      if ($case[2] -eq '1') { continue }
      $link = Get-Content -Raw "$exe.link.rsp"
      $gnu = $target[0] -eq 'windows-gnu'
      $want = @($(if ($case[0] -eq 'full') { '-flto' } else { '-flto=thin' }))
      if ($case[0] -eq 'full' -and -not $gnu) { $want += '-Wl,/opt:lldltopartitions=' }
      if ($case[1] -eq '3') { $want += $(if ($gnu) { '-Wl,--lto-O3' } else { '-Wl,/opt:lldlto=3' }) }
      foreach ($w in $want)
      {
        if (-not ($link -split "`r?`n" | Where-Object { $_ -eq $w -or ($w.EndsWith('=') -and $_.StartsWith($w)) })) { $failed += "${name}: the link has no $w" }
      }
      if ($gnu -and $link -match 'partitions') { $failed += "${name}: lld's MinGW driver has no partitions option, and the link names one" }
    }
  }
}
finally
{
  if ($savedClang) { $env:KPP_CLANG = $savedClang } else { Remove-Item Env:KPP_CLANG -ErrorAction SilentlyContinue }
}
# --lto-partitions 1 (C libraries built with -flto in the full LTO: one partition keeps their statics theirs).
$exe = "$work\one-partition.exe"
$output = Native @('--quiet', '--release', '--lto', 'full', '--lto-partitions', '1', '--modules', '4', '--exe', $exe, $source)
if ($code -ne 0 -or -not (Test-Path $exe)) { $failed += "--lto full --lto-partitions 1: the build failed (exit $code)`n$output" }
elseif ((& $exe | Out-String).Trim() -ne $expected) { $failed += "--lto full --lto-partitions 1: the program printed something else" }
elseif (-not ((Get-Content "$exe.link.rsp") -contains '-Wl,/opt:lldltopartitions=1')) { $failed += "--lto full --lto-partitions 1: the link has no /opt:lldltopartitions=1" }
foreach ($wrong in @(@('--lto', 'fast', 'thin or full'), @('--opt-level', '1', '2 or 3'), @('--lto-partitions', '0', 'a number of partitions')))
{
  $output = Native @('--quiet', '--release', $wrong[0], $wrong[1], '--exe', "$work\wrong.exe", $source)
  if ($code -ne 2 -or $output -notmatch [regex]::Escape($wrong[2])) { $failed += "$($wrong[0]) $($wrong[1]): exit $code, not an error naming '$($wrong[2])'`n$output" }
}
if ($failed.Count) { $failed | ForEach-Object { "FAIL lto: $_" }; exit 1 }
"ok   lto: --lto full (four modules, one, -O3) and ThinLTO at -O3 for $(($targets | ForEach-Object { $_[0] }) -join ' and '), their links' options; other values are errors"
exit 0
