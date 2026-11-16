namespace KppVisualStudio;

using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Utilities;
using Newtonsoft.Json.Linq;

/// <summary>
/// kpp's test coverage (docs/design.md 28, docs/editors.md "Coverage"): a coverage file `kpp --coverage`
/// wrote, one JSON line per source file, shown in the .kpp editors while <see cref="Visible"/>: each
/// statement green when it ran and red when it did not, a condition amber when it was only ever true or
/// only ever false and red when never evaluated (`bar()` in `foo() && bar()`), a switch's missing default
/// amber when no value fell outside its cases; the hover says which (<see cref="CoverageQuickInfoSource"/>).
/// Tools > kpp: Show Coverage turns it on and off, Load Coverage File... reads another file
/// (<see cref="KppPackage"/>). A file whose text is not the text the run had (its hash, with kpp's
/// FNV-1a offset) shows nothing: its places would be another version's.
/// </summary>
internal static class Coverage
{
    public const string RanName = "kpp Covered";
    public const string NotRunName = "kpp Not Covered";
    public const string PartlyName = "kpp Partly Covered";

    internal sealed class Place
    {
        public int Line, Column, EndLine, EndColumn; // from 1; the end column is after the last character
        public string Kind = "statement";            // statement, condition, default
        public bool True, False, Ran;
        // Per test (the runner's --coverage-tests): how many tests ran it and the first of them, by
        // name; a condition's for its true and for its false. -1: the file does not say.
        public long Count = -1, TrueCount = -1, FalseCount = -1;
        public List<string> Tests = new(), TrueTests = new(), FalseTests = new();
    }

    // `count,[test,...]` at items[at] and items[at + 1], the tests by the file's names.
    private static void ReadTests(JToken place, int at, List<string> names, ref long count, List<string> tests)
    {
        if (place is not JArray a || a.Count < at + 2)
            return;
        count = (long)a[at]!;
        foreach (JToken t in a[at + 1] as JArray ?? new JArray())
        {
            int i = (int)t!;
            tests.Add(i >= 0 && i < names.Count ? names[i] : $"test {i}");
        }
    }

    internal sealed class FileCoverage
    {
        public string Hash = "";
        public List<Place> Places = new();
    }

    private static readonly object Lock = new();
    private static Dictionary<string, FileCoverage> files = new(StringComparer.OrdinalIgnoreCase);
    private static FileSystemWatcher? watcher;

    /// <summary>The coverage file shown (null: none read yet).</summary>
    public static string? Source { get; private set; }

    public static bool Visible { get; private set; }

    /// <summary>What is shown changed: every editor draws again.</summary>
    public static event Action? Changed;

    public static void SetVisible(bool visible)
    {
        Visible = visible;
        Changed?.Invoke();
    }

    /// <summary>
    /// Reads a coverage file and shows it; watches it, so that a later run's file shows when it is
    /// written. Returns a line for the status bar.
    /// </summary>
    public static string Load(string path)
    {
        path = Path.GetFullPath(path);
        var read = new Dictionary<string, FileCoverage>(StringComparer.OrdinalIgnoreCase);
        string folder = Path.GetDirectoryName(path)!;
        int count = 0;
        var names = new List<string>();
        foreach (string line in File.ReadAllLines(path))
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            JObject o = JObject.Parse(line);
            if (o["tests"] is JArray testNames)
            {
                foreach (JToken t in testNames)
                    names.Add((string?)t ?? "");
                continue;
            }
            string? name = (string?)o["file"];
            if (name is null)
                continue;
            string source = Path.IsPathRooted(name) ? name : Path.GetFullPath(Path.Combine(folder, name));
            var file = new FileCoverage { Hash = (string?)o["hash"] ?? "" };
            foreach (JToken s in o["statements"] as JArray ?? new JArray())
            {
                var p = new Place { Line = (int)s[0]!, Column = (int)s[1]!, EndLine = (int)s[2]!, EndColumn = (int)s[3]!, Ran = (int)s[4]! != 0 };
                ReadTests(s, 5, names, ref p.Count, p.Tests);
                file.Places.Add(p);
            }
            foreach (JToken c in o["conditions"] as JArray ?? new JArray())
            {
                var p = new Place
                {
                    Kind = "condition",
                    Line = (int)c[0]!,
                    Column = (int)c[1]!,
                    EndLine = (int)c[2]!,
                    EndColumn = (int)c[3]!,
                    True = (int)c[4]! != 0,
                    False = (int)c[5]! != 0,
                };
                ReadTests(c, 6, names, ref p.TrueCount, p.TrueTests);
                ReadTests(c, 8, names, ref p.FalseCount, p.FalseTests);
                file.Places.Add(p);
            }
            foreach (JToken d in o["defaults"] as JArray ?? new JArray())
            {
                var p = new Place
                {
                    Kind = "default",
                    Line = (int)d[0]!,
                    Column = (int)d[1]!,
                    EndLine = (int)d[0]!,
                    EndColumn = (int)d[1]! + "switch".Length,
                    Ran = (int)d[2]! != 0,
                };
                ReadTests(d, 3, names, ref p.Count, p.Tests);
                file.Places.Add(p);
            }
            read[Key(source)] = file;
            ++count;
        }
        lock (Lock)
        {
            files = read;
            if (!string.Equals(Source, path, StringComparison.OrdinalIgnoreCase))
            {
                watcher?.Dispose();
                watcher = new FileSystemWatcher(folder, Path.GetFileName(path)) { NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.Size };
                watcher.Changed += (_, _) => Reload();
                watcher.EnableRaisingEvents = true;
            }
            Source = path;
        }
        Visible = true;
        Changed?.Invoke();
        return $"kpp coverage: {count} file(s) from {path}";
    }

    private static void Reload()
    {
        // (the runner writes it whole at the end of a run: read it once it is written)
        Thread.Sleep(300);
        try
        {
            if (Source is not null)
                Load(Source);
        }
        catch (Exception e)
        {
            KppLanguageClient.Log($"kpp: coverage {Source} not read again: {e.Message}");
        }
    }

    private static readonly HashSet<string> watchedProjects = new(StringComparer.OrdinalIgnoreCase);
    private static readonly List<FileSystemWatcher> projectWatchers = new();

    /// <summary>
    /// A .kpp document opened: the folder of its build.kpp (the nearest above it) is watched, and a
    /// kpp-coverage.json written there (the coverage configuration's run: build.kpp's coverage, kpp
    /// --coverage-file) is shown at once, as Show Coverage would.
    /// </summary>
    public static void WatchProject(string documentPath)
    {
        string? folder = Path.GetDirectoryName(documentPath);
        while (!string.IsNullOrEmpty(folder) && !File.Exists(Path.Combine(folder, "build.kpp")))
            folder = Path.GetDirectoryName(folder);
        if (string.IsNullOrEmpty(folder))
            return;
        lock (Lock)
        {
            if (!watchedProjects.Add(folder!))
                return;
            var w = new FileSystemWatcher(folder!, "kpp-coverage.json") { NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.Size | NotifyFilters.FileName };
            FileSystemEventHandler written = (_, e) =>
            {
                Thread.Sleep(300); // (written whole at the run's end)
                try
                {
                    KppLanguageClient.Log(Load(e.FullPath));
                }
                catch (Exception error)
                {
                    KppLanguageClient.Log($"kpp: coverage {e.FullPath} not read: {error.Message}");
                }
            };
            w.Changed += written;
            w.Created += written;
            w.EnableRaisingEvents = true;
            projectWatchers.Add(w);
        }
    }

    /// <summary>The nearest kpp-coverage.json in a folder or one above it, null if none.</summary>
    public static string? Find(string? folder)
    {
        for (string? d = folder; !string.IsNullOrEmpty(d); d = Path.GetDirectoryName(d))
        {
            string candidate = Path.Combine(d, "kpp-coverage.json");
            if (File.Exists(candidate))
                return candidate;
        }
        return null;
    }

    public static string Key(string path) => Path.GetFullPath(path);

    /// <summary>A file's coverage while it is shown, null otherwise.</summary>
    public static FileCoverage? Of(string path)
    {
        if (!Visible)
            return null;
        lock (Lock)
            return files.TryGetValue(Key(path), out FileCoverage? f) ? f : null;
    }

    /// <summary>The hash kpp writes of a file's text: 64-bit FNV-1a, with kpp's offset 1469598103934665603.</summary>
    public static string Hash(byte[] bytes)
    {
        ulong h = 1469598103934665603UL;
        foreach (byte b in bytes)
            h = (h ^ b) * 1099511628211UL;
        return h.ToString("x16");
    }

    /// <summary>Whether a snapshot's text is the text the run had (UTF-8, as the file is).</summary>
    public static bool Matches(ITextSnapshot snapshot, FileCoverage file)
    {
        if (snapshot.TextBuffer.Properties.TryGetProperty("kppCoverageMatch", out Tuple<int, string, bool>? known) && known!.Item1 == snapshot.Version.VersionNumber &&
            known.Item2 == file.Hash)
            return known.Item3;
        bool matches = Hash(new UTF8Encoding(false).GetBytes(snapshot.GetText())) == file.Hash;
        snapshot.TextBuffer.Properties["kppCoverageMatch"] = Tuple.Create(snapshot.Version.VersionNumber, file.Hash, matches);
        return matches;
    }

    /// <summary>A place as a span of the snapshot (clamped to its lines), null if outside it.</summary>
    public static SnapshotSpan? SpanOf(ITextSnapshot snapshot, Place p)
    {
        if (p.Line < 1 || p.Line > snapshot.LineCount)
            return null;
        ITextSnapshotLine first = snapshot.GetLineFromLineNumber(p.Line - 1);
        ITextSnapshotLine last = snapshot.GetLineFromLineNumber(Math.Min(p.EndLine, snapshot.LineCount) - 1);
        int start = first.Start.Position + Math.Min(p.Column - 1, first.Length);
        int end = last.Start.Position + Math.Min(p.EndColumn - 1, last.Length);
        return end > start ? new SnapshotSpan(snapshot, start, end - start) : null;
    }

    /// <summary>"3 tests: A, B, C"; "1,234 tests, the first 10: A, ..., J"; "no test" (it ran outside the tests).</summary>
    public static string TestsText(long count, List<string> tests)
    {
        if (count <= 0)
            return "no test";
        string names = string.Join(", ", tests);
        string noun = count == 1 ? "test" : "tests";
        return tests.Count >= count ? $"{count:N0} {noun}: {names}" : $"{count:N0} {noun}, the first {tests.Count}: {names}";
    }

    /// <summary>What a place says: null when it is all covered.</summary>
    public static string? Missing(Place p) => p.Kind switch
    {
        "condition" when !p.True && !p.False => "never evaluated",
        "condition" when !p.True => "never true",
        "condition" when !p.False => "never false",
        "default" when !p.Ran => "no value outside the cases",
        "statement" when !p.Ran => "not run",
        _ => null,
    };

#pragma warning disable CS0649 // MEF reads these.
    [Export]
    [Name(RanName)]
    internal static ClassificationTypeDefinition? RanType;

    [Export]
    [Name(NotRunName)]
    internal static ClassificationTypeDefinition? NotRunType;

    [Export]
    [Name(PartlyName)]
    internal static ClassificationTypeDefinition? PartlyType;
#pragma warning restore CS0649
}

/// <summary>Code that ran (Tools > Options > Fonts and Colors, "kpp Covered").</summary>
[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = Coverage.RanName)]
[Name(Coverage.RanName)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class CoveredFormat : ClassificationFormatDefinition
{
    public CoveredFormat()
    {
        this.DisplayName = "kpp Covered";
        this.BackgroundColor = Color.FromRgb(0x2E, 0xA0, 0x43);
        this.BackgroundOpacity = 0.18;
    }
}

/// <summary>Code that did not run, a condition never evaluated.</summary>
[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = Coverage.NotRunName)]
[Name(Coverage.NotRunName)]
[UserVisible(true)]
[Order(After = Coverage.RanName)]
internal sealed class NotCoveredFormat : ClassificationFormatDefinition
{
    public NotCoveredFormat()
    {
        this.DisplayName = "kpp Not Covered";
        this.BackgroundColor = Color.FromRgb(0xE0, 0x3E, 0x3E);
        this.BackgroundOpacity = 0.28;
    }
}

/// <summary>A condition only ever true or only ever false, a missing default never taken.</summary>
[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = Coverage.PartlyName)]
[Name(Coverage.PartlyName)]
[UserVisible(true)]
[Order(After = Coverage.NotRunName)]
internal sealed class PartlyCoveredFormat : ClassificationFormatDefinition
{
    public PartlyCoveredFormat()
    {
        this.DisplayName = "kpp Partly Covered";
        this.BackgroundColor = Color.FromRgb(0xE8, 0xA3, 0x17);
        this.BackgroundOpacity = 0.32;
    }
}

[Export(typeof(IClassifierProvider))]
[ContentType(KppContentType.Name)]
internal sealed class CoverageClassifierProvider : IClassifierProvider
{
    [Import]
    internal IClassificationTypeRegistryService Registry { get; set; } = null!;

    [Import]
    internal ITextDocumentFactoryService Documents { get; set; } = null!;

    public IClassifier? GetClassifier(ITextBuffer buffer)
    {
        if (!this.Documents.TryGetTextDocument(buffer, out ITextDocument document))
            return null;
        Coverage.WatchProject(document.FilePath);
        return buffer.Properties.GetOrCreateSingletonProperty(() => new CoverageClassifier(buffer, document, this.Registry));
    }
}

/// <summary>A .kpp buffer's coverage as classification spans: statements first, conditions over them.</summary>
internal sealed class CoverageClassifier : IClassifier
{
    private readonly ITextBuffer buffer;
    private readonly ITextDocument document;
    private readonly IClassificationType ran, notRun, partly;

    public CoverageClassifier(ITextBuffer buffer, ITextDocument document, IClassificationTypeRegistryService registry)
    {
        this.buffer = buffer;
        this.document = document;
        this.ran = registry.GetClassificationType(Coverage.RanName);
        this.notRun = registry.GetClassificationType(Coverage.NotRunName);
        this.partly = registry.GetClassificationType(Coverage.PartlyName);
        Coverage.Changed += this.OnChanged;
    }

    public event EventHandler<ClassificationChangedEventArgs>? ClassificationChanged;

    public IList<ClassificationSpan> GetClassificationSpans(SnapshotSpan span)
    {
        var spans = new List<ClassificationSpan>();
        Coverage.FileCoverage? file = Coverage.Of(this.document.FilePath);
        if (file is null || !Coverage.Matches(span.Snapshot, file))
            return spans;
        foreach (string kind in new[] { "statement", "condition", "default" })
            foreach (Coverage.Place p in file.Places)
            {
                if (p.Kind != kind || Coverage.SpanOf(span.Snapshot, p) is not SnapshotSpan s || !s.IntersectsWith(span))
                    continue;
                IClassificationType? type = p.Kind == "statement" ? (p.Ran ? this.ran : this.notRun)
                                            : p.Kind == "default" ? (p.Ran ? null : this.partly)
                                            : !p.True && !p.False ? this.notRun
                                            : p.True && p.False ? null
                                            : this.partly;
                if (type is not null)
                    spans.Add(new ClassificationSpan(s, type));
            }
        return spans;
    }

    private void OnChanged()
    {
#pragma warning disable VSSDK007 // A notification: nothing waits for the redraw.
        ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
        {
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
            ITextSnapshot snapshot = this.buffer.CurrentSnapshot;
            this.ClassificationChanged?.Invoke(this, new ClassificationChangedEventArgs(new SnapshotSpan(snapshot, 0, snapshot.Length)));
        }).FileAndForget("kpp/Coverage");
#pragma warning restore VSSDK007
    }
}

[Export(typeof(IAsyncQuickInfoSourceProvider))]
[Name("kpp coverage quick info")]
[ContentType(KppContentType.Name)]
[Order]
internal sealed class CoverageQuickInfoSourceProvider : IAsyncQuickInfoSourceProvider
{
    [Import]
    internal ITextDocumentFactoryService Documents { get; set; } = null!;

    public IAsyncQuickInfoSource? TryCreateQuickInfoSource(ITextBuffer buffer) =>
        this.Documents.TryGetTextDocument(buffer, out ITextDocument document) ? new CoverageQuickInfoSource(document) : null;
}

/// <summary>The hover over code the run missed: "kpp coverage: never false", "not run", ...</summary>
internal sealed class CoverageQuickInfoSource : IAsyncQuickInfoSource
{
    private readonly ITextDocument document;

    public CoverageQuickInfoSource(ITextDocument document) => this.document = document;

    public Task<QuickInfoItem?> GetQuickInfoItemAsync(IAsyncQuickInfoSession session, CancellationToken cancellationToken)
    {
        ITextSnapshot snapshot = session.TextView.TextBuffer.CurrentSnapshot;
        SnapshotPoint? point = session.GetTriggerPoint(snapshot);
        Coverage.FileCoverage? file = Coverage.Of(this.document.FilePath);
        if (point is null || file is null || !Coverage.Matches(snapshot, file))
            return Task.FromResult<QuickInfoItem?>(null);
        // The innermost place there: a condition inside its statement. Without per-test counts only
        // what the run missed says something.
        Coverage.Place? found = null;
        SnapshotSpan foundSpan = default;
        foreach (Coverage.Place p in file.Places)
            if ((Coverage.Missing(p) is not null || p.Count >= 0 || p.TrueCount >= 0) && Coverage.SpanOf(snapshot, p) is SnapshotSpan s &&
                s.Contains(point.Value) && (found is null || foundSpan.Contains(s)))
            {
                found = p;
                foundSpan = s;
            }
        if (found is null)
            return Task.FromResult<QuickInfoItem?>(null);
        string text;
        if (found.Kind == "condition")
        {
            text = Coverage.Missing(found) is string missing ? $"kpp coverage: this condition was {missing}" : "kpp coverage: this condition was true and false";
            if (found.TrueCount >= 0)
                text += $"\ntrue in {Coverage.TestsText(found.TrueCount, found.TrueTests)}\nfalse in {Coverage.TestsText(found.FalseCount, found.FalseTests)}";
        }
        else if (found.Kind == "default")
            text = found.Ran ? $"kpp coverage: a value outside the cases in {Coverage.TestsText(found.Count, found.Tests)}" : "kpp coverage: no value fell outside this switch's cases";
        else
            text = found.Ran ? (found.Count >= 0 ? $"kpp coverage: run by {Coverage.TestsText(found.Count, found.Tests)}" : "kpp coverage: run") : "kpp coverage: not run";
        return Task.FromResult<QuickInfoItem?>(new QuickInfoItem(snapshot.CreateTrackingSpan(foundSpan, SpanTrackingMode.EdgeInclusive), text));
    }

    public void Dispose()
    {
    }
}
