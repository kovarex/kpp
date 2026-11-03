namespace KppVisualStudio;

using System;
using System.ComponentModel.Composition;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Editor;
using Microsoft.VisualStudio.OLE.Interop;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Editor;
using Microsoft.VisualStudio.TextManager.Interop;
using Microsoft.VisualStudio.Utilities;
using Newtonsoft.Json.Linq;

/// <summary>
/// Puts <see cref="GoToDeclaration"/> in front of every .kpp editor's commands.
/// </summary>
[Export(typeof(IVsTextViewCreationListener))]
[ContentType(KppContentType.Name)]
[TextViewRole(PredefinedTextViewRoles.Editable)]
internal sealed class KppTextViewListener : IVsTextViewCreationListener
{
    [Import]
    internal IVsEditorAdaptersFactoryService Adapters { get; set; } = null!;

    public void VsTextViewCreated(IVsTextView textViewAdapter)
    {
        IWpfTextView? view = this.Adapters.GetWpfTextView(textViewAdapter);
        if (view is null)
            return;
        var filter = new GoToDeclaration(view);
        if (textViewAdapter.AddCommandFilter(filter, out IOleCommandTarget next) == VSConstants.S_OK)
            filter.Next = next;
    }
}

/// <summary>
/// Edit.GoToDeclaration (Ctrl+F12) in a .kpp file. Visual Studio's language client sends
/// textDocument/definition for Edit.GoToDefinition but leaves this command unavailable, so this sends
/// textDocument/declaration on the client's connection and goes where the server answers.
/// </summary>
internal sealed class GoToDeclaration : IOleCommandTarget
{
    private readonly IWpfTextView view;

    public GoToDeclaration(IWpfTextView view) => this.view = view;

    public IOleCommandTarget? Next { get; set; }

    private static bool IsGoToDeclaration(Guid group, uint id) =>
        group == VSConstants.GUID_VSStandardCommandSet97 && id == (uint)VSConstants.VSStd97CmdID.GotoDecl;

    public int QueryStatus(ref Guid pguidCmdGroup, uint cCmds, OLECMD[] prgCmds, IntPtr pCmdText)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (cCmds == 1 && IsGoToDeclaration(pguidCmdGroup, prgCmds[0].cmdID))
        {
            prgCmds[0].cmdf = (uint)(OLECMDF.OLECMDF_SUPPORTED | OLECMDF.OLECMDF_ENABLED);
            return VSConstants.S_OK;
        }

        return this.Next?.QueryStatus(ref pguidCmdGroup, cCmds, prgCmds, pCmdText) ?? (int)Microsoft.VisualStudio.OLE.Interop.Constants.OLECMDERR_E_NOTSUPPORTED;
    }

    public int Exec(ref Guid pguidCmdGroup, uint nCmdID, uint nCmdexecopt, IntPtr pvaIn, IntPtr pvaOut)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (IsGoToDeclaration(pguidCmdGroup, nCmdID))
        {
            SnapshotPoint caret = this.view.Caret.Position.BufferPosition;
#pragma warning disable VSSDK007 // The command returns at once; the request's failures are on the status bar.
            ThreadHelper.JoinableTaskFactory.RunAsync(() => this.GoAsync(caret)).FileAndForget("kpp/GoToDeclaration");
#pragma warning restore VSSDK007
            return VSConstants.S_OK;
        }

        return this.Next?.Exec(ref pguidCmdGroup, nCmdID, nCmdexecopt, pvaIn, pvaOut) ?? (int)Microsoft.VisualStudio.OLE.Interop.Constants.OLECMDERR_E_NOTSUPPORTED;
    }

    private async System.Threading.Tasks.Task GoAsync(SnapshotPoint caret)
    {
        if (!this.view.TextBuffer.Properties.TryGetProperty(typeof(ITextDocument), out ITextDocument document))
            return;
        var rpc = KppLanguageClient.Rpc;
        if (rpc is null)
        {
            await StatusAsync("kpp: Go To Declaration: the language server is not running.");
            return;
        }

        ITextSnapshotLine line = caret.GetContainingLine();
        var request = new JObject
        {
            ["textDocument"] = new JObject { ["uri"] = new Uri(document.FilePath).AbsoluteUri },
            // Snapshot positions count UTF-16 code units, as the protocol's default columns do.
            ["position"] = new JObject { ["line"] = line.LineNumber, ["character"] = caret.Position - line.Start.Position },
        };
        JToken? answer;
        try
        {
            answer = await rpc.InvokeWithParameterObjectAsync<JToken?>("textDocument/declaration", request);
        }
        catch (Exception e)
        {
            await StatusAsync($"kpp: Go To Declaration failed: {e.Message}");
            return;
        }

        // Location, Location[] or LocationLink[]: the first one.
        JToken? target = answer is JArray list ? (list.Count > 0 ? list[0] : null) : answer;
        if (target is null || target.Type == JTokenType.Null)
        {
            await StatusAsync("kpp: no declaration found here.");
            return;
        }

        string? uri = (string?)(target["targetUri"] ?? target["uri"]);
        JToken? start = (target["targetSelectionRange"] ?? target["range"])?["start"];
        if (uri is null || start is null)
        {
            await StatusAsync("kpp: Go To Declaration: the server's answer has no location.");
            return;
        }

        int toLine = (int)start["line"]!;
        int toColumn = (int)start["character"]!;
        string file = PathOf(uri);
        await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
        try
        {
            VsShellUtilities.OpenDocument(ServiceProvider.GlobalProvider, file, Guid.Empty, out _, out _, out IVsWindowFrame frame, out IVsTextView targetView);
            frame?.Show();
            if (targetView is null)
            {
                await StatusAsync($"kpp: Go To Declaration: {file} did not open in a text editor.");
                return;
            }

            targetView.SetCaretPos(toLine, toColumn);
            targetView.CenterLines(toLine, 1);
        }
        catch (Exception e)
        {
            await StatusAsync($"kpp: Go To Declaration could not open {file}: {e.Message}");
        }
    }

    /// <summary>
    /// A file URI's path. The server escapes the drive's colon (file:///c%3A/...), which .NET
    /// Framework's Uri.LocalPath leaves as /c:/...: it is unescaped first.
    /// </summary>
    internal static string PathOf(string uri) => new Uri(Uri.UnescapeDataString(uri)).LocalPath;

    internal static async System.Threading.Tasks.Task StatusAsync(string text)
    {
        await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
        if (ServiceProvider.GlobalProvider.GetService(typeof(SVsStatusbar)) is IVsStatusbar bar)
            bar.SetText(text);
    }
}
