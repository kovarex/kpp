# The libraries' tests and kppsite's (kppnet\site) built for another target (targets.md): each
# project's `kpp --target T --sysroot S --no-run --test`, its test program copied to OUT\NAME.elf, and
# run.sh beside them (target-tests.sh), which runs them there from a checkout. A project that does
# not build is a failure unless target-known-failures.txt lists it for the target ("freebsd NAME");
# a listed one that builds fails too (its line must go). The last line says ALL PASSED or what failed.
# CI's libraries_freebsd jobs run it and run.sh in a FreeBSD VM (docs/build.md, "Continuous
# integration").
#   powershell -ExecutionPolicy Bypass -File libraries\target-tests.ps1 -Target freebsd [-Out DIR] [-Kppc PATH] [-Sysroot DIR]
param(
  [Parameter(Mandatory = $true)][ValidateSet('freebsd', 'linux')][string]$Target,
  [string]$Out = '',
  [string]$Kppc = '',
  [string]$Sysroot = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
# (the defaults here: an advanced script's parameter defaults do not see $PSScriptRoot in PowerShell 5.1)
if (-not $Out) { $Out = "$repo\compiler\build\libraries-$Target" }
if (-not $Kppc) { $Kppc = "$repo\compiler\build\kppc.exe" }
if (-not $Sysroot) { $Sysroot = "$repo\compiler\build\sysroots\$Target-x86_64" }
$Kppc = (Resolve-Path $Kppc).Path
$Sysroot = (Resolve-Path $Sysroot).Path
if (Test-Path $Out) { cmd /c rd /s /q $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$known = @(Get-Content "$PSScriptRoot\target-known-failures.txt" | Where-Object { $_ -match "^$Target " } | ForEach-Object { ($_ -split ' ')[1] })

$projects = @(Get-ChildItem -Directory "$repo\libraries" | ForEach-Object { , @($_.Name, "libraries/$($_.Name)") }) + @(, @('kppsite', 'kppnet/site'))
$failed = @()
$list = @()
$built = 0
foreach ($project in $projects)
{
  $name = $project[0]
  Push-Location (Join-Path $repo $project[1])
  $ErrorActionPreference = 'Continue'
  $log = & $Kppc --no-server --target $Target --sysroot $Sysroot --no-run --test 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  Pop-Location
  $log | Out-File -Encoding utf8 "$Out\$name.build.log"
  $elf = $log | ForEach-Object { if ($_ -match '^kppc --test: built (.*) \(not run: --no-run\)$') { $Matches[1] } } | Select-Object -First 1
  $ok = $code -eq 0 -and $elf -and (Test-Path $elf)
  if ($ok)
  {
    Copy-Item $elf "$Out\$name.elf"
    $list += "$($project[1]) $name"
    $built++
  }
  if ($ok -and $known -contains $name) { "FIXED $name builds for ${Target}: its line in target-known-failures.txt must go"; $failed += $name }
  elseif ($ok) { "ok   $name" }
  elseif ($known -contains $name) { "known $name (target-known-failures.txt)" }
  else { "FAIL $name (exit ${code}):"; $log | Select-Object -Last 10 | ForEach-Object { "    $_" }; $failed += $name }
}
# LF line ends for sh, whatever a Windows checkout made of the script.
[IO.File]::WriteAllText("$Out\projects.txt", (($list | ForEach-Object { "$_`n" }) -join ''))
[IO.File]::WriteAllText("$Out\run.sh", ((Get-Content -Raw "$PSScriptRoot\target-tests.sh") -replace "`r`n", "`n"))
if ($failed.Count) { "libraries ${Target} build: $($failed.Count) FAILED: $($failed -join ', ') ($built built)"; exit 1 }
"libraries ${Target} build: ALL PASSED ($built built, $($known.Count) known failures)"
