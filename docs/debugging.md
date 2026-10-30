# Debugging Kpp programs

`kppc --debug-info` puts line tables (CodeView) into the objects and links with `/DEBUG`, so a
`.pdb` lands next to the executable: a debugger maps every instruction to the Kpp line it came from,
breakpoints go on Kpp lines, and stepping walks the Kpp source.

```
compiler\build\kppc.exe --debug-info --exe out.exe program.kpp
```

The converted game: `set KPPC_EXTRA=--debug-info` before `programs\factorio\run.cmd` (run.cmd links
with `/DEBUG` already, for the crash report's function names).

## What is in it

- Each function and lambda is a `DISubprogram` at its declaration's line, named by its IR name
  (`square(int32)`, `Counter::add(int32)`).
- Every statement starts a line: its instructions are at the statement's first line. A `for`'s
  condition and step, a `while`'s and `do`'s condition and a range-`for`'s advance are at their own
  lines, so stepping through a loop comes back to its header. A `switch`'s cases are at their own
  statements' lines.
- The entry block's allocas and parameter stores are at the declaration's line (the debugger stops
  there first, then on the first statement).
- A trivial callee inlined into its caller (docs/compile-speed.md, "Design: inlining trivial
  functions in kppc") has no lines of its own: its instructions are at the call's line, and stepping
  into it steps over it. `--no-inline` keeps every call, for stepping into getters.
- Only lines (`emissionKind: LineTablesOnly`): no types and no variables yet, so a debugger shows the
  stack, the source and the registers, but not locals by name.
- Not covered: the startup functions of global initializers (no lines).

`tests\debuginfo.ps1` (run by run_tests.cmd) builds `tests\debuginfo\lines.kpp`, runs it and checks
the lines of each of its functions in the PDB (llvm-pdbutil) against `lines.expected`;
determinism.ps1 checks that the IR with `--debug-info` does not depend on the thread count.

## What it costs

The converted game (the d0495 snapshot's conversion), full builds as run.cmd builds it (64 modules,
`--stub-missing --incremental-link`, a fresh output directory each), with and without
`--debug-info`, interleaved, 2026-10-30 (scratch measure\build.cmd; other threads were using the
machine, so the seconds are noisy):

| | IR | codegen | clang | link | PDB |
|---|---:|---|---|---|---:|
| without | 739 MB | 7.9 8.5 10.0 s | 5.7 6.2 7.3 s | 2.4 2.5 2.4 s | 143 MB |
| `--debug-info` | 1,084 MB | 9.1 10.9 9.8 s | 8.7 11.2 9.3 s | 2.2 4.1 2.2 s | 268 MB |

The executable is the same size (147.6 MB), and with line tables its 45 menu maps end 600 ticks with
the C++ build's checksums, as without.

The first such build found invalid exception-handling IR that every build had: a handler's
cleanups unwound to the caller instead of to where the handler's catchswitch unwinds. LLVM's
verifier rejects that only when the module has debug info (51 of the 64 modules); spec
language/throw_from_handler_cleans_up_enclosing_scope builds with `--debug-info` to keep it so.

## The crash report

The game's crash report (programs\factorio\native\crash_report.c, linked by run.cmd) names each frame
from the PDB and, when the objects carry line tables, its Kpp file and line:

```
kpp crash report: exception 0xc0000005 at 00007FF7D21B135E (writing 0x0)
   0  t.exe!kpp.main+0x5e  C:\kpp-sandbox\scratch\codegen\dbg\t.kpp:17
   1  t.exe!main+0xf
```

## Visual Studio and WinDbg

Visual Studio: `devenv /DebugExe out.exe` (or File > Open > Project/Solution, the .exe), then F11
starts at `main`; a breakpoint in a `.kpp` opened in the same instance binds through the PDB's file
paths, which are absolute. WinDbg: `windbg out.exe`, `.lines` on, `bp program!kpp.main`, `p` and
`t` step by line. (Checked here: the PDB's line tables with llvm-pdbutil and dbghelp, through the
crash report. The interactive sessions in Visual Studio and WinDbg are still to be tried by hand;
neither cdb nor a working lldb is installed on PC-PRACOVNA.)

## How kppc emits it

The emitter writes a marker line `  ;@LINE` before each statement (`Emitter::debugLine`, not inside a
body inlined into its caller). When the function is done, `Emitter::debugLines` appends
`, !dbg !N` to every instruction after the marker, then the function's `DISubprogram` and one
`DILocation` per distinct line. Each function numbers its metadata from 0, ten digits wide, so that
its text does not depend on where it lands; module assembly (`renumberDebugIds`) rewrites the ids in
place to the module's, after the module's fixed `!0` to `!15` (`debugModuleTail`: the compile unit,
a placeholder file, the empty subroutine type and the CodeView flags). The file of each function is
an inline `!DIFile` with the source's absolute path.
