# Edit-build latency inside Visual Studio, and the Error List (tools/vs/README.md, docs/build.md).
# Starts its own Visual Studio on a project, drives it through DTE: one build to warm the compile
# server, then Rounds times an edit of EditFile (a comment appended) and Build (as Ctrl+Shift+B),
# timed from the call to the build's end. Then a line that does not compile, a build, the Error
# List's items, and the first item navigated to (the editor's file and line). EditFile gets its
# text back and Visual Studio quits.
#   powershell -File tools\vs\measure.ps1 -Project <a .vcxproj kppc generated> -EditFile <one of its sources>
param([Parameter(Mandatory)][string]$Project, [Parameter(Mandatory)][string]$EditFile, [int]$Rounds = 5,
      [string]$Configuration = "Debug")
$ErrorActionPreference = "Stop"
$Project = (Resolve-Path $Project).Path
$EditFile = (Resolve-Path $EditFile).Path

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
// Retries the calls Visual Studio rejects while it is busy (RPC_E_CALL_REJECTED).
[ComImport, Guid("00000016-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IOleMessageFilter
{
  [PreserveSig] int HandleInComingCall(int callType, IntPtr caller, int tickCount, IntPtr info);
  [PreserveSig] int RetryRejectedCall(IntPtr callee, int tickCount, int rejectType);
  [PreserveSig] int MessagePending(IntPtr callee, int tickCount, int pendingType);
}
public class KppMessageFilter : IOleMessageFilter
{
  [DllImport("ole32.dll")] static extern int CoRegisterMessageFilter(IOleMessageFilter filter, out IOleMessageFilter old);
  public static void Register() { IOleMessageFilter old; CoRegisterMessageFilter(new KppMessageFilter(), out old); }
  public int HandleInComingCall(int callType, IntPtr caller, int tickCount, IntPtr info) { return 0; }
  public int RetryRejectedCall(IntPtr callee, int tickCount, int rejectType) { return rejectType == 2 && tickCount < 30000 ? 100 : -1; } // busy: retry for 30 s (a modal dialog blocks it for good)
  public int MessagePending(IntPtr callee, int tickCount, int pendingType) { return 2; }
}
// The DTE of one Visual Studio process, from the running object table ("!VisualStudio.DTE.17.0:PID").
public static class KppRot
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
[KppMessageFilter]::Register()

$devenv = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\devenv.exe"
$watch = [Diagnostics.Stopwatch]::StartNew()
$process = Start-Process $devenv -ArgumentList "`"$Project`"" -PassThru
$dte = $null
while (-not $dte -and $watch.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 250; $dte = [KppRot]::Dte($process.Id) }
if (-not $dte) { throw "no DTE for devenv $($process.Id)" }
while (($dte.Solution.Projects.Count -lt 1) -and $watch.Elapsed.TotalSeconds -lt 180) { Start-Sleep -Milliseconds 250 }
"Visual Studio up with the project: {0:N1} s" -f $watch.Elapsed.TotalSeconds
$dte.Solution.SolutionBuild.SolutionConfigurations.Item($Configuration).Activate()

$original = [IO.File]::ReadAllText($EditFile)
function Build()
{
  $t = [Diagnostics.Stopwatch]::StartNew()
  $dte.Solution.SolutionBuild.Build($true)
  [pscustomobject]@{ Seconds = $t.Elapsed.TotalSeconds; Failed = $dte.Solution.SolutionBuild.LastBuildInfo }
}
try
{
  $first = Build
  "first build (starts the compile server): {0:N2} s, failed projects {1}" -f $first.Seconds, $first.Failed
  $times = @()
  for ($i = 0; $i -lt $Rounds; $i++)
  {
    [IO.File]::AppendAllText($EditFile, "`n// measure.ps1 edit $i`n")
    $b = Build
    $times += $b.Seconds
    "edit-build {0}: {1:N2} s, failed projects {2}" -f $i, $b.Seconds, $b.Failed
  }
  $sorted = $times | Sort-Object
  "median edit-build: {0:N2} s" -f $sorted[[int][math]::Floor($sorted.Count / 2)]

  # An error, in the Error List, navigated to.
  $lines = ([IO.File]::ReadAllText($EditFile) -split "`n").Count
  [IO.File]::AppendAllText($EditFile, "`nint measureBroken() { int x = `"not an int`"; return x; }`n")
  $b = Build
  "broken build: {0:N2} s, failed projects {1}" -f $b.Seconds, $b.Failed
  $dte.ExecuteCommand("View.ErrorList")
  Start-Sleep -Seconds 2
  $items = $dte.ToolWindows.ErrorList.ErrorItems
  "Error List: {0} item(s)" -f $items.Count
  for ($i = 1; $i -le [math]::Min($items.Count, 5); $i++)
  {
    $e = $items.Item($i)
    "  [{0}] {1}({2}): {3}" -f $e.ErrorLevel, $e.FileName, $e.Line, $e.Description
  }
  if ($items.Count -ge 1)
  {
    $items.Item(1).Navigate()
    Start-Sleep -Seconds 1
    "navigated to {0} line {1} (the broken line is {2})" -f $dte.ActiveDocument.FullName, $dte.ActiveDocument.Selection.CurrentLine, ($lines + 1)
  }
}
finally
{
  [IO.File]::WriteAllText($EditFile, $original)
  $dte.Quit()
}
