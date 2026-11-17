namespace KppVisualStudio;

using System;
using System.Threading.Tasks;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;

/// <summary>
/// A rename the server refuses, shown. Visual Studio's language client shows nothing when textDocument/rename or
/// textDocument/prepareRename fails, so Refactor.Rename seemed to do nothing (kovarex, 2026-11-17: renaming
/// InputAction::getType to tag, which the server refuses because tag() is already a member). The server's reason is
/// shown in a message box, and the extension's line is in the server's log.
/// </summary>
internal sealed class RenameRefusals : ILanguageClientMiddleLayer
{
    public bool CanHandle(string methodName) => methodName == "textDocument/rename" || methodName == "textDocument/prepareRename";

    public Task HandleNotificationAsync(string methodName, JToken methodParam, Func<JToken, Task> sendNotification) => sendNotification(methodParam);

    public async Task<JToken?> HandleRequestAsync(string methodName, JToken methodParam, Func<JToken, Task<JToken?>> sendRequest)
    {
        try
        {
            return await sendRequest(methodParam);
        }
        catch (RemoteInvocationException e)
        {
            KppLanguageClient.Log($"kpp: {methodName} refused, shown: {e.Message}");
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
            VsShellUtilities.ShowMessageBox(ServiceProvider.GlobalProvider, e.Message, "kpp: rename", OLEMSGICON.OLEMSGICON_WARNING,
                                            OLEMSGBUTTON.OLEMSGBUTTON_OK, OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
            return null;
        }
    }
}
