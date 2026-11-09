namespace KppVisualStudio;

using System;
using Microsoft.VisualStudio.Debugger;
using Microsoft.VisualStudio.Debugger.CallStack;
using Microsoft.VisualStudio.Debugger.ComponentInterfaces;
using Microsoft.VisualStudio.Debugger.Evaluation;
using Microsoft.VisualStudio.Debugger.Symbols;

/// <summary>
/// Gives code compiled from a .kpp file kpp's language (its id in kpp.pkgdef), so that
/// <see cref="KppExpressionEvaluator"/> is chosen for it; anything else stays as it is. A stack frame takes
/// its language from the basic symbol information it carries (the frame filter), a breakpoint's condition
/// from the symbol query. kpp's debug information itself stays C++'s (VS Code's debugger reads it, and
/// loads no extension).
/// </summary>
public sealed class KppFrameLanguage : IDkmSymbolCompilerIdQuery, IDkmCallStackFilter
{
    private static readonly DkmCompilerId KppCompiler = new(new Guid("b27d6e10-3c84-4a19-8f57-2e9c1a64d0f3"), new Guid("8e4f1c52-6a3b-4f7d-9c21-5b0a7d3e91c4"));

    private static bool IsKpp(string? document) => document != null && document.EndsWith(".kpp", StringComparison.OrdinalIgnoreCase);

    DkmStackWalkFrame[]? IDkmCallStackFilter.FilterNextFrame(DkmStackContext stackContext, DkmStackWalkFrame? input)
    {
        if (input == null)
            return null; // (the end of the stack)
        DkmBasicInstructionSymbolInfo? info = input.BasicSymbolInfo;
        if (info == null || !IsKpp(info.SourcePosition?.DocumentName))
            return new[] { input };
        return new[] { WithLanguage(input, KppCompiler) };
    }

    /// <summary>The frame with the given language in its symbol information.</summary>
    internal static DkmStackWalkFrame WithLanguage(DkmStackWalkFrame frame, DkmCompilerId language)
    {
        DkmBasicInstructionSymbolInfo? info = frame.BasicSymbolInfo;
        if (info == null)
            return frame;
        return DkmStackWalkFrame.Create(frame.Thread, frame.InstructionAddress, frame.FrameBase, frame.FrameSize, frame.Flags, frame.Description, frame.Registers, frame.Annotations,
                                        frame.AnnotatedModule, frame.AsyncContext, frame.Data, DkmBasicInstructionSymbolInfo.Create(language, info.MethodName, info.SourcePosition, info.InlineFrameCount),
                                        frame.Priority);
    }

    // (Asking for the position may ask for the compiler again: the inner call goes on down.)
    [ThreadStatic]
    private static bool asking;

    DkmCompilerId IDkmSymbolCompilerIdQuery.GetCompilerId(DkmInstructionSymbol instruction, DkmInspectionSession? inspectionSession)
    {
        // (A breakpoint binding asks with no inspection session. Asked during an evaluation, the answer stays C++'s.)
        if (!asking && inspectionSession == null)
        {
            asking = true;
            try
            {
                if (IsKpp(instruction.GetSourcePosition(DkmSourcePositionFlags.None, inspectionSession, out bool _)?.DocumentName))
                    return KppCompiler;
            }
            catch (DkmException)
            {
                // (no symbols here: Visual Studio's own answer)
            }
            finally
            {
                asking = false;
            }
        }
        return instruction.GetCompilerId(inspectionSession);
    }
}
