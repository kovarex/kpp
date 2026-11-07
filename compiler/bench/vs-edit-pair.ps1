# Visual Studio's edit build of the game, timed for two kpp binaries side by side (docs/build.md,
# "kpp's own steps in an edit build"): the command a Visual Studio build of factorio-in-kpp runs,
# `kpp factorio debug --msvc-diagnostics`, through its compile server. In alternating blocks, each
# block stops every binary's server, builds once untimed, then times -PerBlock rounds of an edit of
# one function (kpp measure's: src\AnimationBank.kpp), its undo, and a build with nothing changed.
# Prints each round (the machine's load, the three times, KPP_STEP_TIMES' line) and each binary's medians.
# Usage, in a factorio-in-kpp checkout (programs\factorio\setup-checkout.cmd makes one):
#   powershell -File compiler\bench\vs-edit-pair.ps1 -Checkout DIR -KppExes "BEFORE.exe,AFTER.exe" [-Blocks 3] [-PerBlock 3]
# Each kpp.exe must sit in its own compiler\build (it finds the standard library beside it): copy the
# first one's to compiler\build\kpp-before.exe before building the second.
param([Parameter(Mandatory = $true)][string]$Checkout, [Parameter(Mandatory = $true)][string]$KppExes, [int]$Blocks = 3, [int]$PerBlock = 3)
$ErrorActionPreference = 'Stop'
$binaries = @($KppExes -split ',') # (-File passes "BEFORE.exe,AFTER.exe" as one string)
Set-Location $Checkout
$file = Join-Path $Checkout 'src\AnimationBank.kpp'
$a = 'Missing specification of animation with name'
$b = 'Missing specification of the animation with name'
$text = [IO.File]::ReadAllText($file)
if (-not $text.Contains($a)) { throw "no '$a' in $file" }
$env:KPP_STEP_TIMES = '1'
$results = @{}
foreach ($k in $binaries) { $results[$k] = @{ edit = @(); nothing = @() } }
function Build([string]$k)
{
  $ErrorActionPreference = 'Continue'
  $t = Get-Date
  $out = & $k factorio debug --msvc-diagnostics 2>&1 | ForEach-Object { "$_" }
  $wall = ((Get-Date) - $t).TotalSeconds
  $ErrorActionPreference = 'Stop'
  if (-not ($out -match '^kpp: built')) { throw "the build failed:`n$($out -join "`n")" }
  return [pscustomobject]@{ wall = $wall; steps = ($out | Where-Object { $_ -match '^kpp steps' } | Select-Object -Last 1) }
}
function Median([double[]]$values) { $sorted = $values | Sort-Object; return $sorted[[int][Math]::Floor($sorted.Count / 2)] }
try
{
  for ($block = 0; $block -lt $Blocks; $block++)
  {
    foreach ($k in $binaries)
    {
      foreach ($other in $binaries) { & $other factorio debug --msvc-diagnostics --stop-server | Out-Null }
      Build $k | Out-Null
      for ($i = 0; $i -lt $PerBlock; $i++)
      {
        $load = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
        [IO.File]::WriteAllText($file, $text.Replace($a, $b))
        $e = Build $k
        [IO.File]::WriteAllText($file, $text)
        $u = Build $k
        $n = Build $k
        $results[$k].edit += $e.wall, $u.wall
        $results[$k].nothing += $n.wall
        "{0}  load {1,3}%  edit {2,5:N2} s  undo {3,5:N2} s  nothing {4,5:N3} s   {5}" -f $k, $load, $e.wall, $u.wall, $n.wall, $n.steps
      }
    }
  }
}
finally
{
  [IO.File]::WriteAllText($file, $text)
  foreach ($k in $binaries) { & $k factorio debug --msvc-diagnostics --stop-server | Out-Null }
}
foreach ($k in $binaries)
{
  "{0}: edit and undo median {1:N2} s (n {2}), nothing changed median {3:N3} s (n {4})" -f $k, (Median $results[$k].edit), $results[$k].edit.Count,
    (Median $results[$k].nothing), $results[$k].nothing.Count
}
