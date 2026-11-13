# Debugging kpp programs

`kpp --debug-info` puts line tables (CodeView) into the objects and links with `/DEBUG`, so a
`.pdb` lands next to the executable: a debugger maps every instruction to the kpp line it came from,
breakpoints go on kpp lines, and stepping walks the kpp source.

```
compiler\build\kpp.exe --debug-info --exe out.exe program.kpp
```

The converted game: `set KPP_EXTRA=--debug-info` before `programs\factorio\run.cmd` (run.cmd links
with `/DEBUG` already, for the crash report's function names).

## What is in it

- Each function and lambda is a `DISubprogram` at its declaration's line, named by its IR name
  (`square(int32)`, `Counter::add(int32)`). A lambda's closure is named by a tag and its number:
  in a function at file or class level, the function's tag and its number in the function
  (`inspect::__lambda_ec11f6bb_0::operator()(int32)`), elsewhere its file's tag and its number in
  the file. The file's tag is a hash of the file's path under its root (`std/...` for the standard
  library's, the root directory's name and the path under it for the program's), a function's a hash
  of its file's tag and its header's tokens, so the name is the same wherever the sources are and
  whatever files come before it, and a lambda added to one function renames none of another's. A file's private entities (design.md 15) carry its name and the
  same tag (`inflate.kpp@3f0c9a12::helper(int32)`).
- Every statement starts a line: its instructions are at the statement's first line. A `for`'s
  condition and step, a `while`'s and `do`'s condition and a range-`for`'s advance are at their own
  lines, so stepping through a loop comes back to its header. A `switch`'s cases are at their own
  statements' lines.
- A block's closing brace has a line where something happens there, as in clang's tables (battery
  f14, 2026-11-06): the destructors of the block's objects, whichever way it is left (the fall
  through, a `return`, a `break`), and a function's end whatever it destroys, so a breakpoint on a
  function's `}` stops there and stepping into a destructor comes from the brace. A statement that
  leaves a block and has no instruction of its own (`continue;`, `return;`, `return 0;`) keeps the
  first destruction at its own line, for a breakpoint there; clang has a jump or a store at that
  line, and the `}` too. A `catch` block's end without objects has no line of its own.
- The entry block's allocas and parameter stores are at the declaration's line (the debugger stops
  there first, then on the first statement).
- A trivial callee inlined into its caller (docs/compile-speed.md, "Design: inlining trivial
  functions in kpp") has no lines of its own: its instructions are at the call's line, and stepping
  into it steps over it. `--no-inline` keeps every call, for stepping into getters.
- Variables and types, in an unoptimized build (2026-11-04, "Variables and types" below): every
  parameter and local, `this` too, by its kpp name and visible in its block, with its type; every
  class with its members, bases and vtable pointer as kpp lays it out, named as kpp's IR names it
  (`game::Entity`, `Vector<int32>`). An optimized build with `--debug-info` (build.kpp's fastdebug)
  keeps the line tables alone (`emissionKind: LineTablesOnly`).
- Not covered: the startup functions of global initializers (no lines); a function's static locals
  (globals of the program, not described yet).

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
  variables where its code is. What a statement declares in its head (`for (int i = 0; ...)`, a
  range-`for`'s variable, `if (int c = f())`, `while`, `switch`) is in a block of that statement, and
  a catch parameter in one of its handler (2026-11-06; battery f15: they were the function's, so two
  loops' `i` were two variables of one scope, and a catch parameter was not described; Visual Studio
  now reads the second loop's `i` as 100, 101 in a loop from 100). clang has `while`'s and `switch`'s
  and a catch parameter at the function's scope. A variable's address is its alloca, or the pointer a parameter
  passed by address is; a reference's type is the reference, its slot holding the pointer. A
  lambda's captures are its operator's variables by their names: one by value is its field of the
  closure, one by reference that field's reference (`int&`), a captured `this` the enclosing class's
  pointer, to const in a const member function as that function's own `this` (until 2026-11-08 it
  dropped the const).
- **Member functions** (CodeView): a function with `this` is its class's, as clang declares one: its
  scope the class, its name without the class's, its type one whose first parameter is the object
  pointer (DIFlagArtificial | DIFlagObjectPointer). The PDB has it as a member function (LF_MFUNC_ID,
  LF_MFUNCTION), so Visual Studio evaluates `this` and `this->member` in it; as a free function it said
  "'this' may only be used inside a nonstatic member function" (thread F, 2026-11-05). A static member
  function and DWARF's functions stay free functions; the class's field list does not list its methods.
- **Globals**: each global of the program by the name kpp's IR gives it (`game::counter`,
  `PrototypeList<EntityPrototype>::sorted`; a static data member's storage is `Class::member`), where
  the debugger reads it: the module whose text defines it (its home, in a debug build) describes it
  (S_GDATA32, S_GTHREAD32 for a thread_local one).
- **Basic types** as CodeView has them, without names of their own (`int`, `__int64`, `unsigned
  char`, `double`), each of kpp's integers a typedef of its name too: the PDB's S_UDT `int32`,
  `uint8`, which a natvis rule's `$T1` parses.
- **Pointers, references, const, arrays** (their length), function pointers with their signature
  (until 2026-11-06 every one read `void ()*`). noexcept is no part of a function type in CodeView, so
  `int (*)(int)` and `int (*)(int) noexcept` are one record, as clang's; only a class named after them
  differs (`Box<int32(int32) noexcept*>`).
- **Pointers to member functions**: the function's const and volatile are the class `this` points to,
  its `&` and `&&` a bit of that pointer's record, as clang's, and a member function's own type says the
  same (battery f17, 2026-11-06; before, the first two reached a const member function only).
- **Enums** with their enumerators and underlying type.
- **Classes** with their data members at their offsets in kpp's own layout (a bit field at its
  bits), each with its access as written (`private:` in a class's public part, and so on), their
  static data members (LF_STMEMBER), their bases at their offsets with their access, a polymorphic
  class's vtable pointer (CodeView's LF_VFUNCTAB), an
  anonymous struct's or union's members as the enclosing class's (as MSVC's PDBs list them:
  String's `heapChars`). A class or enum is named as kpp's IR names it, the whole spelling
  (TypeTable::toString, qualified: `game::Entity`, `Vector<Vector<int32>>`, every template argument
  with the defaults), which identifies it too: the natvis rules match these names (agreed with
  thread F, "Values in Visual Studio" below).

Each class is defined once in the program, in its home module (the hash of its name over the
modules, as a first use's home), and declared in every other module that names it (DIFlagFwdDecl,
the same identifier): lld-link merges the modules' types into the PDB and Visual Studio resolves a
declaration by the identifier. A home lists its classes in its compile unit's retainedTypes (LLVM's
CodeView writer emits a type only when something in its module names it). A class the program only
points to, never laid out, has its declaration alone.

A polymorphic object shows its dynamic type (a `Shape&` as `{Square}`, with a `[Square]` member;
2026-11-06): Visual Studio reads it from the symbol the object's vtable pointer points at, the name
MSVC gives a vtable (`??_7Square@game@@6B@`, "const game::Square::`vftable'", and
`??_7Cube@@6BShape@@@` for a secondary one), so a debug build for Windows names each table so at its
address point, as clang does: the table private in a comdat keyed by that name, an alias (Layout.cpp,
emitVtables). The linker keeps one table per class however many modules have it. A class MSVC would
name otherwise has none: one with template arguments, a local, anonymous or file-private one, one in
an anonymous or inline namespace or in a class template, and std's classes and `type_info`, whose
names the C runtime's own tables have. tests\debuginfo.ps1 checks the symbols of values.kpp's two.
On the game (factorio-test's debug build, 2026-11-06, Visual Studio 2022 stopped in
TestScenario::addEntityRaw): `entity` reads `Entity * {Inserter}` with an `[Inserter]` member, the
same for ElectricEnergyInterface, ElectricPole and ContainerEntity, an entity's `prototype` member
`const EntityPrototype * {ElectricEnergyInterfacePrototype}`, and through a secondary base
`(PostTransferNotifiable*)entity` is `PostTransferNotifiable * {Inserter}`.
Not guarded: a C++ library linked in (built by MSVC or clang) with a polymorphic class of the same
qualified name as a kpp class has a table of the same symbol, in a comdat too, and the linker keeps
one of the two for both classes without a word (kind any or exactmatch alike; checked with a
clang-cl object defining Shape, 2026-11-06), a wrong answer of a debug build. None in the game:
none of the 21,828 names of factorio-test's debug build is among the 42 its libraries define (sdl,
curl, re2, imgui, open-vcdiff, ...), and a converted class could not have one, the C++ build linking
the same libraries. A strong definition in one home module would make it the linker's error
(LNK2005), which the module-text homing would have to do (listed for thread B).

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

## The edit's link

A debug build's edit relinks with link.exe's incremental link (`--incremental-link`: `/INCREMENTAL
/OPT:NOREF`), which updates the executable, its .ilk and the PDB in place.

Profiled on the game (factorio-in-kpp c9cda971 in debug: 256 modules, link.exe 14.51; B's
scratch\codegen\linkprof, 2026-11-04):
- The edit of one function relinks in 0.44-0.47 s on a quiet machine, and in 0.7-1.5 s while other
  builds run, for the same CPU time.
- The first link after its files have left the file cache takes 0.8-1.3 s (here after 8 idle
  hours, on a machine that empties its standby list hourly).
- The edit's size hardly matters: a changed object of 13 MB costs what one of 1.5 MB does, and 8
  changed objects cost 0.05 s more than one.
- About 70% is the PDB's update; without `/DEBUG` the link takes 0.14 s.
  - link.exe reads the PDB's global streams, 134 MB: symbol records 70 MB, IPI 31 MB, DBI 19 MB,
    TPI 12 MB, publics 6 MB, globals 2 MB.
  - That is about 420 bytes per function: its public, its procedure reference, its LF_FUNC_ID and
    three section contributions.
  - It writes 33 MB.
- The rest is the image: the .ilk (298 MB) and the executable.
- What does not help:
  - `/NATVIS` costs nothing;
  - lld-link `/DEBUG:GHASH` links the whole program every time (2.2 s while other builds ran; 1.2 s
    quiet with one section per module; its type merging is only 0.06 s, each class being defined
    once);
  - link.exe 14.51 no longer has `/DEBUG:FASTLINK`.

An --incremental-link build compiles its modules without `-ffunction-sections -fdata-sections`.
Its link keeps every function anyway, so a module's functions share one section instead of one each.
The PDB's section contributions go from 650,000 to about 130,000 and the .ilk from 298 to 157 MB.

On a quiet machine (load 0-2%; link.exe alone on copies of the game's objects, both ways
interleaved, the medians of 9 rounds and of 5 for the grown function, each series' first round,
its files cold, left out; tests\sections.ps1 checks the sections and an edit loop):

| | before | with one section per module |
|---|---:|---:|
| edit's link, a changed string | 0.45 s (0.44-0.47) | 0.37 s (0.36-0.38) |
| edit's link, a function grown | 0.72 s (0.72-0.73) | 0.52 s (0.52-0.54) |
| clean build's link | 1.53 s | 1.05 s |

kpp's own edit loop on the game (its compile server; link.exe's `/time+`): a changed string 0.35 s
(0.76 s as the first edit after the server started), a grown function 0.51 to 0.62 s, a new
function and its removal 0.36 s each. Every link stayed incremental.

Two more levers were measured, neither taken (relative to one section per module; kovarex,
2026-11-04: "Ok, with E5, leave it"):
- CodeView function names without the parameter list (`Map<...>::erase`): 0.35 s, -5%. Overloads
  would share a name in the PDB, so a crash report's stack could not tell them apart.
- Short linkage names (`k` and 16 hex digits): 0.34 s, -8%. Link errors, `--emit-llvm` and every
  tool show the IR's names.
- Both: 0.32 s, -14%.

What is left, about 0.3 s, is a cost per function of the whole program. Only a smaller link per
edit would remove it, for example the debug program as several DLLs.

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

A build with `--debug-info` for Windows (MSVC, or windows-gnu, the bundled toolchain's default) also
writes `NAME.natvis` beside `NAME.exe` and links it into the PDB (`/NATVIS`; lld's MinGW driver takes it
as `--Xlink=/natvis:`), where Visual Studio finds it with the program: nothing to install. Until
2026-11-05 a windows-gnu build had none, so a project built with an installed kpp showed `String` and
`Vector` raw in Visual Studio (gorating with kpp 0.2.519); tests\windowsgnu.ps1 checks its PDB. A
program's own rules for its own types go in beside it: build.kpp's `executable.natvis("FILE")` (kpp's
`--natvis FILE`; build.md), as Factorio's tools\vs15\Visualizers\factorio.natvis for the converted
game. It shows the library's values as values (compiler\src\natvis.cpp):
- `String` and `StringView` as their text (a String's last byte tells a small string's length from
  the heap's mark, largeMark, taken from the library's literal);
- `Vector<*>` and `Span<*>` as their elements, `UniquePtr<*>`, `SharedPtr<*>` and `Optional<*>` as
  what they hold (or `empty`), `Pair<*>` as `(first, second)`;
- `SharedPtr<*>` and `WeakPtr<*>` with their use count (`[2 strong]`; a SharedPtr's `[weak count]`
  too, the control block's weak count less the one the owners hold), a WeakPtr as `expired` once
  its object is gone;
- `Deque<*>` as its elements, element i in block (start + i) / blockSize() of its map. The rule
  computes blockSize() from `sizeof(**map)`, so it is checked against Deque::blockSize's text: a
  changed blockSize leaves the rule out with a warning;
- `HashMap<*>` and `HashSet<*>` as their entries: one rule per HashTable instance the program has,
  which casts the slots to its entry type by name;
- `List<T>` and `ForwardList<T>` as their elements, `Map<*>`, `Set<*>`, `MultiMap<*>` and
  `MultiSet<*>` as their entries in order (the RbTree, as a tree); one rule per instance of List,
  ForwardList and RbTree, which casts its links to its node type (ListNode<T>, ForwardNode<T>,
  RbNode<E>) by name;
- a typed union (`union enum`) as its active alternative, `Integer 12`: one rule per class (and per
  instance of one in a template) by its tag `__tag`, valued in the alternatives' order, and the
  alternatives' storage `__alt_<Name>`, the names the parser gives them and the PDB keeps.

The type names are kpp's (TypeTable::toString, qualified: `HashTable<Pair<String, int32>, String,
HashMapKey<String, int32>, Hash<String>, EqualTo<String>, true>`), each instance's full argument
list, made in one place: the debug information names its classes (Emitter::debuggerTypeName and
debuggerClassName, compiler\src\Codegen\DebugInfo.inl) and hands the classes it defines with those
names to the natvis (CodegenResult::debugClasses, the classes of every module, a compile server's
build too), whose per-instance rules are of those classes by those names. Closing angle brackets
are kept apart, `RbNode<Pair<String, int32> >`, as MSVC's PDBs do: Visual Studio's expression
evaluator reads `>>` as a shift, and with the PDB's
`RbNode<Pair<String, int32>>` neither a cast to it nor a rule for `RbTree<..., Less<String>>` was
possible (`Map<String, int32>` showed raw).

A UniquePtr with its default deleter is named as the program writes it, `Point^` (design.md 20;
debuggerClassName): the Watch, Locals and Autos windows show `Point^` as its type,
and the natvis has the UniquePtr rule once more by each such name, which `UniquePtr<*>` does not
match. Visual Studio's expression evaluator reads `Point^` in an expression as a C++/CLI handle,
so a cast to it (`(Point^*)p`) is "a handle to a non-managed class is not allowed"; `p.held->x`
reads the object. (`p->x` does not evaluate with either name: the evaluator does not call
UniquePtr's operator->.) Checked in Visual Studio 17.14 on 2026-11-04. A rule reads the
library's members by their names; each is checked against the library's classes as they are, and
one whose member is gone is left out with `warning: natvis: ...`. The per-instance rules are sorted
by name, so the file does not depend on the threads.

`tests\natvis.ps1` (run by run_tests.cmd) builds `tests\natvis\values.kpp` with `--debug-info` and
checks that kpp warned of nothing, that the .natvis is valid by Visual Studio's natvis.xsd, that
the PDB holds it (its named stream `/src/files/PATH`) and that its rules are `values.expected`.
Checked in Visual Studio 17.14.42 on 2026-11-04, once the PDB had the types (thread B's bcbbed6e):
values.kpp as a project of its own, Debug, stopped on its `println`, each local read through DTE's
GetExpression, which applies the natvis:

| local | Visual Studio shows | expanded |
|---|---|---|
| `Vector<int32>` | `{ length=2 }` | `[capacity] 4`, `[0] 2`, `[1] 3` |
| `String` (small) | `"kpp"` | `[length] 3` |
| `String` (heap) | `"a string longer than thirty characters, on the heap"` | `[length] 51`, `[capacity] 51` |
| `StringView` | its text | `[length] 51` |
| `HashMap<String, int32, Hash<String>, EqualTo<String>, true>` | `{ count=2 }` | `[capacity] 16`, `("alan", 41)`, `("ada", 36)` |
| `HashSet<int32, Hash<int32>, EqualTo<int32>, true>` | `{ count=1 }` | `7` |
| `Optional<int32>` | `5`, `empty` | |
| `Point^` (UniquePtr<Point>) | `{x=4 y=0 }` | `x 4`, `y 0` |
| `Property` (a typed union) | `Integer 12`, `Text "gear"` | `[tag] Integer`, `Integer 12` |
| `Deque<int32>` (40 elements, 3 blocks) | `{ length=40 }` | `[0] -20` ... `[39] 19` |
| `List<String>` | `{ length=2 }` | `"belt"`, `"cog"` |
| `ForwardList<int32>` | `{ front=9 }` | `9`, `8` |
| `Map<String, int32, Less<String>>` | `{ count=3 }` | `("assembler", 2)`, `("chest", 1)`, `("inserter", 3)` |
| `Set<int32, Less<int32>>` | `{ count=3 }` | `10`, `20`, `30` |
| `HashMap<int32, Vector<int32>, ...>` | `{ count=1 }` | `(1, { length=1 })` |
| `SharedPtr<Point>` (two owners, one WeakPtr) | `{x=0 y=6 } [2 strong]` | `[use count] 2`, `[weak count] 1`, `x 0`, `y 6` |
| `WeakPtr<Point>` | `{x=0 y=6 } [2 strong]` | `[use count] 2`, `x 0`, `y 6` |

Every one has its `[Raw View]` too. (tools\vs\vsix\natvis-check.ps1; not part of run_tests, which
has no Visual Studio session.)

### debugValue

A type says what the debugger shows with `T debugValue() const` (design.md 26, decided 2026-11-11): the
watch window shows the value as what debugValue returns, and kpp writes that into the .natvis as one
rule per class instance, so nothing runs in the debuggee and a crash dump shows it too (built
2026-11-11, compiler\src\natvis.cpp):

```
template<i32 Bits> class Fixed
{
  static constexpr i32 ValuesPerOne = 1 << Bits;
  f64 debugValue() const { return f64(this.value) * (1.0 / This::ValuesPerOne); } // 1.5
  i32 value = 0;
};
struct Item { std::String debugValue() const { return this.name; } std::String name; }; // gear (a text view)
struct Load
{
  f64 debugValue() const
  {
    if (this.empty)
      return 0;
    return this.weight.debugValue() * 2; // Fixed's body, inlined on weight
  }
  bool empty = false;
  Fixed<12> weight;
};
struct Buffer { std::Span<i32> debugValue() const { return std::Span<i32>(this.data, this.size); } i32* data; usize size; }; // {length=3}, [0] [1] [2]
```

- **The body**: `return EXPRESSION;`, after any number of `if (CONDITION) return EXPRESSION;` and
  `switch (VALUE) { case A: return EXPRESSION; ... default: ... }` (each a case of the rule, a DisplayString
  with that Condition); a body may end in a `[[noreturn]]` call instead (Comparison::str()'s
  `logEnumAndAbortOrThrow` after its switch: no case for the values it has none for).
- **Written with the type's own names and conversions** (kovarex, 2026-11-12, reviewing the game's first
  debugValues: "in the kpp code we should write nicely"): `this == Force::player.id` compares a class whose
  `==` is defaulted member by member against the folded constant (`index == 1`); `return this.str();` uses
  the conversion the type has, inlined with its cases and choices: MapVersion's `std::toString(a, ".", b,
  std::StringView(this.branchVersion ? " (branch)" : ""))` (std::Text's pieces, a class's `operator bool`,
  a choice between texts as two DisplayStrings, natvis's expressions having no string literals),
  Comparison's switch of signs (`≥`, its `\xE2\x89\xA5` escapes read), StationID's name through its
  SizeLimitedString's `operator const std::String&`. An expression reads the object's data members
  (`this.x`, chains of members, `->` and `*` through pointers); what reads none is folded to a
  constant under the instance's arguments (`This::ValuesPerOne`, an enumerator, `1.0 / 256`), so
  `Fixed<8>` shows `{(double)(value) * 0.00390625,g}` and `Fixed<12>` its own scale; arithmetic,
  comparisons, `!`, `~`, `?:` and casts to a number type (`f64(x)`, `(i32)x`, `static_cast`) stay as natvis
  writes them; a call of a const member function without arguments, non-virtual, is its body inlined on
  its object (one `return`, after constexpr locals, no `if`): the type's own conversion
  (`f64 debugValue() const { return this.toDouble(); }`, kovarex 2026-11-12), another type's debugValue on
  a member (`this.weight.debugValue()`). A constexpr local (`constexpr f64 PerStep = 1.0 / 4294967296.0;`)
  folds to its value.
- **What it returns** says how the type shows: a number, a bool, an enumeration or a pointer as that
  value (a floating one with `,g`, `1.5` and not `1.5000000000000000`); a `std::String` built with `+`
  as text (2026-11-12, kovarex's plan to replace the game's factorio.natvis): its string literals as they
  are, `std::toString(x)` and other values each as `{x}` by its own view (a String without quotes, an 8-bit
  integer as its number), so `"{index=" + std::toString(this.index) + "}"` shows `{index=7}` and the
  program calling it gets the same text; an enumeration cast from a member (`Enum(this.value)`) as its
  enumerator's name; a class pointer as itself, expanding to the object; a `std::StringView` of a `char`
  array, or of a pointer and a length, as its characters (`{text,sb}`, `{p,[n]sb}`). A cast may name a
  number type, an enumeration, or a pointer or reference to a class, the object itself too:
  `static_cast<const Entity&>(this).position` reads the derived class's member (`(*(Entity*)this).position`); a data member that is a class
  as that class's view, and its expansion (`ExpandedItem`); a `std::Span` made of a pointer and a length
  (or two pointers) as `{length=N}` and its elements (`ArrayItems`). Any other type returned is an error.
- **Another object's debugValue in a text** (a member's: `this.first.debugValue()`) is that object by its own rule,
  `{first}`, not its text inlined, whose cases would multiply with the others' (the game's CircuitCondition: two signals
  and a comparison). tests
atvis.ps1 checks Match, `{left} vs {right}`.
- **A class's static data member** is read too, a global the program writes, by its name in the debug information:
  `Registry<T>::all[this.index]->name` in Handle<Part> is `{Registry<Part>::all.items[index]->name,sb}` (an element of
  the library's Vector is its `items[i]`, of a pointer or array `[i]`). The game's ID<T> shows its prototype's name so
  (2026-11-13). tests\natvis.ps1 checks Handle<Part>; tools\vs\vsix\debugger-check.ps1 saw `gear` and `{0}` in
  Visual Studio.
- **Any member, private ones of the bases too**, may be named in a debugValue, which no other function may (kovarex,
  2026-11-13): the debugger sees them all, and a view reads the members themselves rather than the functions that guard
  them, whose asserts a view cannot evaluate (Targeter's view reads its base's `target`, not `get()`). Specs
  kpp/debugvalue_names_private_members_of_bases, kpp/debugvalue_only_names_private_members_of_bases.
- **Anything else is a compile error** at its place, naming what it was: a call of any other function
  (a free function, a virtual or non-const one, one with arguments), an operator function, a variable, a statement other than those `if`s and `return`, a debugValue that is not
  const or takes parameters ("FILE:LINE:COLUMN: error: debugValue of Counted cannot be shown by the
  debugger: a call ..."), in every build and in the language server's check (codegen lists the classes laid
  out that have a debugValue, CodegenResult::debugValueClasses; the driver translates each), on every target.
- **Not yet** (design.md 26, Open): loops (a linked list's nodes), the standard library's own rules
  written as debugValue methods, the converted game's FixedPointNumber; and LLDB and GDB, once kpp writes
  DWARF (stage 2).
- tests\natvis.ps1 checks the rules of natvis\values.kpp's Fixed<8>, Item, Load and Buffer and the errors
  of two bodies the debugger cannot compute; in Visual Studio (exe-check, 2026-11-11) `item` showed
  `"gear"` (`gear` since text views, 2026-11-12), `item.speed` `1.5`, `load` `1`, `buffer` `{length=3}` with `[0] 4`, `[1] 5`, `[2] 6`.
- **A set of flags** (`flags`, design.md 27) has its view without a debugValue: the names of the flags set,
  `Red | Blue` (kovarex, 2026-11-12: "it will show all the named flags when more is selected in the form of A |
  B | C | D"), a bit no flag names as its number at the end (`Red | Blue | 64`), `{}` when none is; and a row
  per flag's name, true or false (a second name of a bit too), with `[bits]`. Natvis has no loop, so the value
  line is a chain of views of the one object, a view a flag (`kpp0`, `kpp1`, ...: its name when set, ` | ` and
  the next view when a later bit is set too, `{*this,view(kpp1)}`); Visual Studio drops a display's leading
  spaces, so the separator goes after a name. A debugValue of its own replaces it. tests\natvis.ps1 checks
  natvis\values.kpp's Usage; tools\vs\vsix\debugger-check.ps1 (2026-11-13) saw `{}`, `Green`, `Red | Blue |
  64` and a set of 20 flags in full in Visual Studio.

### this in the watch window

Visual Studio takes `this.x` as kpp writes it (kovarex, 2026-11-09: "In debug window we need to write
this. to make it consistent"): in the watch window, a hover, the Immediate window and a breakpoint's
condition, `this.x` and `this.f()` work, and `&this` is the pointer. Formats (`this.x,x`) and assigning
(`this.x = 3`) work too. `this->x` and a bare `x` still work.

It is two debugger engine (Concord) components in kpp-visualstudio.vsix:
- `KppFrameLanguage` gives code compiled from a .kpp file a language of kpp's own (kpp.pkgdef registers
  it). A stack frame takes its language from the symbol information it carries, so a call stack filter
  replaces that; a breakpoint's condition asks the symbol query, which it answers too. The Language
  column of the call stack says kpp; C and C++ frames stay C++.
- `KppExpressionEvaluator` is the evaluator of that language. It writes `this.` as `this->` and `&this` as
  `this`, outside strings, and passes the text to Visual Studio's C++ evaluator in a C++ inspection
  context. The results carry that context, so Locals and expanding a value are the C++ evaluator's, as
  before. Visual Studio applies the natvis inside a PDB only in the context it makes for a C++ frame, so
  this context is given the files by name: the `NAME.natvis` beside the executable and the program's own
  (`--natvis`, listed in `NAME.natvis-files`). Moved without those files, the program's values show raw.
  A type with an `<Intrinsic>` is not applied from a file given this way, so kpp's rules write those
  expressions out (a program's own natvis should too; factorio.natvis has none).
  The context keeps the symbols connection Visual Studio gave the frame's, and the files are compiled
  for it: until 2026-11-12 a value's own line had its rule and its members, expanded on that connection,
  showed raw (a MapPosition's x as `{value=384}` where its line showed `x=1.5`; kovarex).

The debug information stays C++'s, and objects keep naming their compiler `kpp 0.2.5`. Tried and not
used: a component in front of the C++ evaluator, filtered on C++ (what it passes on comes back to it,
and it loops); the registry's `Debugger\CodeView Compilers\1:NAME`, which maps a compiler name to a language and
works, but only for one exact name, so every version would need its own key.

What stays C++'s: in Locals the `this` row is the pointer, and "Add Watch" on a member writes `this->x`
(which evaluates). A bare `this` is the pointer. The debug information cannot change that: giving its
`this` variable a reference type left it as it was (tried 2026-11-07 on kpp ac6731fd), so kpp writes `this`
as a pointer, as clang does.

VS Code's debugger loads no kpp extension: there, `this.x` is still an error or a wrong value (C++'s
evaluator reads `this.count` as a static member's), and `this->x` or `x` is what works.

tools\vs\vsix\debugger-check.ps1 checks all of it in the experimental instance (not part of run_tests,
which has no Visual Studio session).

## How kpp emits it

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
function's own ids are then 0 its DISubprogram, its blocks from 2, its variables, its locations.
A variable's type is a placeholder in the function's text, `!T` and 16 hex digits: the hash of the
type's IR spelling (`Emitter::debugType`), whose metadata text is made once per build into
Shared::debugTypeShards, its own ids from 0 as a function's. Two other nodes are shared the same way
(`Emitter::debugSharedNode`): a file's DIFile, which every definition, block, variable and global
of the file names (`debugFileNode`), and a class's object pointer type, which all its member
functions' types name (`debugObjectPointer`). Until 2026-11-07 each definition wrote its file three
times and each member function its own object pointer: the game's IR was 109 MB larger (1,668
against 1,559 MB), its objects the same. Module assembly (`appendDebugTypes`)
collects the placeholders its definitions name and the types those name, appends each type's text
once in key order (so the text does not depend on the threads), renumbers it after the
definitions' ids, and writes each placeholder's id over it in place, padded with blanks. Classes
are defined after the emission (`homeDebugClasses`, from prepareModuleTexts: then every layout the
program has is laid out) for their home's instance piece (a debug build's program pieces are
assembled earlier, so they declare every class): level by level, the classes named and then the ones
their definitions name, each level's on every emitter at once and taken in key order after (on one
thread until 2026-11-07, about 2.5 s of the game's clean build with the other cores idle). The types
are in 64 shards by key, each with its own lock, and each shard lists the classes as the build names
them, which the next level takes; an emitter keeps the placeholders it was given (a type's by its
type, an object pointer's by its class, a file node's by its file) for the rest of the build. Until
2026-11-07 every ask built the type's spelling and took the shared lock, and every level scanned all
the types: homeDebugClasses was 1.3 s of the game's clean build, most of its samples threads
waiting on that lock. A compile server's build makes a type's text
again when it first names the type (an edited enum's enumerators), and keeps the texts its kept
definitions name.

A global's definition line ends with `, !dbg` and its placeholder, `!G` and the hash of its IR name
(`Emitter::debugGlobal`, which makes its DIGlobalVariableExpression into Shared::debugGlobals); a
module whose text holds that line appends the global's metadata and lists it in its compile unit's
`globals`, and the external declaration other modules get (`externalDeclarationOf`) has none. A
lambda's capture is declared (`#dbg_declare`) right after the instruction that computes its field's
address, before the next instruction, not with the allocas.
