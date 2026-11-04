# Debugging kpp programs

`kppc --debug-info` puts line tables (CodeView) into the objects and links with `/DEBUG`, so a
`.pdb` lands next to the executable: a debugger maps every instruction to the kpp line it came from,
breakpoints go on kpp lines, and stepping walks the kpp source.

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
- Variables and types, in an unoptimized build (2026-11-04, "Variables and types" below): every
  parameter and local, `this` too, by its kpp name and visible in its block, with its type; every
  class with its members, bases and vtable pointer as kppc lays it out, named as kppc's IR names it
  (`game::Entity`, `Vector<int32>`). An optimized build with `--debug-info` (build.kpp's fastdebug)
  keeps the line tables alone (`emissionKind: LineTablesOnly`).
- Not covered: the startup functions of global initializers (no lines); globals, static members and
  a lambda's captures (no variables yet).

`tests\debuginfo.ps1` (run by run_tests.cmd) builds `tests\debuginfo\lines.kpp`, runs it and checks
the lines of each of its functions in the PDB (llvm-pdbutil) against `lines.expected`, then
`tests\debuginfo\values.kpp` as one module and as 64, and its variables and types in the PDB against
`values.expected`; determinism.ps1 checks that the IR with `--debug-info` does not depend on the
thread count.

## Variables and types

In a debug build (unoptimized) `--debug-info` describes the program's data too (2026-11-04):
- **Variables**: each parameter (`this` artificial, the object pointer) and local by its kpp name,
  at its declaration's line, in the lexical block of the block statement it is declared in (a
  function's own body is the function's scope, as clang has it): the debugger shows a block's
  variables where its code is. A variable's address is its alloca, or the pointer a parameter
  passed by address is; a reference's type is the reference, its slot holding the pointer.
- **Basic types** as CodeView has them, without names of their own (`int`, `__int64`, `unsigned
  char`, `double`), each of kpp's integers a typedef of its name too: the PDB's S_UDT `int32`,
  `uint8`, which a natvis rule's `$T1` parses.
- **Pointers, references, const, arrays** (their length), function pointers.
- **Enums** with their enumerators and underlying type.
- **Classes** with their data members at their offsets in kppc's own layout (a bit field at its
  bits), their bases at theirs, a polymorphic class's vtable pointer (CodeView's LF_VFUNCTAB), an
  anonymous struct's or union's members as the enclosing class's (as MSVC's PDBs list them:
  String's `heapChars`). A class or enum is named as kppc's IR names it, the whole spelling
  (TypeTable::toString, qualified: `game::Entity`, `Vector<Vector<int32>>`, every template argument
  with the defaults), which identifies it too: the natvis rules match these names (agreed with
  thread F, "Values in Visual Studio" below).

Each class is defined once in the program, in its home module (the hash of its name over the
modules, as a first use's home), and declared in every other module that names it (DIFlagFwdDecl,
the same identifier): lld-link merges the modules' types into the PDB and Visual Studio resolves a
declaration by the identifier. A home lists its classes in its compile unit's retainedTypes (LLVM's
CodeView writer emits a type only when something in its module names it). A class the program only
points to, never laid out, has its declaration alone.

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

Variables and types (2026-11-04): factorio-in-kpp c9cda971 in its debug configuration (`kpp factorio
debug`: debug info, 256 modules, the compile server, link.exe incremental), built by kpp master with
the line tables alone and with variables and types (B's scratch bench\measure-debuginfo.ps1: per
round, each kpp's server stopped and build\debug deleted, the clean build, then the edit of one
function in AnimationBank.kpp built and put back; 2 rounds interleaved; other threads' load 11-17%,
so the seconds are noisy):

| | IR | codegen | clang | link | clean build | PDB | the edit | undone |
|---|---:|---|---|---|---|---:|---|---|
| line tables | 1,091 MB | 6.98 6.79 s | 4.42 4.16 s | 2.2 1.7 s | 16.0 14.8 s | 208 MB | 1.81 1.63 s | 1.56 1.74 s |
| variables and types | 1,366 MB | 7.37 7.74 s | 5.06 4.85 s | 2.2 1.8 s | 17.0 16.6 s | 244 MB | 1.83 1.72 s | 1.78 1.65 s |

The IR is 25% more and the PDB 17% (Entity, FluidBox and Vector<int32> complete classes in it); a
clean build about 1.4 s longer; an edit's build the same within the noise.

The first such build found invalid exception-handling IR that every build had: a handler's
cleanups unwound to the caller instead of to where the handler's catchswitch unwinds. LLVM's
verifier rejects that only when the module has debug info (51 of the 64 modules); spec
language/throw_from_handler_cleans_up_enclosing_scope builds with `--debug-info` to keep it so.

## The crash report

The game's crash report (programs\factorio\native\crash_report.c, linked by run.cmd) names each frame
from the PDB and, when the objects carry line tables, its kpp file and line:

```
kpp crash report: exception 0xc0000005 at 00007FF7D21B135E (writing 0x0)
   0  t.exe!kpp.main+0x5e  C:\kpp-sandbox\scratch\codegen\dbg\t.kpp:17
   1  t.exe!main+0xf
```

## Visual Studio and WinDbg

Visual Studio: open the solution `kpp --generate-visual-studio-files` writes (build.md) and press F5,
or `devenv /DebugExe out.exe`. A breakpoint in a `.kpp` file binds through the PDB's file paths,
which are absolute. WinDbg: `windbg out.exe`, `.lines` on, `bp program!kpp.main`, `p` and `t` step by
line (not tried: neither cdb nor a working lldb is installed on PC-PRACOVNA).

Tried in Visual Studio 17.14.42 on 2026-11-04 (the experimental instance, driven through DTE, the
kpp extension installed; scratch programs, not a test of run_tests):
- A small program (a Vector, a String, a struct; main calls sum, which calls square), its generated
  solution. The build from Visual Studio takes 2.4 to 5.5 s. A breakpoint on a line of sum is hit 1.0
  to 2.0 s after F5. The call stack reads `sum(Vector<int32>&)`, `kpp.main`, `main` (the C runtime's).
  Step into stops in square at its declaration's line (the entry block's), step over at its first
  statement, step out back at the call's line. A breakpoint on `return 0;` is hit.
- The game (factorio-in-kpp c9cda971, its generated solution, Debug): the startup project builds in
  12.9 s. A breakpoint on Main.kpp:27, `SystemUtil::preventSleep();`, is hit 1.7 s after F5 with the
  stack `fmain(int32, char16_t**)`, `kpp.wmain`. Step into stops in SystemUtil.kpp at
  preventSleep's line 695, step over at 699, step out back in Main.kpp at line 30. A second
  breakpoint, line 35, is hit.
- Locals and watches showed nothing ("identifier "values" is undefined"): the line tables had no
  variables. A debug build has them since 2026-11-04 ("Variables and types" above).

## Values in Visual Studio

A build with `--debug-info` for Windows (MSVC) also writes `NAME.natvis` beside `NAME.exe` and links it
into the PDB (`/NATVIS`), where Visual Studio finds it with the program: nothing to install. It
shows the library's values as values (compiler\src\natvis.cpp):
- `String` and `StringView` as their text (a String's last byte tells a small string's length from
  the heap's mark, largeMark, taken from the library's literal);
- `Vector<*>` and `Span<*>` as their elements, `UniquePtr<*>`, `SharedPtr<*>` and `Optional<*>` as
  what they hold (or `empty`), `Pair<*>` as `(first, second)`;
- `HashMap<*>` and `HashSet<*>` as their entries: one rule per HashTable instance the program has,
  which casts the slots to its entry type by name.

The type names are kppc's (TypeTable::toString, qualified: `HashTable<Pair<String, int32>, String,
HashMapKey<String, int32>, Hash<String>, EqualTo<String>, true>`), which are the PDB's: agreed with
thread B (2026-11-04), whose types carry each instance's full argument list. A rule reads the
library's members by their names; each is checked against the library's classes as they are, and
one whose member is gone is left out with `warning: natvis: ...`. The per-instance rules are sorted
by name, so the file does not depend on the threads.

`tests\natvis.ps1` (run by run_tests.cmd) builds `tests\natvis\values.kpp` with `--debug-info` and
checks that kppc warned of nothing, that the .natvis is valid by Visual Studio's natvis.xsd, that
the PDB holds it (its named stream `/src/files/PATH`) and that its rules are `values.expected`.
What a rule shows in Visual Studio can be checked only once the PDB has the types (thread B).

## How kppc emits it

The emitter writes a marker line `  ;@LINE` before each statement (`Emitter::debugLine`, not inside a
body inlined into its caller). When the function is done, `Emitter::debugLines` appends
`, !dbg !N` to every instruction after the marker, then the function's `DISubprogram` and one
`DILocation` per distinct line. Each function numbers its metadata from 0, ten digits wide, so that
its text does not depend on where it lands; module assembly (`renumberDebugIds`) rewrites the ids in
place to the module's, after the module's fixed `!0` to `!15` (`debugModuleTail`: the compile unit,
a placeholder file, the empty subroutine type and the CodeView flags). The file of each function is
an inline `!DIFile` with the source's absolute path.

With variables (an unoptimized build): a block statement opens a lexical block (`openDebugBlock`),
which the markers then name (`  ;@LINE:BLOCK`: a location per line and block); each parameter and
local is recorded (`Emitter::debugVariable`) and gets a `#dbg_declare` after the allocas. A
function's own ids are then 0 its DISubprogram, 1 its file, its blocks, its variables, its
locations. A variable's type is a placeholder in the function's text, `!T` and 16 hex digits: the
hash of the type's IR spelling (`Emitter::debugType`), whose metadata text is made once per build
into Shared::debugTypes, its own ids from 0 as a function's. Module assembly (`appendDebugTypes`)
collects the placeholders its definitions name and the types those name, appends each type's text
once in key order (so the text does not depend on the threads), renumbers it after the
definitions' ids, and writes each placeholder's id over it in place, padded with blanks. Classes
are defined after the emission (`homeDebugClasses`, from prepareModuleTexts: then every layout the
program has is laid out) for their home's instance piece (a debug build's program pieces are
assembled earlier, so they declare every class). A compile server's build makes a type's text
again when it first names the type (an edited enum's enumerators), and keeps the texts its kept
definitions name.
