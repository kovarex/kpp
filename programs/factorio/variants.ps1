# One configuration's and platform's conversion of the whole, for every variant of the game's build
# (design.md 12, "The game's build variants as config constants"), called by convert.cmd's :whole.
# The game is converted once for each of the 16 combinations of STEAM, DEMO, HEADLESS and SPACE_AGE
# (DISABLE_UPDATER with STEAM or DEMO, as fbuild.bff defines it), four at a time, and the 16 made one
# by kpp --convert-merge-flag a constant at a time: config::spaceAge, config::headless, config::demo,
# then config::steam. The two sides of each merge differ by that constant alone, the others' choices
# already made alike in both, so a group that one variant decides is chosen by that constant alone
# (combinations no package is built from, Steam's headless server, are converted too: they cost
# conversions, not code). The marks are kept for the platforms' and the configurations' merges.
# From convert.cmd's environment: KPP, FACTORIO, EXT, CLIBUSE, and WHOLEOUT (the directory written),
# WHOLEOPTS (the configuration's and platform's options), WHOLEINPUTS (the inputs).
$ErrorActionPreference = 'Stop'
$out = $env:WHOLEOUT
$kpp = (Resolve-Path -LiteralPath $env:KPP).Path
$split = { param($text) if ($text) { @($text -split '\s+' | Where-Object { $_ -ne '' }) } else { @() } }
$common = @('--no-std', '--quiet', '--convert-marks', '--convert-native-tests') + (& $split $env:WHOLEOPTS)
$inputs = @('--convert-base', $env:FACTORIO) + (& $split $env:EXT) + @('--overrides', 'overrides') + (& $split $env:CLIBUSE) + (& $split $env:WHOLEINPUTS)
# A combination's name: v, then 0 or 1 for STEAM, DEMO, HEADLESS and SPACE_AGE.
$combinations = foreach ($steam in 0, 1) { foreach ($demo in 0, 1) { foreach ($headless in 0, 1) { foreach ($spaceAge in 0, 1) {
  $defines = @()
  if ($steam) { $defines += '--convert-define', 'STEAM' }
  if ($demo) { $defines += '--convert-define', 'DEMO' }
  if ($headless) { $defines += '--convert-define', 'HEADLESS' }
  if (-not $spaceAge) { $defines += '--convert-undefine', 'SPACE_AGE' }
  if ($steam -or $demo) { $defines += '--convert-define', 'DISABLE_UPDATER' }
  [pscustomobject]@{ Name = "v$steam$demo$headless$spaceAge"; Defines = $defines }
} } } }
function Remove-Tree($path) { if (Test-Path -LiteralPath $path) { cmd /c rd /s /q "$path" } }
foreach ($c in $combinations) { Remove-Tree "$out-$($c.Name)" }
$running = @()
$failed = @()
foreach ($c in $combinations) {
  while (@($running | Where-Object { -not $_.HasExited }).Count -ge 4) { Start-Sleep -Milliseconds 200 }
  $arguments = $common + $c.Defines + @('--convert', "$out-$($c.Name)") + $inputs
  $p = Start-Process -FilePath $kpp -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput "$out-$($c.Name).out" -RedirectStandardError "$out-$($c.Name).err"
  $null = $p.Handle # (kept, or ExitCode is lost once it exits)
  $p | Add-Member -NotePropertyName Variant -NotePropertyValue $c.Name
  $running += $p
}
foreach ($p in $running) {
  $p.WaitForExit()
  if ($p.ExitCode -ne 0) { $failed += $p.Variant }
}
if ($failed) { foreach ($f in $failed) { Get-Content "$out-$f.err" -ErrorAction SilentlyContinue | Select-Object -Last 20 }; Write-Output "variants: the conversion failed for $($failed -join ', ')"; exit 1 }
foreach ($c in $combinations) { Remove-Item -LiteralPath "$out-$($c.Name).out", "$out-$($c.Name).err" -ErrorAction SilentlyContinue }
# The merges, the innermost constant first: each pair's names without the last digit.
$names = @($combinations | ForEach-Object { $_.Name })
foreach ($condition in 'config::spaceAge', 'config::headless', 'config::demo', 'config::steam') {
  $next = @()
  for ($i = 0; $i -lt $names.Count; $i += 2) {
    $off = $names[$i]; $on = $names[$i + 1]
    $merged = $off.Substring(0, $off.Length - 1)
    if ($merged -eq 'v') { $merged = 'vall' }
    $log = "$out-$merged.log"
    & $kpp --convert-merge-flag "$out-$off" "$out-$on" "$out-$merged" $condition > $log 2>&1
    if ($LASTEXITCODE -ne 0) { Get-Content $log; Write-Output "variants: the merge by $condition of $off and $on failed"; exit 1 }
    if (-not $env:VARIANTS_KEEP) { Remove-Tree "$out-$off"; Remove-Tree "$out-$on" } # (VARIANTS_KEEP=1: each step kept, to see where a text came from)
    Remove-Item -LiteralPath $log
    $next += $merged
  }
  $names = $next
}
Remove-Tree $out
if (Test-Path -LiteralPath $out) { Write-Output "variants: $out cannot be removed (a process has a file or its directory open)"; exit 1 }
Move-Item -LiteralPath "$out-vall" -Destination $out
exit 0
