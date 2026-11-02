namespace Kpp.VisualStudio;

using System;
using System.IO;
using System.Threading.Tasks;
using EnvDTE;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Newtonsoft.Json.Linq;

/// <summary>
/// The cell (docs/editors.md, "Cells") a solution kppc generated is in: its startup project names
/// the target (one project per target, named after it) and its active configuration the
/// configuration (Debug, or the cell's Visual Studio name). Opened as a folder, or in a solution
/// kppc did not write, there is none and the server keeps its own.
/// </summary>
internal sealed class Cell : IVsUpdateSolutionEvents, IVsSelectionEvents
{
    private const string GeneratedMarker = "generate-visual-studio-files";

    private readonly Func<JObject, Task> changed;
    private string? last;

    private Cell(Func<JObject, Task> changed) => this.changed = changed;

    /// <summary>
    /// The solution's cell now, null when it has none.
    /// </summary>
    public static JObject? Current()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (Package.GetGlobalService(typeof(DTE)) is not DTE dte || dte.Solution is null || !dte.Solution.IsOpen || string.IsNullOrEmpty(dte.Solution.FullName))
            return null;
        string directory = Path.GetDirectoryName(dte.Solution.FullName)!;
        bool kpp = false;
        foreach (Project project in dte.Solution.Projects)
            kpp = kpp || IsGenerated(project.FullName);
        string? configuration = dte.Solution.SolutionBuild.ActiveConfiguration?.Name;
        if (!kpp || configuration is null)
            return null;
        string? target = null;
        if (dte.Solution.SolutionBuild.StartupProjects is object[] { Length: 1 } startup && startup[0] is string unique)
        {
            string file = Path.Combine(directory, unique);
            if (IsGenerated(file))
                target = Path.GetFileNameWithoutExtension(file);
        }

        return new JObject { ["target"] = target, ["configuration"] = configuration };
    }

    /// <summary>
    /// Calls changed with the new cell whenever the active configuration or the startup project
    /// changes it.
    /// </summary>
    public static Cell Watch(JObject? initial, Func<JObject, Task> changed)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var cell = new Cell(changed) { last = initial?.ToString() };
        if (Package.GetGlobalService(typeof(SVsSolutionBuildManager)) is IVsSolutionBuildManager build)
            build.AdviseUpdateSolutionEvents(cell, out _);
        if (Package.GetGlobalService(typeof(SVsShellMonitorSelection)) is IVsMonitorSelection selection)
            selection.AdviseSelectionEvents(cell, out _);
        return cell;
    }

    private static bool IsGenerated(string? project)
    {
        if (string.IsNullOrEmpty(project) || !File.Exists(project))
            return false;
        using var reader = new StreamReader(project!);
        for (int i = 0; i < 5 && reader.ReadLine() is string line; ++i)
        {
            if (line.Contains(GeneratedMarker))
                return true;
        }

        return false;
    }

    private void Check()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        JObject? now = Current();
        if (now is null || now.ToString() == this.last)
            return;
        this.last = now.ToString();
#pragma warning disable VSSDK007 // A notification: the request's failures are on the status bar.
        ThreadHelper.JoinableTaskFactory.RunAsync(() => this.changed(now)).FileAndForget("kpp/Cell");
#pragma warning restore VSSDK007
    }

    public int OnActiveProjectCfgChange(IVsHierarchy pIVsHierarchy)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        this.Check();
        return VSConstants.S_OK;
    }

    public int OnElementValueChanged(uint elementid, object varValueOld, object varValueNew)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (elementid == (uint)VSConstants.VSSELELEMID.SEID_StartupProject)
            this.Check();
        return VSConstants.S_OK;
    }

    public int UpdateSolution_Begin(ref int pfCancelUpdate) => VSConstants.S_OK;

    public int UpdateSolution_Done(int fSucceeded, int fModified, int fCancelCommand) => VSConstants.S_OK;

    public int UpdateSolution_StartUpdate(ref int pfCancelUpdate) => VSConstants.S_OK;

    public int UpdateSolution_Cancel() => VSConstants.S_OK;

    public int OnSelectionChanged(IVsHierarchy pHierOld, uint itemidOld, IVsMultiItemSelect pMISOld, ISelectionContainer pSCOld, IVsHierarchy pHierNew, uint itemidNew, IVsMultiItemSelect pMISNew, ISelectionContainer pSCNew) => VSConstants.S_OK;

    public int OnCmdUIContextChanged(uint dwCmdUICookie, int fActive) => VSConstants.S_OK;
}
