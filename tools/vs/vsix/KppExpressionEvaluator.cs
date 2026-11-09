namespace KppVisualStudio;

using System;
using System.Text;
using Microsoft.VisualStudio.Debugger;
using Microsoft.VisualStudio.Debugger.Breakpoints;
using Microsoft.VisualStudio.Debugger.CallStack;
using Microsoft.VisualStudio.Debugger.ComponentInterfaces;
using Microsoft.VisualStudio.Debugger.Evaluation;
using Microsoft.VisualStudio.Debugger.Symbols;

/// <summary>
/// The expression evaluator of kpp's frames (docs/debugging.md, "this in the watch window"): the watch
/// window, DataTips, the Immediate window and breakpoint conditions. kpp's debug information is C++'s, so
/// Visual Studio would give its frames the C++ evaluator, where `this` is the object pointer and `this.x` an
/// error or a wrong value. <see cref="KppFrameLanguage"/> gives those frames kpp's language, this component
/// is chosen for it, and it writes `this.x` as `this->x` and `&this` as `this` and evaluates through the C++
/// evaluator, in a C++ inspection context. The results carry that context, so Locals, expanding a value
/// and natvis are the C++ evaluator's. A component filtered on C++ itself cannot do this: what it passes on
/// comes back to it (tried, 2026-11-09).
/// </summary>
public sealed class KppExpressionEvaluator : IDkmLanguageExpressionEvaluator, IDkmLanguageFrameDecoder, IDkmLanguageInstructionDecoder, IDkmLanguageExpressionCompiler
{
    private static DkmLanguage Cpp() => DkmLanguage.Create("C++", new DkmCompilerId(DkmVendorId.Microsoft, DkmLanguageId.Cpp));

    private static DkmLanguage CppOf(DkmInspectionContext c) => c.RuntimeInstance.Process.EngineSettings.GetLanguage(new DkmCompilerId(DkmVendorId.Microsoft, DkmLanguageId.Cpp));

    // The C++ context of a kpp frame. Visual Studio applies the natvis in a PDB in the context it makes for a
    // C++ frame alone, so the program's rules are given here: the .natvis files kpp writes or lists beside the
    // executable, compiled once per inspection session (a stop). A module without one shows its values raw.
    private static DkmInspectionContext AsCpp(DkmInspectionContext c, DkmStackWalkFrame? frame = null)
    {
        DkmLanguage cpp = CppOf(c);
        DkmCompiledVisualizationData? natvis = c.AdditionalVisualizationData;
        DkmCompiledVisualizationDataPriority priority = c.AdditionalVisualizationDataPriority;
        if (natvis == null && frame != null && NatvisOf(c, frame, cpp) is { } found)
        {
            natvis = found;
            priority = DkmCompiledVisualizationDataPriority.High;
        }
        return DkmInspectionContext.Create(c.InspectionSession, c.RuntimeInstance, c.Thread, c.Timeout, c.EvaluationFlags, c.FuncEvalFlags, c.Radix, cpp, c.ReturnValue, natvis, priority, c.ReturnValues);
    }

    private sealed class SessionNatvis : DkmDataItem
    {
        public readonly System.Collections.Generic.Dictionary<string, DkmCompiledVisualizationData?> Compiled = new();
    }

    private static DkmCompiledVisualizationData? NatvisOf(DkmInspectionContext c, DkmStackWalkFrame frame, DkmLanguage cpp)
    {
        string? module = frame.ModuleInstance?.FullName;
        if (module == null)
            return null;
        DkmInspectionSession session = c.InspectionSession;
        SessionNatvis? known = session.GetDataItem<SessionNatvis>();
        if (known == null)
        {
            known = new SessionNatvis();
            session.SetDataItem(DkmDataCreationDisposition.CreateAlways, known);
        }
        lock (known)
        {
            if (known.Compiled.TryGetValue(module, out DkmCompiledVisualizationData? compiled))
                return compiled;
            string path = System.IO.Path.ChangeExtension(module, ".natvis");
            if (System.IO.File.Exists(path))
            {
                var files = new System.Collections.Generic.List<string> { path };
                if (System.IO.File.Exists(path + "-files")) // (the program's own, build.kpp's natvis)
                    files.AddRange(System.Linq.Enumerable.Where(System.IO.File.ReadAllLines(path + "-files"), System.IO.File.Exists));
                compiled = DkmCompiledVisualizationData.Create(cpp, session, null);
                compiled.Initialize(files.ToArray());
            }
            known.Compiled[module] = compiled;
            return compiled;
        }
    }

    // (the C++ evaluator is given the frame as Visual Studio made it, C++'s)
    private static DkmStackWalkFrame AsCpp(DkmStackWalkFrame frame) => KppFrameLanguage.WithLanguage(frame, Cpp().Id);

    void IDkmLanguageExpressionEvaluator.EvaluateExpression(DkmInspectionContext inspectionContext, DkmWorkList workList, DkmLanguageExpression expression,
                                                            DkmStackWalkFrame stackFrame, DkmCompletionRoutine<DkmEvaluateExpressionAsyncResult> completionRoutine)
    {
        string rewritten = Rewrite(expression.Text);
        DkmLanguage cpp = Cpp();
        DkmLanguageExpression written = DkmLanguageExpression.Create(cpp, expression.CompilationFlags, rewritten, null);
        AsCpp(inspectionContext, stackFrame).EvaluateExpression(workList, written, AsCpp(stackFrame), completionRoutine);
    }

    void IDkmLanguageExpressionEvaluator.GetFrameLocals(DkmInspectionContext inspectionContext, DkmWorkList workList, DkmStackWalkFrame stackFrame,
                                                        DkmCompletionRoutine<DkmGetFrameLocalsAsyncResult> completionRoutine) =>
        AsCpp(inspectionContext, stackFrame).GetFrameLocals(workList, AsCpp(stackFrame), completionRoutine);

    void IDkmLanguageExpressionEvaluator.GetFrameArguments(DkmInspectionContext inspectionContext, DkmWorkList workList, DkmStackWalkFrame frame,
                                                           DkmCompletionRoutine<DkmGetFrameArgumentsAsyncResult> completionRoutine) =>
        AsCpp(inspectionContext, frame).GetFrameArguments(workList, AsCpp(frame), completionRoutine);

    // (results and enumerations carry the C++ context, so Concord sends these to the C++ evaluator itself)
    void IDkmLanguageExpressionEvaluator.GetChildren(DkmEvaluationResult result, DkmWorkList workList, int initialRequestSize, DkmInspectionContext inspectionContext,
                                                     DkmCompletionRoutine<DkmGetChildrenAsyncResult> completionRoutine) =>
        result.GetChildren(workList, initialRequestSize, AsCpp(inspectionContext), completionRoutine);

    void IDkmLanguageExpressionEvaluator.GetItems(DkmEvaluationResultEnumContext enumContext, DkmWorkList workList, int startIndex, int count,
                                                  DkmCompletionRoutine<DkmEvaluationEnumAsyncResult> completionRoutine) =>
        enumContext.GetItems(workList, startIndex, count, completionRoutine);

    void IDkmLanguageExpressionEvaluator.SetValueAsString(DkmEvaluationResult result, string value, int timeout, out string errorText) =>
        result.SetValueAsString(value, timeout, out errorText!);

    string IDkmLanguageExpressionEvaluator.GetUnderlyingString(DkmEvaluationResult result) => result.GetUnderlyingString()!;

    void IDkmLanguageFrameDecoder.GetFrameName(DkmInspectionContext inspectionContext, DkmWorkList workList, DkmStackWalkFrame frame, DkmVariableInfoFlags argumentFlags,
                                               DkmCompletionRoutine<DkmGetFrameNameAsyncResult> completionRoutine) =>
        AsCpp(inspectionContext).GetFrameName(workList, AsCpp(frame), argumentFlags, completionRoutine);

    void IDkmLanguageFrameDecoder.GetFrameReturnType(DkmInspectionContext inspectionContext, DkmWorkList workList, DkmStackWalkFrame frame,
                                                     DkmCompletionRoutine<DkmGetFrameReturnTypeAsyncResult> completionRoutine) =>
        AsCpp(inspectionContext).GetFrameReturnType(workList, AsCpp(frame), completionRoutine);

    string IDkmLanguageInstructionDecoder.GetMethodName(DkmLanguageInstructionAddress languageInstructionAddress, DkmVariableInfoFlags argumentFlags) =>
        DkmLanguageInstructionAddress.Create(Cpp(), languageInstructionAddress.Address).GetMethodName(argumentFlags);

    // A breakpoint's condition: compiled by the C++ evaluator, written as for the watch window.
    DkmCompiledInspectionQuery? IDkmLanguageExpressionCompiler.CompileCondition(DkmLanguageInstructionAddress languageInstructionAddress, DkmBreakpointCondition condition, out string? errorText)
    {
        string rewritten = Rewrite(condition.Text);
        DkmLanguage cpp = Cpp();
        var written = DkmBreakpointCondition.Create(condition.Operator, cpp.Id, condition.CompilationFlags, rewritten, condition.Timeout);
        return DkmLanguageInstructionAddress.Create(cpp, languageInstructionAddress.Address).CompileCondition(written, out errorText);
    }

    void IDkmLanguageExpressionCompiler.Compile(DkmLanguageInstructionAddress languageInstructionAddress, DkmLanguageExpression expression, out DkmFailedEvaluationResult failed) =>
        throw new NotSupportedException(); // (obsolete in Concord, never called)

    /// <summary>
    /// `this.` as `this->` and `&this` as `this`, `this` a whole word outside string and character
    /// literals; not after `.`, `->` or `::` (a member named this is no C++).
    /// </summary>
    public static string Rewrite(string text)
    {
        var output = new StringBuilder(text.Length + 8);
        int i = 0;
        while (i < text.Length)
        {
            char c = text[i];
            if (c == '"' || c == '\'')
            {
                int end = i + 1;
                while (end < text.Length && text[end] != c)
                    end += text[end] == '\\' ? 2 : 1;
                end = System.Math.Min(end + 1, text.Length);
                output.Append(text, i, end - i);
                i = end;
                continue;
            }
            if (IsWordAt(text, i, "this"))
            {
                int after = i + 4;
                int dot = after;
                while (dot < text.Length && char.IsWhiteSpace(text[dot]))
                    ++dot;
                // `&this`: the reference's address is the pointer itself
                int amp = output.Length - 1;
                while (amp >= 0 && char.IsWhiteSpace(output[amp]))
                    --amp;
                if (amp >= 0 && output[amp] == '&' && (amp == 0 || output[amp - 1] != '&'))
                {
                    int operand = amp - 1;
                    while (operand >= 0 && char.IsWhiteSpace(output[operand]))
                        --operand;
                    if (operand < 0 || !IsOperandEnd(output[operand]))
                        output.Remove(amp, output.Length - amp);
                }
                output.Append("this");
                if (dot < text.Length && text[dot] == '.' && (dot + 1 >= text.Length || text[dot + 1] != '.') &&
                    (dot + 1 >= text.Length || !char.IsDigit(text[dot + 1])))
                {
                    output.Append("->");
                    i = dot + 1;
                    continue;
                }
                i = after;
                continue;
            }
            output.Append(c);
            ++i;
        }
        return output.ToString();
    }

    // `this` at i as a word of its own, not a member's name after `.`, `->` or `::`.
    private static bool IsWordAt(string text, int i, string word)
    {
        if (string.CompareOrdinal(text, i, word, 0, word.Length) != 0)
            return false;
        if (i + word.Length < text.Length && IsIdentifier(text[i + word.Length]))
            return false;
        if (i > 0 && IsIdentifier(text[i - 1]))
            return false;
        int before = i - 1;
        while (before >= 0 && char.IsWhiteSpace(text[before]))
            --before;
        if (before >= 0 && text[before] == '.')
            return false;
        if (before >= 1 && ((text[before] == '>' && text[before - 1] == '-') || (text[before] == ':' && text[before - 1] == ':')))
            return false;
        return true;
    }

    private static bool IsIdentifier(char c) => char.IsLetterOrDigit(c) || c == '_';

    // Whether the character before a `&` ends an operand (then the `&` is a binary and, not an address).
    private static bool IsOperandEnd(char c) => IsIdentifier(c) || c == ')' || c == ']';
}
