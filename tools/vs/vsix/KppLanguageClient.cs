namespace KppVisualStudio;

using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Threading;
using Microsoft.VisualStudio.Utilities;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;

/// <summary>
/// The content type of .kpp files: Visual Studio's language client serves it (its base type), the
/// TextMate grammar kpp.pkgdef registers colours it.
/// </summary>
internal static class KppContentType
{
    public const string Name = "kpp";

#pragma warning disable CS0649 // MEF reads these.
    [Export]
    [Name(Name)]
    [BaseDefinition(CodeRemoteContentDefinition.CodeRemoteContentTypeName)]
    internal static ContentTypeDefinition? Definition;

    [Export]
    [FileExtension(".kpp")]
    [ContentType(Name)]
    internal static FileExtensionToContentTypeDefinition? FileExtension;
#pragma warning restore CS0649
}

/// <summary>
/// IntelliSense for .kpp files: Visual Studio's language client talks to `kpp --lsp`
/// (docs/editors.md, "The language server") on its stdin and stdout. The connection is kept
/// (<see cref="Rpc"/>) for the requests Visual Studio does not send itself: textDocument/declaration
/// (<see cref="GoToDeclaration"/>).
/// </summary>
[ContentType(KppContentType.Name)]
[Export(typeof(ILanguageClient))]
[RunOnContext(RunningContext.RunOnHost)]
internal sealed class KppLanguageClient : ILanguageClient, ILanguageClientCustomMessage2
{
    /// <summary>
    /// The server's stderr (its log) goes here, written again at each start.
    /// </summary>
    public static string LogPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "kpp", "visual-studio-lsp.log");

    /// <summary>
    /// The running server's connection, null while none runs.
    /// </summary>
    public static JsonRpc? Rpc { get; private set; }

    private static StreamWriter? logWriter;

    /// <summary>
    /// A line of the extension's own in the server's log (LogPath).
    /// </summary>
    public static void Log(string line)
    {
        if (logWriter is StreamWriter log)
        {
            lock (log)
                log.WriteLine(line);
        }
    }

    public string Name => "kpp";

    public IEnumerable<string>? ConfigurationSections => null;

    /// <summary>
    /// The cell the solution is in (<see cref="Cell"/>): the server checks it from the start. And
    /// inactiveRegions: this client cannot add clangd's capability, so it asks for the notification
    /// (<see cref="InactiveCode"/>) here.
    /// </summary>
    public object? InitializationOptions
    {
        get
        {
            var options = this.cell is null ? new JObject() : (JObject)this.cell.DeepClone();
            options["inactiveRegions"] = true;
            return options;
        }
    }

    public IEnumerable<string>? FilesToWatch => null;

    public bool ShowNotificationOnInitializeFailed => true;

    public object? MiddleLayer => null;

    public object? CustomMessageTarget { get; } = new ServerNotifications();

    public event AsyncEventHandler<EventArgs>? StartAsync;

#pragma warning disable CS0067 // Part of ILanguageClient; the server is stopped by Visual Studio.
    public event AsyncEventHandler<EventArgs>? StopAsync;
#pragma warning restore CS0067

    private JObject? cell;
    private Cell? watcher;

    public async Task OnLoadedAsync()
    {
        await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
        this.cell = Cell.Current();
        this.watcher ??= Cell.Watch(this.cell, this.SelectAsync);
        if (this.StartAsync is not null)
            await this.StartAsync.InvokeAsync(this, EventArgs.Empty);
    }

    public async Task<Connection?> ActivateAsync(CancellationToken token)
    {
        string? server = FindServer();
        if (server is null)
        {
            // ShowNotificationOnInitializeFailed: Visual Studio shows this in its info bar.
            throw new FileNotFoundException(
                "kpp: the language server was not found, so .kpp files have no IntelliSense. Install kpp (its installer puts kpp.exe on the PATH), " +
                "or set the environment variable KPP_LSP_SERVER to kpp.exe or kppc.exe, then restart Visual Studio.");
        }

        Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
        var log = new StreamWriter(LogPath, append: false) { AutoFlush = true };
        logWriter = log;
        await log.WriteLineAsync($"kpp: {server} --lsp");
        var info = new ProcessStartInfo
        {
            FileName = server,
            Arguments = "--lsp",
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        var process = new Process { StartInfo = info, EnableRaisingEvents = true };
        process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is not null)
            {
                lock (log)
                    log.WriteLine(e.Data);
            }
        };
        process.Exited += (_, _) =>
        {
            lock (log)
                log.WriteLine($"kpp: the server exited with {process.ExitCode}");
        };
        if (!process.Start())
            throw new InvalidOperationException($"kpp: the language server {server} did not start.");
        process.BeginErrorReadLine();
        return new Connection(process.StandardOutput.BaseStream, process.StandardInput.BaseStream);
    }

    public Task OnServerInitializedAsync() => Task.CompletedTask;

    public Task<InitializationFailureContext?> OnServerInitializeFailedAsync(ILanguageClientInitializationInfo initializationState)
    {
        var context = new InitializationFailureContext
        {
            FailureMessage = $"kpp: the language server failed to start ({initializationState.StatusMessage}). Its log: {LogPath}",
        };
        return Task.FromResult<InitializationFailureContext?>(context);
    }

    /// <summary>
    /// Another cell (a configuration or startup project picked): kpp/selectCell, which the server
    /// answers with the cell or an error, both shown on the status bar.
    /// </summary>
    private async Task SelectAsync(JObject cell)
    {
        this.cell = cell;
        JsonRpc? rpc = Rpc;
        if (rpc is null)
            return;
        try
        {
            JToken? answer = await rpc.InvokeWithParameterObjectAsync<JToken?>("kpp/selectCell", cell);
            string target = (string?)answer?["target"] ?? "every target";
            await GoToDeclaration.StatusAsync($"kpp: IntelliSense checks {target}, {(string?)answer?["configuration"]}.");
        }
        catch (RemoteInvocationException e)
        {
            await GoToDeclaration.StatusAsync($"kpp: {e.Message}");
        }
    }

    public Task AttachForCustomMessageAsync(JsonRpc rpc)
    {
        Rpc = rpc;
        rpc.Disconnected += (_, _) =>
        {
            if (Rpc == rpc)
                Rpc = null;
        };
        return Task.CompletedTask;
    }

    /// <summary>
    /// KPP_LSP_SERVER when set, else kpp.exe on the PATH (the installer puts it there), else kppc.exe.
    /// </summary>
    private static string? FindServer()
    {
        string? explicitServer = Environment.GetEnvironmentVariable("KPP_LSP_SERVER");
        if (!string.IsNullOrEmpty(explicitServer))
            return File.Exists(explicitServer) ? explicitServer : null;
        foreach (string name in new[] { "kpp.exe", "kppc.exe" })
        {
            foreach (string directory in (Environment.GetEnvironmentVariable("PATH") ?? string.Empty).Split(Path.PathSeparator))
            {
                if (directory.Length == 0)
                    continue;
                string candidate = Path.Combine(directory.Trim('"'), name);
                if (File.Exists(candidate))
                    return candidate;
            }
        }

        return null;
    }
}
