# The readme's first three rows (readme.md, "Why kpp") measured on this machine with the kpp on the
# PATH: an edit of one function, an edit of an inline function used across the game, and a clean
# build of the game, on factorio-in-kpp's debug build and, given -Factorio, on Factorio's own C++
# Debug build with FASTBuild (docs/compile-speed.md, "Measure it yourself"):
#   powershell -NoProfile -ExecutionPolicy Bypass -File factorio-edits.ps1 -Checkout D:\factorio-in-kpp [-Factorio D:\Factorio] [-Rounds 3] [-DryRun]
#
# kpp, in the factorio-in-kpp checkout: `kpp factorio` once (its compile server started, the C and C++
# libraries built), then in each round
#   the leaf edit: a string in AnimationBank::getAnimation (src\AnimationBank.kpp), built, undone, built;
#   the wide edit: MapPosition::operator+'s operands swapped (src\Util\MapPosition.kpp), an inline
#     function used across the game, built, undone, built;
#   the clean build of the game: the compile server stopped and build\debug deleted (the game's
#     outputs; build\clib, the C and C++ libraries, and kppc's import cache %TEMP%\kpp-imports stay),
#     then `kpp factorio --no-server`, as the readme's row was measured (a server's first build is
#     slower than a build in its own process: docs/test-matrix.md, "Build time"); then, not timed,
#     `kpp factorio` starts the server the next round's edits use.
# C++, in the Factorio checkout (FASTBuild in fastbuild\, vs2022-Debugx64-factorio-run), as
# docs/test-matrix.md, "Build time", measured it, in the same rounds: one full build first, then
#   the leaf edit: src\AnimationBank.hpp touched (its time stamp: FASTBuild rebuilds what includes it as
#     for an edit, and the file's contents stay as they are);
#   the wide edit: src\ActionResult.hpp touched (the precompiled header includes it);
#   the clean build of the game: `FBuild -clean` of every library factorio-run links except the C and
#     C++ libraries factorio-in-kpp builds as C libraries (those stay built, as kpp's do), then
#     factorio-run (its link).
# Each time is the wall time of the build command from start to exit, the link included. Every edited
# file is restored after each build and at the end, also on an error or Ctrl+C; a file with changes of
# its own stops the script before anything is built. -DryRun checks everything and prints the plan,
# building and changing nothing. The logs go to the checkout's build\measure\.
param(
  [Parameter(Mandatory = $true)][string]$Checkout,
  [string]$Factorio = '',
  [int]$Rounds = 3,
  [string]$KppExe = '',
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
# git status refreshes no index: it writes nothing into either checkout.
$env:GIT_OPTIONAL_LOCKS = '0'

if ($Rounds -lt 1) { throw "-Rounds must be 1 or more" }

$edits = @(
  @{ Name = 'leaf'; File = 'src\AnimationBank.kpp'
     Old = 'Missing specification of animation with name'
     New = 'Missing specification of the animation with name' },
  @{ Name = 'wide'; File = 'src\Util\MapPosition.kpp'
     Old = 'operator+(const MapPosition& other) const { return MapPosition(this->x + other.x, this->y + other.y); }'
     New = 'operator+(const MapPosition& other) const { return MapPosition(other.x + this->x, other.y + this->y); }' }
)
$touches = @(
  @{ Name = 'leaf'; File = 'src\AnimationBank.hpp' },
  @{ Name = 'wide'; File = 'src\ActionResult.hpp' }
)
# What factorio-run links (fbuild.bff's factorioLibrariesPCH and factorio-run-files) less the libraries
# factorio-in-kpp's build.kpp builds as C libraries (cLibraries) and factorio-icon (its resource is in
# kpp-native there).
$cLibraries = 'sdl', 'sdl-mixer', 'curl', 'vorbis', 'opus', 'opusfile', 're2', 'imgui', 'open-vcdiff'
$gameLibraries = 'factorio-run-files', 'factorio', 'agui', 'agui-void', 'common-util', 'lua', 'stack-walker', 'tracy',
                 'minizip', 'trio', 'glad', 'fontstash', 'stb', 'ogg', 'zlib', 'crc32fast', 'zstd'
$cppTarget = 'vs2022-Debugx64-factorio-run'

function GitOutput([string]$directory) {
  $out = & git -C $directory @args
  if ($LASTEXITCODE -ne 0) { throw "git -C $directory $args failed ($LASTEXITCODE)" }
  $out
}

function Quote([string]$argument) {
  if ($argument -notmatch '[\s"]') { return $argument }
  '"' + ($argument -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

# Runs a build command, its output into $log, and returns its exit code and wall time. The compile
# server kpp starts inherits no handle, so the output ends with the client.
function Run([string]$exe, [string[]]$arguments, [string]$directory, [string]$log) {
  $info = New-Object Diagnostics.ProcessStartInfo
  $info.FileName = $exe
  $info.Arguments = ($arguments | ForEach-Object { Quote $_ }) -join ' '
  $info.WorkingDirectory = $directory
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $clock = [Diagnostics.Stopwatch]::StartNew()
  $process = [Diagnostics.Process]::Start($info)
  $out = $process.StandardOutput.ReadToEndAsync()
  $err = $process.StandardError.ReadToEndAsync()
  $process.WaitForExit()
  $clock.Stop()
  $text = $out.Result + $err.Result
  [IO.File]::WriteAllText($log, $text)
  [pscustomobject]@{ Code = $process.ExitCode; Seconds = $clock.Elapsed.TotalSeconds; Text = $text }
}

function Load { [int](Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average }

function Stamp([string]$file) {
  if (Test-Path -LiteralPath $file) { (Get-Item -LiteralPath $file).LastWriteTimeUtc } else { [DateTime]::MinValue }
}

# --- What is measured, checked before anything is built or changed --------------------------------

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git is not on the PATH" }
if ($KppExe -eq '') {
  $found = Get-Command kpp -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $found) { throw "kpp is not on the PATH: install it (docs/getting-started.md) or pass -KppExe <kpp.exe>" }
  $KppExe = $found.Source
}
$KppExe = (Resolve-Path -LiteralPath $KppExe).Path
$versionLines = & $KppExe --version
if ($LASTEXITCODE -ne 0) { throw "$KppExe --version failed ($LASTEXITCODE)" }

$Checkout = (Resolve-Path -LiteralPath $Checkout).Path
if (-not (Test-Path -LiteralPath (Join-Path $Checkout 'build.kpp'))) { throw "$Checkout has no build.kpp: -Checkout is a factorio-in-kpp checkout" }
$checkoutCommit = GitOutput $Checkout log -1 '--format=%h (%cs)'
$checkoutSubject = GitOutput $Checkout log -1 '--format=%s'
$converted = ''
if ($checkoutSubject -match 'Factorio ([0-9a-f]{7,40})') { $converted = ", converted from Factorio $($Matches[1])" }

$problems = @()
foreach ($e in $edits) {
  $e.Path = Join-Path $Checkout $e.File
  if (-not (Test-Path -LiteralPath $e.Path)) { $problems += "$($e.Path) is not there"; continue }
  if (GitOutput $Checkout status --porcelain -- ($e.File -replace '\\', '/')) { $problems += "$($e.Path) has changes of its own (git status): commit or restore it first" }
  $e.Bytes = [IO.File]::ReadAllBytes($e.Path)
  $text = [Text.Encoding]::UTF8.GetString($e.Bytes)
  $at = $text.IndexOf($e.Old, [StringComparison]::Ordinal)
  if ($at -lt 0 -or $text.LastIndexOf($e.Old, [StringComparison]::Ordinal) -ne $at) { $problems += "$($e.Path) does not hold `"$($e.Old)`" exactly once" }
  $e.Edited = [Text.Encoding]::UTF8.GetBytes($text.Replace($e.Old, $e.New))
}

$cpp = $Factorio -ne ''
if ($cpp) {
  $Factorio = (Resolve-Path -LiteralPath $Factorio).Path
  $fastbuild = Join-Path $Factorio 'fastbuild'
  $fbuild = Join-Path $fastbuild 'FBuild.exe'
  if (-not (Test-Path -LiteralPath $fbuild)) { throw "$fbuild is not there: -Factorio is a Factorio checkout" }
  $factorioCommit = GitOutput $Factorio log -1 '--format=%h (%cs)'
  foreach ($t in $touches) {
    $t.Path = Join-Path $Factorio $t.File
    if (-not (Test-Path -LiteralPath $t.Path)) { $problems += "$($t.Path) is not there"; continue }
    if (GitOutput $Factorio status --porcelain -- ($t.File -replace '\\', '/')) { $problems += "$($t.Path) has changes of its own (git status): commit or restore it first" }
  }
  $bff = [IO.File]::ReadAllText((Join-Path $fastbuild 'fbuild.bff'))
  foreach ($name in $gameLibraries) {
    if (-not $bff.Contains("('`$environmentName`$-`$TargetName`$-$name')")) { $problems += "fbuild.bff defines no ${name}: the game's libraries have changed" }
  }
  $cppExe = Join-Path $Factorio 'bin\Debugx64vs2022\factorio-run.exe'
}
if ($problems) { throw ("Nothing was built or changed:`n  " + ($problems -join "`n  ")) }

$cpu = Get-CimInstance Win32_Processor
$system = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$machine = "{0}, {1} cores, {2} threads, {3:N0} GB, {4}" -f ($cpu | Select-Object -First 1).Name.Trim(),
  ($cpu | Measure-Object NumberOfCores -Sum).Sum, ($cpu | Measure-Object NumberOfLogicalProcessors -Sum).Sum,
  ($system.TotalPhysicalMemory / 1GB), $os.Caption

$gameExe = Join-Path $Checkout 'build\debug\factorio.exe'
$logs = Join-Path $Checkout 'build\measure'
"kpp:       $KppExe"
$versionLines | ForEach-Object { "           $_" }
"machine:   $machine"
"checkout:  $Checkout at $checkoutCommit$converted"
if ($cpp) { "C++:       $Factorio at $factorioCommit, $fbuild $cppTarget" }
"rounds:    $Rounds, interleaved"
""
"It edits, and restores after each build and at the end:"
foreach ($e in $edits) { "  $($e.Path)`n    `"$($e.Old)`"`n    -> `"$($e.New)`"" }
"Clean build of the game, kpp: ``kpp --stop-server``, then $(Join-Path $Checkout 'build\debug') deleted (build\clib and"
"  %TEMP%\kpp-imports stay), then ``kpp factorio --no-server``; then, not timed, ``kpp factorio`` for the server."
if ($cpp) {
  "It touches (the time stamp only; the contents stay):"
  foreach ($t in $touches) { "  $($t.Path)" }
  "Clean build of the game, C++: FBuild -clean of $($gameLibraries.Count) libraries ($($gameLibraries -join ', ')),"
  "  then FBuild $cppTarget; built and kept: $($cLibraries -join ', ')."
}
"Logs: $logs"
""
if ($DryRun) { "Dry run: nothing built or changed."; return }

# --- The measurement ------------------------------------------------------------------------------

New-Item -ItemType Directory -Force $logs | Out-Null
$times = [ordered]@{}
foreach ($row in 'kpp leaf', 'kpp leaf undo', 'kpp wide', 'kpp wide undo', 'kpp clean', 'C++ leaf', 'C++ wide', 'C++ clean') { $times[$row] = @() }

function KppBuild([string]$label, [string]$log, [string[]]$more = @()) {
  $load = Load
  $before = Stamp $gameExe
  $r = Run $KppExe (@('factorio') + $more) $Checkout (Join-Path $logs "$log.txt")
  if ($r.Code -ne 0) { throw "kpp factorio failed ($($r.Code)) at $label; its output: $(Join-Path $logs "$log.txt")" }
  if ((Stamp $gameExe) -le $before) { throw "kpp factorio wrote no new $gameExe at ${label}: not a build of the change; its output: $(Join-Path $logs "$log.txt")" }
  $line = ($r.Text -split "`r?`n" | Where-Object { $_ -match '^kpp: built' } | Select-Object -Last 1)
  Write-Host ("{0,-16} {1,7:N2} s   (machine {2,3}% busy before)  {3}" -f $label, $r.Seconds, $load, $line)
  $r.Seconds
}

function FBuild([string[]]$arguments, [string]$label, [string]$log) {
  $r = Run $fbuild (@('-summary') + $arguments) $fastbuild (Join-Path $logs "$log.txt")
  if ($r.Code -ne 0) { throw "FBuild $arguments failed ($($r.Code)) at $label; its output: $(Join-Path $logs "$log.txt")" }
  $r
}

function CppBuild([string]$label, [string]$log, [switch]$Clean) {
  $load = Load
  $before = Stamp $cppExe
  $seconds = 0
  if ($Clean) {
    $seconds += (FBuild (@('-clean') + ($gameLibraries | ForEach-Object { "vs2022-Debugx64-$_" })) $label "$log-libraries").Seconds
  }
  $r = FBuild @($cppTarget) $label $log
  $seconds += $r.Seconds
  if ((Stamp $cppExe) -le $before) { throw "FBuild wrote no new $cppExe at ${label}: not a build of the change; its output: $(Join-Path $logs "$log.txt")" }
  $objects = ''
  if ($r.Text -match '(?m)^ - Object\s+:\s+\d+\s+(\d+)') { $objects = "objects built $($Matches[1])" }
  Write-Host ("{0,-16} {1,7:N2} s   (machine {2,3}% busy before)  {3}" -f $label, $seconds, $load, $objects)
  $seconds
}

function Restore {
  foreach ($e in $edits) {
    if ($null -ne $e.Bytes -and -not [Collections.StructuralComparisons]::StructuralEqualityComparer.Equals([IO.File]::ReadAllBytes($e.Path), $e.Bytes)) {
      [IO.File]::WriteAllBytes($e.Path, $e.Bytes)
    }
  }
}

$started = $false
try {
  "warm-up: kpp factorio (the compile server, the C and C++ libraries)"
  $r = Run $KppExe @('--stop-server') $Checkout (Join-Path $logs 'kpp-stop.txt')
  if ($r.Code -ne 0) { throw "kpp --stop-server failed ($($r.Code)); its output: $(Join-Path $logs 'kpp-stop.txt')" }
  $started = $true
  $r = Run $KppExe @('factorio') $Checkout (Join-Path $logs 'kpp-warm-up.txt')
  if ($r.Code -ne 0) { throw "kpp factorio failed ($($r.Code)); its output: $(Join-Path $logs 'kpp-warm-up.txt')" }
  "  {0:N1} s" -f $r.Seconds
  if ($cpp) {
    "warm-up: FBuild $cppTarget"
    "  {0:N1} s" -f (FBuild @($cppTarget) 'warm-up' 'cpp-warm-up').Seconds
  }
  for ($round = 1; $round -le $Rounds; $round++) {
    ""
    "round $round"
    foreach ($e in $edits) {
      [IO.File]::WriteAllBytes($e.Path, $e.Edited)
      $times["kpp $($e.Name)"] += KppBuild "kpp $($e.Name) edit" "kpp-$round-$($e.Name)"
      [IO.File]::WriteAllBytes($e.Path, $e.Bytes)
      $times["kpp $($e.Name) undo"] += KppBuild "kpp $($e.Name) undo" "kpp-$round-$($e.Name)-undo"
      if ($cpp) {
        $t = $touches | Where-Object { $_.Name -eq $e.Name }
        (Get-Item -LiteralPath $t.Path).LastWriteTime = Get-Date
        $times["C++ $($e.Name)"] += CppBuild "C++ $($e.Name) touch" "cpp-$round-$($e.Name)"
      }
    }
    $r = Run $KppExe @('--stop-server') $Checkout (Join-Path $logs "kpp-$round-stop.txt")
    if ($r.Code -ne 0) { throw "kpp --stop-server failed ($($r.Code)); its output: $(Join-Path $logs "kpp-$round-stop.txt")" }
    $debug = Join-Path $Checkout 'build\debug'
    if (Test-Path -LiteralPath $debug) { cmd /c rd /s /q $debug }
    if (Test-Path -LiteralPath $debug) { throw "$debug could not be deleted (a program holding a file in it?)" }
    $times['kpp clean'] += KppBuild 'kpp clean' "kpp-$round-clean" @('--no-server')
    if ($round -lt $Rounds) {
      $r = Run $KppExe @('factorio') $Checkout (Join-Path $logs "kpp-$round-server.txt")
      if ($r.Code -ne 0) { throw "kpp factorio failed ($($r.Code)); its output: $(Join-Path $logs "kpp-$round-server.txt")" }
      Write-Host ("{0,-16} {1,7:N2} s   (not timed: the compile server for the next round)" -f 'kpp server', $r.Seconds)
    }
    if ($cpp) {
      $times['C++ clean'] += CppBuild 'C++ clean' "cpp-$round-clean" -Clean
    }
  }
} finally {
  Restore
  if ($started) { Run $KppExe @('--stop-server') $Checkout (Join-Path $logs 'kpp-stop.txt') | Out-Null }
  $left = @($edits | Where-Object { GitOutput $Checkout status --porcelain -- ($_.File -replace '\\', '/') })
  if ($left) { Write-Warning "still changed after the restore: $(($left | ForEach-Object { $_.Path }) -join ', ')" }
  else { ""; "Restored: $(($edits | ForEach-Object { $_.File }) -join ', ') (git status clean)." }
}

# --- The results, in the readme's rows ------------------------------------------------------------

function Span([string]$row) {
  $values = $times[$row]
  if (-not $values) { return '' }
  $sorted = $values | Sort-Object
  "{0:N1} to {1:N1} s ({2})" -f $sorted[0], $sorted[-1], (($values | ForEach-Object { '{0:N2}' -f $_ }) -join ', ')
}
""
"kpp $(($versionLines | Select-Object -First 1) -replace '^kpp ', ''); $machine"
"factorio-in-kpp $checkoutCommit$converted; debug; $Rounds rounds; wall time, min to max (each round)"
$rows = @(
  @('Edit of one function', 'kpp leaf', 'C++ leaf'),
  @('  its undo', 'kpp leaf undo', ''),
  @('Edit of an inline function used across the game', 'kpp wide', 'C++ wide'),
  @('  its undo', 'kpp wide undo', ''),
  @('Clean build of the game (libraries built)', 'kpp clean', 'C++ clean')
)
foreach ($row in $rows) {
  "{0}`n    kpp: {1}" -f $row[0], (Span $row[1])
  if ($cpp -and $row[2]) { "    C++: {0}" -f (Span $row[2]) }
}
