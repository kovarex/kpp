# kpp's C++ back end (--via-cpp, src\cppwriter.cpp, docs\cpp-backend.md), when this kpp has LLVM in it
# (kpp --version): tests\viacpp\program.kpp built plainly and through C++, the two programs' outputs and exit
# codes the same (virtual calls, containers, exceptions caught by a base, thrown on, and passing a destructor,
# integer wraparound, floating point), and each module written as C++ beside its IR. The whole specification
# suite through C++ is `spec.cmd --via-cpp`. run_tests.cmd runs it. Exit 0: passed; 1: failed; 3: skipped (no
# LLVM in this kpp).
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
if (-not ((& $KppExe --version) -match "^back end: LLVM's")) { "skipped: this kpp has no LLVM in it"; exit 3 }
$work = Join-Path $PSScriptRoot '..\build\tests\viacpp'
if (Test-Path $work) { cmd /c rd /s /q $work }
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$source = (Resolve-Path "$PSScriptRoot\viacpp\program.kpp").Path
$failed = @()
$outputs = @{}
foreach ($build in @(@{ name = 'plain'; options = @() }, @{ name = 'cpp'; options = @('--via-cpp') }))
{
  $exe = "$work\$($build.name).exe"
  $said = & $KppExe --quiet --modules 4 @($build.options) --exe $exe $source 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { $failed += "$($build.name): the build failed: $said"; continue }
  $text = & $exe 2>&1 | Out-String
  $outputs[$build.name] = "exit $LASTEXITCODE`n$text"
}
if (-not $failed.Count)
{
  if ($outputs['plain'] -ne $outputs['cpp']) { $failed += "the program built through C++ prints`n$($outputs['cpp'])`nand built plainly`n$($outputs['plain'])" }
  if (-not $outputs['plain'].Contains('outer failure')) { $failed += "the program did not run to its end: $($outputs['plain'])" }
  if (-not (Test-Path "$work\cpp.exe.0.cpp")) { $failed += "no module was written as C++ ($work\cpp.exe.0.cpp)" }
  if (Test-Path "$work\plain.exe.0.cpp") { $failed += "a plain build wrote C++ ($work\plain.exe.0.cpp)" }
}
foreach ($f in $failed) { "FAIL via C++: $f" }
if (-not $failed.Count) { "ok   via C++: a program built through kpp's C++ back end prints what its plain build prints" }
exit [int]($failed.Count -ne 0)
