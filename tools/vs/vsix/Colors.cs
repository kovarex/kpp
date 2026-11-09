namespace KppVisualStudio;

using System.ComponentModel.Composition;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Utilities;

/// <summary>
/// kpp's colors (docs/editors.md, "Colors"; kovarex, 2026-11-09: VS Code's Light Modern and Dark Modern, "Just
/// set it to the selected style"), each listed in Tools > Options > Fonts and Colors. A format's default color
/// is not here: make-colors.ps1's table gives one per theme (kpp.colors.pkgdef). Plain text and punctuation
/// have no classification, so they stay in the user's Plain Text color.
///
/// What the server's tokens name (<see cref="SemanticClassifier"/>) comes after Priority.High; what the grammar
/// names (kpp.tmLanguage.tmTheme) before it, so that a token decides over the grammar's guess: `f(1)` is a call
/// until the server says f is a local.
/// </summary>
internal static class KppColors
{
    public const string Local = "kpp local";
    public const string Parameter = "kpp parameter";
    public const string Member = "kpp member";
    public const string StaticMember = "kpp static member";
    public const string Global = "kpp global";
    public const string Namespace = "kpp namespace";
    public const string Type = "kpp type";
    public const string Method = "kpp method";
    public const string Enumerator = "kpp enumerator";

    public const string Function = "kpp function";
    public const string ControlKeyword = "kpp control keyword";
    public const string Number = "kpp number";
    public const string NumberType = "kpp number type";
    public const string String = "kpp string";
    public const string StringEscape = "kpp string escape";
    public const string Comment = "kpp comment";
    public const string Operator = "kpp operator";

#pragma warning disable CS0649 // MEF reads them.
    [Export]
    [Name(Local)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? LocalType;

    [Export]
    [Name(Parameter)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? ParameterType;

    [Export]
    [Name(Member)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? MemberType;

    [Export]
    [Name(StaticMember)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? StaticMemberType;

    [Export]
    [Name(Global)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? GlobalType;

    [Export]
    [Name(Namespace)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? NamespaceType;

    [Export]
    [Name(Type)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? TypeType;

    [Export]
    [Name(Method)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? MethodType;

    [Export]
    [Name(Enumerator)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? EnumeratorType;

    [Export]
    [Name(Function)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? FunctionType;

    [Export]
    [Name(ControlKeyword)]
    [BaseDefinition("keyword")]
    internal static ClassificationTypeDefinition? ControlKeywordType;

    [Export]
    [Name(Number)]
    [BaseDefinition("number")]
    internal static ClassificationTypeDefinition? NumberClassificationType;

    [Export]
    [Name(NumberType)]
    [BaseDefinition("identifier")]
    internal static ClassificationTypeDefinition? NumberTypeType;

    [Export]
    [Name(String)]
    [BaseDefinition("string")]
    internal static ClassificationTypeDefinition? StringType;

    [Export]
    [Name(StringEscape)]
    [BaseDefinition("string")]
    internal static ClassificationTypeDefinition? StringEscapeType;

    [Export]
    [Name(Comment)]
    [BaseDefinition("comment")]
    internal static ClassificationTypeDefinition? CommentType;

    [Export]
    [Name(Operator)]
    [BaseDefinition("operator")]
    internal static ClassificationTypeDefinition? OperatorType;
#pragma warning restore CS0649
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Local)]
[Name(KppColors.Local)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppLocalFormat : ClassificationFormatDefinition
{
    public KppLocalFormat() => this.DisplayName = "kpp Local Variable";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Parameter)]
[Name(KppColors.Parameter)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppParameterFormat : ClassificationFormatDefinition
{
    public KppParameterFormat() => this.DisplayName = "kpp Parameter";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Member)]
[Name(KppColors.Member)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppMemberFormat : ClassificationFormatDefinition
{
    public KppMemberFormat() => this.DisplayName = "kpp Member";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.StaticMember)]
[Name(KppColors.StaticMember)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppStaticMemberFormat : ClassificationFormatDefinition
{
    public KppStaticMemberFormat() => this.DisplayName = "kpp Static Member";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Global)]
[Name(KppColors.Global)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppGlobalFormat : ClassificationFormatDefinition
{
    public KppGlobalFormat() => this.DisplayName = "kpp Global Variable";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Namespace)]
[Name(KppColors.Namespace)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppNamespaceFormat : ClassificationFormatDefinition
{
    public KppNamespaceFormat() => this.DisplayName = "kpp Namespace";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Type)]
[Name(KppColors.Type)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppTypeFormat : ClassificationFormatDefinition
{
    public KppTypeFormat() => this.DisplayName = "kpp Type";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Method)]
[Name(KppColors.Method)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppMethodFormat : ClassificationFormatDefinition
{
    public KppMethodFormat() => this.DisplayName = "kpp Member Function";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Enumerator)]
[Name(KppColors.Enumerator)]
[UserVisible(true)]
[Order(After = Priority.High)]
internal sealed class KppEnumeratorFormat : ClassificationFormatDefinition
{
    public KppEnumeratorFormat() => this.DisplayName = "kpp Enumerator";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Function)]
[Name(KppColors.Function)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppFunctionFormat : ClassificationFormatDefinition
{
    public KppFunctionFormat() => this.DisplayName = "kpp Function";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.ControlKeyword)]
[Name(KppColors.ControlKeyword)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppControlKeywordFormat : ClassificationFormatDefinition
{
    public KppControlKeywordFormat() => this.DisplayName = "kpp Control Keyword";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Number)]
[Name(KppColors.Number)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppNumberFormat : ClassificationFormatDefinition
{
    public KppNumberFormat() => this.DisplayName = "kpp Number";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.NumberType)]
[Name(KppColors.NumberType)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppNumberTypeFormat : ClassificationFormatDefinition
{
    public KppNumberTypeFormat() => this.DisplayName = "kpp Number Type";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.String)]
[Name(KppColors.String)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppStringFormat : ClassificationFormatDefinition
{
    public KppStringFormat() => this.DisplayName = "kpp String";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.StringEscape)]
[Name(KppColors.StringEscape)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppStringEscapeFormat : ClassificationFormatDefinition
{
    public KppStringEscapeFormat() => this.DisplayName = "kpp String Escape";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Comment)]
[Name(KppColors.Comment)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppCommentFormat : ClassificationFormatDefinition
{
    public KppCommentFormat() => this.DisplayName = "kpp Comment";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = KppColors.Operator)]
[Name(KppColors.Operator)]
[UserVisible(true)]
[Order(After = Priority.Default, Before = Priority.High)]
internal sealed class KppOperatorFormat : ClassificationFormatDefinition
{
    public KppOperatorFormat() => this.DisplayName = "kpp Operator";
}
