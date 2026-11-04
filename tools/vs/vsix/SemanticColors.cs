namespace KppVisualStudio;

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Threading.Tasks;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Utilities;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;

/// <summary>
/// Names colored by what they name (docs/editors.md, "Colors"): the server's semantic tokens
/// (textDocument/semanticTokens/full), asked for again whenever it sends kpp/semanticTokensChanged
/// (after each check), drawn by <see cref="SemanticClassifier"/> in Visual Studio's C++ colors: a
/// kpp type is drawn as a C++ type, a function as a C++ function, and Tools > Options > Fonts and
/// Colors ("C++ Types", "C++ Functions", ...) sets both. Visual Studio's own client would take the
/// tokens too, but draws types and functions alike: the server offers them to the extension alone
/// (initializationOptions' semanticColors).
/// </summary>
internal static class SemanticTokens
{
    /// <summary>
    /// The server's legend (lsp.cpp, tokenTypes), by index, as the C++ classification each is drawn
    /// as; a modifier picks another (static, globalScope). A local is left as the grammar draws it,
    /// in the text's color (kovarex, 2026-11-04), and so are keywords, literals and comments.
    /// </summary>
    private static readonly string[] TokenTypes =
    {
        "namespace", "type", "class", "enum", "struct", "typeParameter", "parameter", "variable", "property", "enumMember", "function", "method",
    };

    private const int Static = 4;
    private const int GlobalScope = 8;

    /// <summary>
    /// Each file's tokens as last answered: (line, character, length, classification name).
    /// </summary>
    private static readonly ConcurrentDictionary<string, (int, int, int, string)[]> Tokens = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// The files open in an editor (their full paths): the ones a refresh asks for.
    /// </summary>
    private static readonly ConcurrentDictionary<string, bool> Open = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// A file's tokens changed (its full path).
    /// </summary>
    public static event Action<string>? Changed;

    public static (int, int, int, string)[] Of(string path) => Tokens.TryGetValue(InactiveCode.Key(path), out var t) ? t : Array.Empty<(int, int, int, string)>();

    public static void Opened(string path)
    {
        Open[InactiveCode.Key(path)] = true;
        _ = AskAsync(InactiveCode.Key(path));
    }

    public static void Closed(string path) => Open.TryRemove(InactiveCode.Key(path), out _);

    /// <summary>
    /// kpp/semanticTokensChanged: every open file's tokens again.
    /// </summary>
    public static void Refresh()
    {
        KppLanguageClient.Log($"kpp: semantic tokens refresh, {Open.Count} open file(s)");
        foreach (string path in Open.Keys)
            _ = AskAsync(path);
    }

    /// <summary>
    /// The C++ classification a token is drawn as.
    /// </summary>
    public static string? ClassificationOf(int type, int modifiers)
    {
        if (type < 0 || type >= TokenTypes.Length)
            return null;
        return TokenTypes[type] switch
        {
            "namespace" => "cppNamespace",
            "type" or "class" or "enum" or "struct" or "typeParameter" => "cppType",
            "parameter" => "cppParameter",
            "variable" => (modifiers & GlobalScope) != 0 ? "cppGlobalVariable" : null,
            "property" => (modifiers & Static) != 0 ? "cppStaticMemberField" : "cppMemberField",
            "enumMember" => "cppEnumerator",
            "function" => "cppFunction",
            "method" => (modifiers & Static) != 0 ? "cppStaticMemberFunction" : "cppMemberFunction",
            _ => null,
        };
    }

    private static async Task AskAsync(string path)
    {
        JsonRpc? rpc = KppLanguageClient.Rpc;
        if (rpc is null)
        {
            KppLanguageClient.Log($"kpp: semantic tokens in {path}: not asked, the server is not up");
            return;
        }

        JToken? answer;
        try
        {
            var request = new JObject { ["textDocument"] = new JObject { ["uri"] = new Uri(path).AbsoluteUri } };
            answer = await rpc.InvokeWithParameterObjectAsync<JToken?>("textDocument/semanticTokens/full", request);
        }
        catch (Exception e) when (e is RemoteInvocationException || e is ConnectionLostException || e is ObjectDisposedException)
        {
            KppLanguageClient.Log($"kpp: semantic tokens in {path}: {e.Message}");
            return;
        }

        // Each token is 5 numbers: its line after the previous token's, its character (after the
        // previous token's on the same line), its length, its type and its modifiers.
        var tokens = new List<(int, int, int, string)>();
        if (answer?["data"] is JArray data)
        {
            int line = 0, character = 0;
            for (int i = 0; i + 4 < data.Count; i += 5)
            {
                int deltaLine = (int)data[i], deltaStart = (int)data[i + 1];
                line += deltaLine;
                character = deltaLine == 0 ? character + deltaStart : deltaStart;
                if (ClassificationOf((int)data[i + 3], (int)data[i + 4]) is string name)
                    tokens.Add((line, character, (int)data[i + 2], name));
            }
        }

        Tokens[path] = tokens.ToArray();
        KppLanguageClient.Log($"kpp: semantic tokens in {path}: {tokens.Count}");
        Changed?.Invoke(path);
    }
}

[Export(typeof(IClassifierProvider))]
[ContentType(KppContentType.Name)]
internal sealed class SemanticClassifierProvider : IClassifierProvider
{
    [Import]
    internal IClassificationTypeRegistryService Registry { get; set; } = null!;

    [Import]
    internal ITextDocumentFactoryService Documents { get; set; } = null!;

    private bool watching;

    public IClassifier? GetClassifier(ITextBuffer buffer)
    {
        if (!this.Documents.TryGetTextDocument(buffer, out ITextDocument document))
            return null;
        if (!this.watching)
        {
            // A closed file is asked for no more.
            this.Documents.TextDocumentDisposed += (_, e) => SemanticTokens.Closed(e.TextDocument.FilePath);
            this.watching = true;
        }

        return buffer.Properties.GetOrCreateSingletonProperty(() => new SemanticClassifier(buffer, document, this.Registry));
    }
}

/// <summary>
/// A .kpp buffer's names as classification spans: the C++ classification of each token, where
/// Visual Studio has it (the C++ workload).
/// </summary>
internal sealed class SemanticClassifier : IClassifier
{
    private readonly ITextBuffer buffer;
    private readonly ITextDocument document;
    private readonly IClassificationTypeRegistryService registry;
    private readonly Dictionary<string, IClassificationType?> types = new();

    public SemanticClassifier(ITextBuffer buffer, ITextDocument document, IClassificationTypeRegistryService registry)
    {
        this.buffer = buffer;
        this.document = document;
        this.registry = registry;
        SemanticTokens.Changed += this.OnChanged;
        SemanticTokens.Opened(document.FilePath);
    }

    public event EventHandler<ClassificationChangedEventArgs>? ClassificationChanged;

    public IList<ClassificationSpan> GetClassificationSpans(SnapshotSpan span)
    {
        var spans = new List<ClassificationSpan>();
        ITextSnapshot snapshot = span.Snapshot;
        int first = snapshot.GetLineNumberFromPosition(span.Start);
        int last = snapshot.GetLineNumberFromPosition(span.End);
        foreach (var (line, character, length, name) in SemanticTokens.Of(this.document.FilePath))
        {
            // The server's positions are of the text it checked: a token past this snapshot's line is left out.
            if (line < first || line > last || line >= snapshot.LineCount)
                continue;
            ITextSnapshotLine text = snapshot.GetLineFromLineNumber(line);
            if (character + length > text.Length)
                continue;
            IClassificationType? type = this.TypeOf(name);
            if (type is null)
                continue;
            var token = new SnapshotSpan(snapshot, text.Start.Position + character, length);
            if (token.IntersectsWith(span))
                spans.Add(new ClassificationSpan(token, type));
        }

        return spans;
    }

    private IClassificationType? TypeOf(string name)
    {
        if (!this.types.TryGetValue(name, out IClassificationType? type))
            this.types[name] = type = this.registry.GetClassificationType(name);
        return type;
    }

    private void OnChanged(string path)
    {
        if (!string.Equals(path, InactiveCode.Key(this.document.FilePath), StringComparison.OrdinalIgnoreCase))
            return;
#pragma warning disable VSSDK007 // A notification: nothing waits for the redraw.
        ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
        {
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
            ITextSnapshot snapshot = this.buffer.CurrentSnapshot;
            this.ClassificationChanged?.Invoke(this, new ClassificationChangedEventArgs(new SnapshotSpan(snapshot, 0, snapshot.Length)));
        }).FileAndForget("kpp/SemanticColors");
#pragma warning restore VSSDK007
    }
}
