# What Visual Studio shows of kpp values (docs/debugging.md, "Values in Visual Studio"): builds the
# solution's startup project in the experimental instance (/rootsuffix Exp; kpp-visualstudio.vsix
# installed by test.ps1 before), stops at Main:Line and prints each expression as the debugger gives
# it, the natvis applied, with Depth levels of members. A kpp project's generated solution, Debug.
#   natvis-check.ps1 -Sln build\visual-studio\x.sln -Main src\main.kpp -Line 36 -Exprs 'numbers,small,ages'
param([string]$Sln, [string]$Main, [int]$Line, [string]$Exprs, [int]$Depth = 1, [string]$Separator = ",")
$ErrorActionPreference = 'Stop'
$test = [IO.File]::ReadAllText("$PSScriptRoot\test.ps1")
$a = $test.IndexOf('Add-Type @"'); $b = $test.IndexOf('[KppVsMessageFilter]::Register()')
Invoke-Expression $test.Substring($a, $b - $a)
[KppVsMessageFilter]::Register()
$ide = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
$env:KPP_LSP_SERVER = (Resolve-Path "$PSScriptRoot\..\..\..\compiler\build\kppc.exe").Path
$Sln = (Resolve-Path $Sln).Path
$Main = (Resolve-Path $Main).Path
$devenv = Start-Process "$ide\devenv.exe" -ArgumentList '/rootsuffix', 'Exp', "`"$Sln`"" -PassThru
$dte = $null; $t = [Diagnostics.Stopwatch]::StartNew()
while (-not $dte -and $t.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 250; $dte = [KppVsRot]::Dte($devenv.Id) }
while ($dte.Solution.Projects.Count -lt 1 -and $t.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 250 }
function Show($x, $indent, $depth)
{
  foreach ($m in $x.DataMembers)
  {
    "{0}{1} : {2} = {3}" -f $indent, $m.Name, $m.Type, $m.Value
    if ($depth -gt 1) { Show $m ($indent + '    ') ($depth - 1) }
  }
}
try
{
  $build = $dte.Solution.SolutionBuild
  $build.BuildProject($build.ActiveConfiguration.Name, @($build.StartupProjects)[0], $true)
  "build: failed projects {0}" -f $build.LastBuildInfo
  if ($build.LastBuildInfo) { return }
  foreach ($bp in @($dte.Debugger.Breakpoints)) { $bp.Delete() }
  $dte.Debugger.Breakpoints.Add('', $Main, $Line) | Out-Null
  $dte.Debugger.Go($false)
  $t = [Diagnostics.Stopwatch]::StartNew(); while ($dte.Debugger.CurrentMode -ne 2 -and $t.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 200 }
  "break: mode {0}, {1:N1} s, frame {2}" -f $dte.Debugger.CurrentMode, $t.Elapsed.TotalSeconds, $dte.Debugger.CurrentStackFrame.FunctionName
  foreach ($e in ($Exprs.Split($Separator)))
  {
    $x = $dte.Debugger.GetExpression($e, $true, 5000)
    "== {0} : {1} = {2}" -f $e, $x.Type, $x.Value
    if ($x.IsValidValue) { Show $x '    ' $Depth }
  }
  $dte.Debugger.Stop($true)
}
finally
{
  try { $dte.Documents.CloseAll(2) } catch { }
  try { $dte.Quit() } catch { }
  # (Quit after a debugging session can leave it running, holding kppc.exe through its language
  # server: the instance this script started is ended.)
  if (-not $devenv.WaitForExit(60000)) { Stop-Process -Id $devenv.Id -Force -ErrorAction SilentlyContinue }
}
