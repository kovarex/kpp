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
/// (after each check), drawn by <see cref="SemanticClassifier"/> as kpp's own classifications
/// (<see cref="KppColors"/>), set in Tools > Options > Fonts and Colors ("kpp Type", "kpp Function",
/// ...). Visual Studio's own client would take the tokens too, but draws types and functions alike:
/// the server offers them to the extension alone (initializationOptions' semanticColors).
/// </summary>
internal static class SemanticTokens
{
    /// <summary>
    /// The server's legend (kpp/semanticTokensLegend, lsp.cpp's tokenTypes and tokenModifiers): the
    /// token types by index and each modifier's bit, by name. Asked for once per server.
    /// </summary>
    private static (string[] Types, Dictionary<string, int> Modifiers)? legend;
    private static JsonRpc? legendOf;

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
    /// The classification a token is drawn as, by its type's and modifiers' names. Keywords, literals
    /// and comments are left to the grammar. kpp's own words (check, require, fallthrough, implicit,
    /// This...), where the parser takes them as such, are "kpp Keyword" (<see cref="KppKeywordFormat"/>).
    /// </summary>
    public static string? ClassificationOf(string type, bool isStatic, bool globalScope) => type switch
    {
        "kppKeyword" => KppKeywordFormat.ClassificationName,
        "namespace" => KppColors.Namespace,
        "type" or "class" or "enum" or "struct" or "typeParameter" => KppColors.Type,
        "parameter" => KppColors.Parameter,
        "variable" => globalScope ? KppColors.Global : KppColors.Local,
        "property" => isStatic ? KppColors.StaticMember : KppColors.Member,
        "enumMember" => KppColors.Enumerator,
        "function" => KppColors.Function,
        "method" => KppColors.Method,
        _ => null,
    };

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
            if (legend is null || legendOf != rpc)
            {
                JToken? l = await rpc.InvokeWithParameterObjectAsync<JToken?>("kpp/semanticTokensLegend", new JObject());
                var types = new List<string>();
                foreach (JToken t in l?["tokenTypes"] as JArray ?? new JArray())
                    types.Add((string?)t ?? string.Empty);
                var modifiers = new Dictionary<string, int>();
                int bit = 0;
                foreach (JToken m in l?["tokenModifiers"] as JArray ?? new JArray())
                    modifiers[(string?)m ?? string.Empty] = 1 << bit++;
                legend = (types.ToArray(), modifiers);
                legendOf = rpc;
            }

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
        var (typeNames, modifierBits) = legend!.Value;
        int staticBit = modifierBits.TryGetValue("static", out int s) ? s : 0;
        int globalBit = modifierBits.TryGetValue("globalScope", out int g) ? g : 0;
        if (answer?["data"] is JArray data)
        {
            int line = 0, character = 0;
            for (int i = 0; i + 4 < data.Count; i += 5)
            {
                int deltaLine = (int)data[i], deltaStart = (int)data[i + 1];
                line += deltaLine;
                character = deltaLine == 0 ? character + deltaStart : deltaStart;
                int type = (int)data[i + 3], modifiers = (int)data[i + 4];
                if (type >= 0 && type < typeNames.Length &&
                    ClassificationOf(typeNames[type], (modifiers & staticBit) != 0, (modifiers & globalBit) != 0) is string name)
                    tokens.Add((line, character, (int)data[i + 2], name));
            }
        }

        Tokens[path] = tokens.ToArray();
        KppLanguageClient.Log($"kpp: semantic tokens in {path}: {tokens.Count}");
        Changed?.Invoke(path);
    }
}

/// <summary>
/// How kpp's own words are drawn (the server's kppKeyword, docs/ide.md): a keyword's color, bold, so
/// that `check(...)` in a test reads as the statement it is, not a call (Tools > Options > Fonts and
/// Colors, "kpp Keyword").
/// </summary>
[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppKeywordFormat.ClassificationName)]
[Name(KppKeywordFormat.ClassificationName)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppKeywordFormat : ClassificationFormatDefinition
{
    public const string ClassificationName = "kpp keyword";

#pragma warning disable CS0649 // MEF reads it.
    [Export]
    [Name(ClassificationName)]
    [BaseDefinition("keyword")] // (PredefinedClassificationTypeNames.Keyword)
    internal static ClassificationTypeDefinition? Type;
#pragma warning restore CS0649

    public KppKeywordFormat()
    {
        this.DisplayName = "kpp Keyword";
        this.IsBold = true;
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
/// A .kpp buffer's names as classification spans: the classification of each token.
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
