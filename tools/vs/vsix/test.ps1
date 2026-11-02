# kpp-visualstudio.vsix in a real Visual Studio (tools/vs/vsix/README.md): installs it into the experimental
# instance (/rootsuffix Exp, a profile of its own), opens a copy of a project as a folder, and drives
# Visual Studio through DTE: Edit.GoToDefinition and Edit.GoToDeclaration (where the caret lands), an
# error typed into a file (the Error List), and a screenshot of the window. Prints one line per check.
#   test.ps1 -Name cells -Project compiler\tests\lsp\cells -File src\game\main.kpp -Line 6 -Column 12 -Definition src\common\shared.kpp:1
# -Definition and -Declaration are FILE:LINE, relative to the project; -Declaration defaults to -Definition.
# -Solution opens the solution kppc --generate-visual-studio-files writes instead of the folder; with
# -CellError TEXT it then checks the cells (docs/editors.md, "Cells"): -CellConfiguration as the active
# configuration must show an error containing TEXT, -OtherTarget as the startup project must not.
#   test.ps1 -Name cells-solution -Project compiler\tests\lsp\cells -File src\game\main.kpp -Line 6 -Column 12 -Definition src\common\shared.kpp:1 -Solution -CellError 'debug only' -OtherTarget tool
param(
  [Parameter(Mandatory)][string]$Name,
  [Parameter(Mandatory)][string]$Project,
  [Parameter(Mandatory)][string]$File,
  [Parameter(Mandatory)][int]$Line,
  [Parameter(Mandatory)][int]$Column,
  [Parameter(Mandatory)][string]$Definition,
  [string]$Declaration,
  [string]$Kppc, # default: the checkout's compiler\build\kppc.exe
  [string]$Vsix, # default: tools\vs\kpp-visualstudio.vsix (build.cmd)
  [string]$Work, # default: compiler\build\vs: the project's copy, the log, the screenshot
  [int]$Timeout = 120,
  [switch]$Solution,
  [string]$CellError,
  [string]$CellConfiguration = 'Release',
  [string]$OtherTarget
)
$ErrorActionPreference = 'Stop'
$checkout = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $Kppc) { $Kppc = "$checkout\compiler\build\kppc.exe" }
if (-not $Vsix) { $Vsix = "$checkout\tools\vs\kpp-visualstudio.vsix" }
if (-not $Work) { $Work = "$checkout\compiler\build\vs" }
if (-not $Declaration) { $Declaration = $Definition }
New-Item -ItemType Directory -Force $Work | Out-Null
$Kppc = (Resolve-Path $Kppc).Path
$ide = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
$failures = 0
function Check($what, $ok, $detail)
{
  "{0} {1}: {2}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $what, $detail
  if (-not $ok) { $script:failures++; if ($devenv) { Snap 'failed' } }
}

# Into the experimental instance only: the user's own Visual Studio is not touched.
$p = Start-Process "$ide\VSIXInstaller.exe" -ArgumentList '/quiet', '/rootSuffix:Exp', "`"$((Resolve-Path $Vsix).Path)`"" -PassThru -Wait
Check 'VSIXInstaller /rootSuffix:Exp' ($p.ExitCode -eq 0) "exit $($p.ExitCode)"
# The experimental instance's pkgdefs and MEF cache brought up to date, so the new build's parts load.
$p = Start-Process "$ide\devenv.exe" -ArgumentList '/rootsuffix', 'Exp', '/updateconfiguration' -PassThru -Wait
Check 'devenv /rootsuffix Exp /updateconfiguration' ($p.ExitCode -eq 0) "exit $($p.ExitCode)"

# A test's Visual Studio still open on the copy would get the files opened below.
$open = Get-CimInstance Win32_Process -Filter "Name='devenv.exe'" | Where-Object { $_.CommandLine -like "*/rootsuffix Exp*$Work*" }
if ($open) { throw "test.ps1: an experimental Visual Studio is still open on $Work (process $($open.ProcessId -join ', ')): close it first" }

# A copy: the checks write build\ into the project.
$copy = "$Work\$Name"
if (Test-Path $copy) { cmd /c "rd /s /q `"$copy`"" }
robocopy $Project $copy /E /XD .git build bin .vs /NFL /NDL /NJH /NJS /NP /MT:16 | Out-Null
$open = $copy
if ($Solution)
{
  Push-Location $copy
  $generated = & $Kppc --generate-visual-studio-files 2>&1
  Pop-Location
  $open = (Get-ChildItem "$copy\build\visual-studio" -Filter *.sln | Select-Object -First 1).FullName
  Check 'kppc --generate-visual-studio-files' ([bool]$open) "$generated"
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
[ComImport, Guid("00000016-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IOleMessageFilter
{
  [PreserveSig] int HandleInComingCall(int callType, IntPtr caller, int tickCount, IntPtr info);
  [PreserveSig] int RetryRejectedCall(IntPtr callee, int tickCount, int rejectType);
  [PreserveSig] int MessagePending(IntPtr callee, int tickCount, int pendingType);
}
public class KppVsMessageFilter : IOleMessageFilter
{
  [DllImport("ole32.dll")] static extern int CoRegisterMessageFilter(IOleMessageFilter filter, out IOleMessageFilter old);
  public static void Register() { IOleMessageFilter old; CoRegisterMessageFilter(new KppVsMessageFilter(), out old); }
  public int HandleInComingCall(int callType, IntPtr caller, int tickCount, IntPtr info) { return 0; }
  public int RetryRejectedCall(IntPtr callee, int tickCount, int rejectType) { return rejectType == 2 && tickCount < 30000 ? 100 : -1; }
  public int MessagePending(IntPtr callee, int tickCount, int pendingType) { return 2; }
}
public static class KppVsRot
{
  [DllImport("ole32.dll")] static extern int GetRunningObjectTable(int reserved, out IRunningObjectTable rot);
  [DllImport("ole32.dll")] static extern int CreateBindCtx(int reserved, out IBindCtx ctx);
  public static object Dte(int pid)
  {
    IRunningObjectTable rot; IEnumMoniker monikers; IBindCtx ctx;
    GetRunningObjectTable(0, out rot); rot.EnumRunning(out monikers); CreateBindCtx(0, out ctx);
    var one = new IMoniker[1];
    while (monikers.Next(1, one, IntPtr.Zero) == 0)
    {
      string name; one[0].GetDisplayName(ctx, null, out name);
      if (name == "!VisualStudio.DTE.17.0:" + pid) { object o; rot.GetObject(one[0], out o); return o; }
    }
    return null;
  }
}
"@
[KppVsMessageFilter]::Register()
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
# Visual Studio's windows (a prompt too), rendered by themselves: never the screen, which other
# windows may cover.
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Collections.Generic; using System.Drawing; using System.Runtime.InteropServices; using System.Text;
public static class SnapAll {
  delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  struct RECT { public int L, T, R, B; }
  public static List<string> Save(uint pid, string prefix) {
    var result = new List<string>(); int i = 0;
    EnumWindows((h, p) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h)) return true;
      RECT r; GetWindowRect(h, out r); int w = r.R - r.L, hh = r.B - r.T;
      if (w < 50 || hh < 50) return true;
      var t = new StringBuilder(256); GetWindowText(h, t, 256);
      string path = prefix + "-" + (i++) + ".png";
      using (var b = new Bitmap(w, hh)) using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); b.Save(path); }
      result.Add(path + " '" + t + "' " + w + "x" + hh);
      return true; }, IntPtr.Zero);
    return result;
  }
}
'@
$shots = 0
function Snap($why) { if ($script:shots++ -lt 3) { [SnapAll]::Save([uint32]$devenv.Id, "$Work\$Name-$why") | % { "screenshot: $_" } } }

# The server the extension starts: this checkout's kppc (KPP_LSP_SERVER, read by the extension).
$env:KPP_LSP_SERVER = $Kppc
$watch = [Diagnostics.Stopwatch]::StartNew()
$devenv = Start-Process "$ide\devenv.exe" -ArgumentList '/rootsuffix', 'Exp', "`"$open`"" -PassThru
$dte = $null
while (-not $dte -and $watch.Elapsed.TotalSeconds -lt $Timeout) { Start-Sleep -Milliseconds 250; $dte = [KppVsRot]::Dte($devenv.Id) }
if (-not $dte) { throw "test.ps1: no DTE for devenv $($devenv.Id)" }
"Visual Studio up: {0:N1} s" -f $watch.Elapsed.TotalSeconds

function Open($relative, $line, $column)
{
  $dte.ItemOperations.OpenFile((Join-Path $copy $relative)) | Out-Null
  $dte.ActiveDocument.Selection.MoveToLineAndOffset($line, $column)
}
function Here
{
  $d = $dte.ActiveDocument
  if (-not $d) { return '(no document)' }
  $name = if ($d.FullName.StartsWith($copy, [StringComparison]::OrdinalIgnoreCase)) { $d.FullName.Substring($copy.Length + 1) } else { $d.FullName }
  "{0}:{1}" -f $name, $d.Selection.CurrentLine
}
# A command until the caret leaves FILE:LINE (the first answers wait for the server's first check).
function Go($command, $expected)
{
  $t = [Diagnostics.Stopwatch]::StartNew()
  $start = "{0}:{1}" -f ($File -replace '/', '\'), $Line
  $at = $start
  while ($t.Elapsed.TotalSeconds -lt $Timeout)
  {
    Open $File $Line $Column
    try { $dte.ExecuteCommand($command) } catch { }
    Start-Sleep -Milliseconds 500
    $at = Here
    if ($at -ne $start) { break }
    Start-Sleep -Seconds 1
  }
  Check $command ($at -ieq ($expected -replace '/', '\')) ("{0} from {1}:{2}, expected {3}; {4:N1} s" -f $at, $File, $Line, $expected, $t.Elapsed.TotalSeconds)
}

try
{
  Go 'Edit.GoToDefinition' $Definition
  Go 'Edit.GoToDeclaration' $Declaration

  # An error typed in: the Error List shows the server's diagnostic, and it goes after undo.
  Open $File $Line 1
  $dte.ActiveDocument.Selection.Insert("  kppUndeclaredName();`n")
  $document = $dte.ActiveDocument
  $dte.ExecuteCommand('View.ErrorList')
  # DTE's ErrorItems has the build's errors only: the Error List's rows are read as UI Automation sees them.
  function ErrorRow($text = 'kppUndeclaredName')
  {
    $window = [Windows.Automation.AutomationElement]::FromHandle($devenv.MainWindowHandle)
    $rows = $window.FindAll([Windows.Automation.TreeScope]::Descendants,
      [Windows.Automation.Condition]::TrueCondition)
    foreach ($row in $rows) { if ($row.Current.Name -like "*$text*") { return $row.Current.Name } }
    $null
  }
  $t = [Diagnostics.Stopwatch]::StartNew()
  $found = $null
  while (-not $found -and $t.Elapsed.TotalSeconds -lt $Timeout) { Start-Sleep -Milliseconds 500; $found = ErrorRow }
  Check 'a typed error in the Error List' ([bool]$found) $(if ($found) { "'{0}'; {1:N1} s" -f $found, $t.Elapsed.TotalSeconds } else { 'none' })
  $document.Undo() | Out-Null
  $document.Save() | Out-Null
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ((ErrorRow) -and $t.Elapsed.TotalSeconds -lt $Timeout) { Start-Sleep -Milliseconds 500 }
  Check 'the error gone after undo' (-not (ErrorRow)) ("{0:N1} s" -f $t.Elapsed.TotalSeconds)

  # The cells: the active configuration and the startup project pick the server's cell.
  if ($Solution -and $CellError)
  {
    $build = $dte.Solution.SolutionBuild
    $first = @($build.StartupProjects)[0]
    $firstName = [IO.Path]::GetFileNameWithoutExtension($first)
    function Wait($what, $present)
    {
      $t = [Diagnostics.Stopwatch]::StartNew()
      while ([bool](ErrorRow $CellError) -ne $present -and $t.Elapsed.TotalSeconds -lt $Timeout) { Start-Sleep -Milliseconds 500 }
      $row = ErrorRow $CellError
      Check $what ([bool]$row -eq $present) ("{0}; {1:N1} s" -f $(if ($row) { "'$row'" } else { 'no such error' }), $t.Elapsed.TotalSeconds)
    }
    $build.SolutionConfigurations.Item($CellConfiguration).Activate()
    Wait "$firstName, $CellConfiguration`: the error '$CellError'" $true
    if ($OtherTarget)
    {
      $build.StartupProjects = "$OtherTarget.vcxproj"
      Wait "$OtherTarget, $CellConfiguration`: no error '$CellError'" $false
      $build.StartupProjects = $first
    }
    $build.SolutionConfigurations.Item('Debug').Activate()
    Wait "$firstName, Debug: no error '$CellError'" $false
  }

  # Hover: Quick Info at the call, for the screenshot.
  Open $File $Line $Column
  try { $dte.ExecuteCommand('Edit.QuickInfo') } catch { }
  Start-Sleep -Seconds 3
  Snap 'end'
}
finally
{
  # The test's edits are not saved: a save prompt would keep Visual Studio open.
  try { $dte.Documents.CloseAll(2) } catch { }
  $dte.Quit()
  if (-not $devenv.WaitForExit(60000)) { "test.ps1: Visual Studio $($devenv.Id) did not quit" }
}
$log = "$env:LOCALAPPDATA\kpp\visual-studio-lsp.log"
if (Test-Path $log) { "the server's log: $log" }
"done, $failures failed"
if ($failures) { exit 1 }
exit 0
