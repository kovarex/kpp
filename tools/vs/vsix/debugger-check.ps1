# kpp's `this.` in Visual Studio's debugger (docs/debugging.md, "this in the watch window"): debugs
# debugger-test in the experimental instance (/rootsuffix Exp; kpp-visualstudio.vsix installed by test.ps1
# before) through DTE: expressions, conditional breakpoints, setting a value, the Immediate window, Locals,
# natvis in a kpp frame and stepping. Prints one line per check; the last says whether all passed, and
# the exit code agrees. Not part of run_tests, which has no Visual Studio session.
param([string]$KppExe, [string]$Work)
$ErrorActionPreference = 'Stop'
$test = [IO.File]::ReadAllText("$PSScriptRoot\test.ps1")
$a = $test.IndexOf('Add-Type @"'); $b = $test.IndexOf('[KppVsMessageFilter]::Register()')
Invoke-Expression $test.Substring($a, $b - $a)
[KppVsMessageFilter]::Register()
$ide = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $KppExe) { $KppExe = "$root\compiler\build\kpp.exe" }
if (-not $Work) { $Work = "$root\compiler\build\vs-debugger" }
if (Test-Path $Work) { cmd /c rd /s /q "$Work" }
New-Item -ItemType Directory -Force $Work | Out-Null
Copy-Item "$PSScriptRoot\debugger-test\*" $Work
$env:KPP_LSP_SERVER = $KppExe
Push-Location $Work
& $KppExe --generate-visual-studio-files | Out-Null
if ($LASTEXITCODE -ne 0) { "debugger-check: FAILED: kpp --generate-visual-studio-files exit $LASTEXITCODE"; exit 1 }
Pop-Location
$Sln = "$Work\build\visual-studio\$(Split-Path $Work -Leaf).sln" # (named after its directory)
$Main = "$Work\main.kpp"
$bodyLine = 8
$script:failed = 0
function Check($what, $ok, $detail)
{
  if ($ok) { "ok   $what ($detail)" } else { "FAIL $what ($detail)"; $script:failed++ }
}
$devenv = Start-Process "$ide\devenv.exe" -ArgumentList '/rootsuffix', 'Exp', "`"$Sln`"" -PassThru
$dte = $null; $t = [Diagnostics.Stopwatch]::StartNew()
while (-not $dte -and $t.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 250; $dte = [KppVsRot]::Dte($devenv.Id) }
while ($dte.Solution.Projects.Count -lt 1 -and $t.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 250 }
function WaitStop($seconds)
{
  $t = [Diagnostics.Stopwatch]::StartNew()
  Start-Sleep -Milliseconds 500
  while ($t.Elapsed.TotalSeconds -lt $seconds)
  {
    $m = $dte.Debugger.CurrentMode
    if ($m -eq 1 -or $m -eq 2) { return $m }
    Start-Sleep -Milliseconds 300
  }
  return 0
}
function Value($e) { $dte.Debugger.GetExpression($e, $true, 5000) }
function Expect($e, $want)
{
  $x = Value $e
  Check "$e" ($x.IsValidValue -and $x.Value -like $want) "$($x.Type) = $($x.Value)"
}
function Conditional($condition, $stops)
{
  foreach ($bp in @($dte.Debugger.Breakpoints)) { $bp.Delete() }
  $dte.Debugger.Breakpoints.Add('', $Main, $bodyLine, 1, $condition, 1) | Out-Null
  $dte.Debugger.Go($false)
  $mode = WaitStop 40
  Check "breakpoint condition $condition" ($mode -eq $(if ($stops) { 2 } else { 1 })) "mode $mode"
  if ($dte.Debugger.CurrentMode -ne 1) { $dte.Debugger.Stop($true); WaitStop 20 | Out-Null }
}
try
{
  $build = $dte.Solution.SolutionBuild
  $build.BuildProject($build.ActiveConfiguration.Name, @($build.StartupProjects)[0], $true)
  Check 'build' ($build.LastBuildInfo -eq 0) "failed projects $($build.LastBuildInfo)"
  if ($build.LastBuildInfo) { return }
  Conditional 'this.side == 7' $true
  Conditional 'this.side == 8' $false
  foreach ($bp in @($dte.Debugger.Breakpoints)) { $bp.Delete() }
  $dte.Debugger.Breakpoints.Add('', $Main, $bodyLine) | Out-Null
  $dte.Debugger.Go($false)
  $mode = WaitStop 40
  Check 'stops in the member function' ($mode -eq 2 -and $dte.Debugger.CurrentStackFrame.FunctionName -eq 'Square::area') "mode $mode, $($dte.Debugger.CurrentStackFrame.FunctionName)"
  Check "a kpp frame's language" ($dte.Debugger.CurrentStackFrame.Language -eq 'kpp') $dte.Debugger.CurrentStackFrame.Language
  Expect 'this.side' '7'
  Expect 'this.side * 2' '14'
  Expect 'this->side' '7'
  Expect 'side' '7'
  Expect 'this.area()' '49'
  Expect 'this.side,x' '0x00000007'
  Expect '&this' '0x*{side=7 }'
  Expect '*this' '{side=7 }'
  Expect '"this.side"' '"this.side"'
  $x = Value 'this.nothing'
  Check 'this.nothing is an error' (-not $x.IsValidValue) $x.Value
  $locals = @($dte.Debugger.CurrentStackFrame.Locals | ForEach-Object { $_.Name })
  Check 'Locals' ($locals -contains 'this' -and $locals -contains 'result') ($locals -join ', ')
  (Value 'this.side').Value = '9'
  Expect 'this.side' '9'
  $dte.Debugger.ExecuteStatement('this.side = 11', 5000, $true)
  Expect 'this.side' '11'
  $dte.Debugger.StepOver($true)
  $mode = WaitStop 20
  Check 'step over' ($mode -eq 2) "mode $mode, result = $((Value 'result').Value)"
  $dte.Debugger.StepOut($true)
  $mode = WaitStop 20
  Check 'step out' ($mode -eq 2 -and $dte.Debugger.CurrentStackFrame.FunctionName -like '*main') "mode $mode, $($dte.Debugger.CurrentStackFrame.FunctionName)"
  Expect 'square.side' '11'
  Expect 'square.area()' '121'
  Expect 'numbers' '{ length=3 }'
  Expect 'numbers.items[2]' '3'
  Expect 'name' '"kpp"'
  Expect 'queue' '{ length=1 }'
  Expect 'corner' 'corner 1 2'
  # A value's members by their rules when it is expanded, the program's and kpp's (until 2026-11-12 they
  # showed raw in a kpp frame: a MapPosition's x as {value=384}).
  $members = @((Value 'held').DataMembers | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', '
  Check "a value's members by their rules" ($members -eq 'corner=corner 1 2, label="held"') $members
  # A set of flags: none, one, several and a bit no flag names (a chain of natvis views, natvis.cpp).
  Expect 'none' '{}'
  Expect 'one' 'Green'
  Expect 'many' 'Red | Blue | 64'
  Expect 'wide' 'F0 | F1 | F2 | F3 | F4 | F5 | F6 | F7 | F8 | F9 | F10 | F11 | F12 | F13 | F14 | F15 | F16 | F17 | F18 | F19'
  # A global read by a view: a handle's name in a template's static list (the game's ID<T>).
  Expect 'handle' '{1, gear}'
  Expect 'none0' '{0}'
  Expect 'outside' 'assert failed: handle.index < Registry<T>::all.size()'
  # The game's SignalIDBase through its typeToStr and getName: a generic lambda per type, a Handle made from the index.
  Expect 'emptySignal' '{part }'
  Expect 'gearSignal' '{part gear}'
  Expect 'waterSignal' '{fluid water}'
  Expect 'badSignal' 'reaches abort()'
  # A container's elements by its own begin() and end() (a ring's index iterator wrapping, a pointer, a sentinel list).
  foreach ($case in @(@('ring', 'count=3, [0]=10, [1]=20, [2]=30, [Raw View]='), @('shelf', 'size=3, [0]=7, [1]=8, [2]=9, [Raw View]=')))
  {
    $rows = @((Value $case[0]).DataMembers | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', '
    Check "$($case[0])'s members and elements" ($rows.Contains($case[1])) $rows
  }
  $links = @((Value 'chain').DataMembers)
  $values = @($links | Where-Object { $_.Name -like '`[?`]' } | ForEach-Object { @($_.DataMembers | Where-Object { $_.Name -eq 'value' })[0].Value }) -join ', '
  Check "chain's elements" ($values -eq '1, 2') $values
  # A class's own listing of its children (debugItems): a tree's line and its children.
  Expect 'tree' '{ root, List }'
  $rows = @((Value 'tree').DataMembers | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', '
  Check "tree's children" ($rows.Contains('[0]={ a, 4, Leaf }, [1]={ b, 5, Leaf }')) $rows
  $rows = @((Value 'many').DataMembers | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', '
  Check "a set's rows" ($rows -like 'Red=true, Green=false, Blue=true, `[bits`]=0x61*') $rows
  $stack = @($dte.Debugger.CurrentThread.StackFrames | ForEach-Object { "$($_.Language)" })
  Check "C++ frames stay C++" ($stack -contains 'C++') ($stack -join ', ')
  $dte.Debugger.Stop($true)
}
catch { Check 'the run' $false "threw: $($_.Exception.Message), line $($_.InvocationInfo.ScriptLineNumber)" }
finally
{
  try { $dte.Documents.CloseAll(2) } catch { }
  try { $dte.Quit() } catch { }
  if (-not $devenv.WaitForExit(60000)) { Stop-Process -Id $devenv.Id -Force -ErrorAction SilentlyContinue }
}
if ($script:failed) { "debugger-check: $($script:failed) FAILED"; exit 1 }
"debugger-check: ALL PASSED"
exit 0
