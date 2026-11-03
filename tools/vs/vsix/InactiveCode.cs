namespace KppVisualStudio;

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.IO;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Utilities;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;

/// <summary>
/// The code the checked cell does not build (docs/editors.md, "Inactive code"): the server's
/// textDocument/inactiveRegions (clangd's notification), by file, drawn faded by
/// <see cref="InactiveClassifier"/>.
/// </summary>
internal static class InactiveCode
{
    public const string ClassificationName = "kpp inactive code";

    /// <summary>
    /// Each file's regions as the server last sent them: (start line, character, end line, character).
    /// </summary>
    private static readonly ConcurrentDictionary<string, (int, int, int, int)[]> Regions = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// A file's regions changed (its full path).
    /// </summary>
    public static event Action<string>? Changed;

    public static (int, int, int, int)[] Of(string path) => Regions.TryGetValue(Key(path), out var r) ? r : Array.Empty<(int, int, int, int)>();

    public static void Set(string path, (int, int, int, int)[] regions)
    {
        Regions[Key(path)] = regions;
        Changed?.Invoke(Key(path));
    }

    public static string Key(string path) => Path.GetFullPath(path);

#pragma warning disable CS0649 // MEF reads these.
    [Export]
    [Name(ClassificationName)]
    internal static ClassificationTypeDefinition? Type;
#pragma warning restore CS0649
}

/// <summary>
/// The notifications of the server the client does not take itself (KppLanguageClient's
/// CustomMessageTarget).
/// </summary>
internal sealed class ServerNotifications
{
    [JsonRpcMethod("textDocument/inactiveRegions", UseSingleObjectParameterDeserialization = true)]
    public void InactiveRegions(JToken parameters)
    {
        string? uri = (string?)parameters["textDocument"]?["uri"];
        if (uri is null)
            return;
        var regions = new List<(int, int, int, int)>();
        foreach (JToken r in parameters["regions"] as JArray ?? new JArray())
            regions.Add(((int)r["start"]!["line"]!, (int)r["start"]!["character"]!, (int)r["end"]!["line"]!, (int)r["end"]!["character"]!));
        string path = GoToDeclaration.PathOf(uri);
        InactiveCode.Set(path, regions.ToArray());
        KppLanguageClient.Log($"kpp: inactive regions in {path}: {string.Join(" ", regions.ConvertAll(r => $"{r.Item1 + 1}-{r.Item3 + 1}"))}");
    }
}

/// <summary>
/// How inactive code is drawn: faded (Tools > Options > Fonts and Colors, "kpp Inactive Code").
/// </summary>
[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = InactiveCode.ClassificationName)]
[Name(InactiveCode.ClassificationName)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class InactiveFormat : ClassificationFormatDefinition
{
    public InactiveFormat()
    {
        this.DisplayName = "kpp Inactive Code";
        this.ForegroundOpacity = 0.55;
    }
}

[Export(typeof(IClassifierProvider))]
[ContentType(KppContentType.Name)]
internal sealed class InactiveClassifierProvider : IClassifierProvider
{
    [Import]
    internal IClassificationTypeRegistryService Registry { get; set; } = null!;

    [Import]
    internal ITextDocumentFactoryService Documents { get; set; } = null!;

    public IClassifier? GetClassifier(ITextBuffer buffer)
    {
        if (!this.Documents.TryGetTextDocument(buffer, out ITextDocument document))
            return null;
        IClassificationType type = this.Registry.GetClassificationType(InactiveCode.ClassificationName);
        return buffer.Properties.GetOrCreateSingletonProperty(() => new InactiveClassifier(buffer, document, type));
    }
}

/// <summary>
/// A .kpp buffer's inactive regions as classification spans.
/// </summary>
internal sealed class InactiveClassifier : IClassifier
{
    private readonly ITextBuffer buffer;
    private readonly ITextDocument document;
    private readonly IClassificationType type;

    public InactiveClassifier(ITextBuffer buffer, ITextDocument document, IClassificationType type)
    {
        this.buffer = buffer;
        this.document = document;
        this.type = type;
        InactiveCode.Changed += this.OnChanged;
    }

    public event EventHandler<ClassificationChangedEventArgs>? ClassificationChanged;

    public IList<ClassificationSpan> GetClassificationSpans(SnapshotSpan span)
    {
        var spans = new List<ClassificationSpan>();
        ITextSnapshot snapshot = span.Snapshot;
        foreach (var (fromLine, fromCharacter, toLine, toCharacter) in InactiveCode.Of(this.document.FilePath))
        {
            // The server's positions are of the text it checked: clamped to this snapshot's.
            if (fromLine >= snapshot.LineCount)
                continue;
            ITextSnapshotLine first = snapshot.GetLineFromLineNumber(fromLine);
            ITextSnapshotLine last = snapshot.GetLineFromLineNumber(Math.Min(toLine, snapshot.LineCount - 1));
            int start = first.Start.Position + Math.Min(fromCharacter, first.Length);
            int end = last.Start.Position + Math.Min(toCharacter, last.Length);
            if (end <= start)
                continue;
            var region = new SnapshotSpan(snapshot, start, end - start);
            if (region.IntersectsWith(span))
                spans.Add(new ClassificationSpan(region, this.type));
        }

        return spans;
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
        }).FileAndForget("kpp/InactiveCode");
#pragma warning restore VSSDK007
    }
}
