# A virtual call that no override can change is a direct call in the IR, as C++ compilers make it: through a final
# class, or to a function marked final (spec classes/call_through_final_class_or_function_is_direct, whose output the
# spec checks). This checks the calls themselves: the function named, no vtable slot loaded; a call through a class
# that is not final stays one through the vtable. run_tests.cmd runs it.
param([string]$KppExe = "$PSScriptRoot\..\build\kpp.exe")
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot '..\build\tests\finalcalls'
New-Item -ItemType Directory -Force $work | Out-Null
$work = (Resolve-Path $work).Path
$KppExe = (Resolve-Path $KppExe).Path
$source = (Resolve-Path "$PSScriptRoot\spec\classes\call_through_final_class_or_function_is_direct.kpp").Path
$failed = @()

& $KppExe --quiet --emit-llvm "$work\final.ll" --exe "$work\final.exe" $source 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$work\final.ll")) { "FAIL final calls: the snippet does not build (exit $LASTEXITCODE)"; exit 1 }
$ir = Get-Content -Raw "$work\final.ll"

function Body([string]$name)
{
  $m = [regex]::Match($ir, '(?ms)^define [^\n]*@"' + [regex]::Escape($name) + '"\(.*?^\}')
  if (-not $m.Success) { return $null }
  return $m.Value
}

foreach ($case in @(
    @('viaFinalClass(Base&)', 'Leaf::update('),
    @('viaFinalFunction(Deeper&)', 'Open::update('),
    @('idOf(Both&)', 'Both::id(')))
{
  $body = Body $case[0]
  if (-not $body) { $failed += "$($case[0]): not in the IR"; continue }
  if (-not $body.Contains('@"' + $case[1])) { $failed += "$($case[0]): no direct call of $($case[1])...)" }
  if ($body -match 'call [^\n]*%t\d+\(') { $failed += "$($case[0]): still calls through a loaded slot" }
}
$base = Body 'viaBase(Base&)'
if (-not $base -or $base -notmatch 'call [^\n]*%t\d+\(') { $failed += 'viaBase(Base&): a call through a class that is not final is no longer one through the vtable' }

if ($failed.Count) { $failed | ForEach-Object { "FAIL final calls: $_" }; exit 1 }
"ok   final calls: a call through a final class or to a final function is direct; through another class it stays virtual"
exit 0
