namespace KppVisualStudio;

using System;
using System.ComponentModel.Design;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Task = System.Threading.Tasks.Task;

/// <summary>
/// kpp's commands in Tools > kpp (KppCommands.vsct, registered by kpp.pkgdef): Show Coverage, a check
/// that turns the coverage colours on and off (<see cref="Coverage"/>), and Load Coverage File....
/// Both can be given a key in Tools > Options > Keyboard (Tools.kppShowCoverage, Tools.kppLoadCoverageFile).
/// </summary>
[PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
[Guid(PackageGuid)]
public sealed class KppPackage : AsyncPackage
{
    public const string PackageGuid = "845d7c14-d316-4b0c-9ef2-efb3bf028d7b";
    public static readonly Guid CommandSet = new("ff9d7826-43e3-4c47-816c-1baa99d5c2e8");
    public const int ShowCoverageId = 0x0100;
    public const int LoadCoverageId = 0x0101;

    protected override async Task InitializeAsync(CancellationToken cancellationToken, IProgress<ServiceProgressData> progress)
    {
        await this.JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await this.GetServiceAsync(typeof(IMenuCommandService)) is not OleMenuCommandService commands)
            return;
        var show = new OleMenuCommand((_, _) => this.ToggleCoverage(), new CommandID(CommandSet, ShowCoverageId));
        show.BeforeQueryStatus += (_, _) => show.Checked = Coverage.Visible;
        commands.AddCommand(show);
        commands.AddCommand(new OleMenuCommand((_, _) => this.LoadCoverage(null), new CommandID(CommandSet, LoadCoverageId)));
    }

    /// <summary>
    /// Off when on. On: the file shown before, else the nearest kpp-coverage.json above the active
    /// document or the solution, else one picked.
    /// </summary>
    private void ToggleCoverage()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (Coverage.Visible)
        {
            Coverage.SetVisible(false);
            this.Status("kpp coverage: hidden");
            return;
        }
        if (Coverage.Source is not null && File.Exists(Coverage.Source))
        {
            this.LoadCoverage(Coverage.Source);
            return;
        }
        string? found = Coverage.Find(Path.GetDirectoryName(this.ActiveDocument() ?? "")) ?? Coverage.Find(this.SolutionFolder());
        this.LoadCoverage(found);
    }

    private void LoadCoverage(string? path)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (path is null)
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Title = "kpp: the coverage file to show",
                Filter = "kpp coverage (*.json)|*.json|All files (*.*)|*.*",
                FileName = "kpp-coverage.json",
                InitialDirectory = this.SolutionFolder() ?? "",
            };
            if (dialog.ShowDialog() != true)
                return;
            path = dialog.FileName;
        }
        try
        {
            this.Status(Coverage.Load(path));
        }
        catch (Exception e)
        {
            this.Status($"kpp coverage: {path} cannot be read: {e.Message}");
        }
    }

    private string? ActiveDocument()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        return this.GetService(typeof(EnvDTE.DTE)) is EnvDTE.DTE dte ? dte.ActiveDocument?.FullName : null;
    }

    private string? SolutionFolder()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (this.GetService(typeof(SVsSolution)) is IVsSolution solution && solution.GetSolutionInfo(out string directory, out _, out _) == 0 &&
            !string.IsNullOrEmpty(directory))
            return directory;
        return null;
    }

    private void Status(string text)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        KppLanguageClient.Log(text);
        if (this.GetService(typeof(SVsStatusbar)) is IVsStatusbar bar)
            bar.SetText(text);
    }
}
