# Faster code generation through tighter LLVM integration

**Status: a proposal for kovarex (thread D, 2026-11-02). Nothing in it is decided.** It answers
kovarex's question "Since these tools are opensource, can't we figure out a tighter integration of
clang and our compiler for better performance?" with measurements on the converted game and throwaway
prototypes in thread D's scratch (`C:\kpp-sandbox\scratch\templates\cg`). Nothing in compiler/ was
changed for it; codegen.cpp and the driver are thread B's. kovarex picked the direction: option (b),
LLVM inside kppc, as b1 (LLVM as a library, our own scheduling) and b2 (LLVM changed to fit kppc).

## The answer in brief

- **LLVM is 6 s of today's 17 s clean debug build; kppc's own codegen is 9 s.** The 9 s is not text
  formatting. kppc's codegen is 23.6 s of work that runs only 2.65 times faster on 32 threads, and
  60% of its samples are threads waiting on its shared locks. Building IR in memory instead of as
  text would not shorten it much.
- **A third of clang's time is the shape of our IR, not clang.** Each of the 128 modules defines its
  own copy of about 11,500 `linkonce_odr` constants, plus whatever constants its own functions use.
  Defining each constant once cuts clang's wall time by 20% (3.70 -> 2.95 s), the objects in half
  (475 -> 260 MB), and the full link by 36% (1.67 -> 1.07 s). It needs no LLVM integration and is
  the cheapest step.
- **967 MB of IR is not abnormal for Factorio** (section 3). clang's own -O0 IR for the game's C++
  alone is about 1.7 GB as the 100 unity files the C++ build compiles, and 4.5 GB as 3,269 separate
  files. kppc emits fewer instructions for the game and its libraries together (9.8 million against
  about 15.7 million for the game alone). What is ours to cut is duplication: globals repeated per
  module are 24% of kppc's IR against 6% of clang's unity IR.
- **b1 (LLVM in-process) is worth about 1 to 1.5 s by itself:** no text written and parsed again
  (parsing is 20% of LLVM's work) and no processes. Its larger gain is **streaming**: LLVM's back end
  can then run on the cores kppc's codegen leaves idle, which hides most of the 3 to 4 s of clang.
- **b2 (a kpp-owned, thread-safe LLVM)** adds little over b1 with streaming. What makes one LLVMContext
  single-threaded (shared constants' use-lists, the uniquing tables, one object writer per module)
  is in LLVM's core. Changing it is years of work, LLVM rejected it before, and every LLVM release
  would have to be merged into it. The middle way, stock LLVM driven by function from kppc's own
  scheduler (one context per worker), gets nearly all of b2's benefit.
- **Expected:** clean debug build 17 s -> about 11 to 12 s with b1, home globals and streaming. That
  leaves kppc's codegen as the floor, and its lock scaling is the next lever, thread B's. Leaf edit
  1.4 s -> about 1.1 s, the link then the largest part. Wide edit 1.9 s -> about 1.1 s with
  per-function caching. The order of steps is at the end.

## 1. Where a clean debug build's codegen time goes

**Method.** The game as run.cmd builds it (incremental link, `/DEBUG` names), from C:\factorio-in-kpp
441967bc (the conversion of C:\Factorio 4c178a5b915 by kpp a5839f47), kpp master f9dbfa24..975355ac,
128 modules, -j 32. `cg\build.cmd OUT [kppc options]` builds into OUT. Wall times are from a quiet
machine (CPU load 0 to 4% before each run) unless marked; rounds are interleaved. The back end
was measured by `cg\proto\cgproto.exe`, a prototype that drives LLVM through the `LLVM-C.dll` Visual
Studio ships (LLVM 19.1.5, the same LLVM as kppc's clang, no headers needed), and by its `spawn`
mode, which runs kppc's own clang command per module through `system()` on N threads as kppc does.
`cg\measure.ps1` interleaves the variants. kppc's codegen was profiled with `cg\sampler\sampler.exe`:
it suspends each busy thread every millisecond, walks its stack with dbghelp, and splits samples by
kppc's report lines. The profiled kppc was the same Release code built with a PDB in cg\kppc-pdb.

**The build, phase by phase** (`cg\build.cmd`, three clean builds at `--codegen-threads` 1, 8 and 32):

| phase | wall at -j 32 | notes |
|---|---:|---|
| read, lex, imports, parse, index, resolve | 1.5 s | resolve 0.75 s |
| kppc codegen (IR emission and module assembly) | 8.9 s | 23.6 s at `--codegen-threads 1`, 9.7 s at 8 |
| writing the 128 `.ll` files (967 MB) | 0.11 s | inside kppc's clang phase; `cgproto write 32` |
| clang -O0, 128 processes | 3.9 s | 3.68 to 3.70 s alone (`spawn:out`, below) |
| link (link.exe, `/INCREMENTAL`, PDB) | 2.1 s | a relink after one object changed: 0.45 s |
| **wall** | **17.1 s** | thread A measured 18.0 to 19.6 s the same day (compile-speed.md) |

**kppc's codegen: lock waits, the heap and the evaluator, not text.** Of 1,778 samples in the
codegen window, 1,061 (60%) are threads blocked in a lock. The takers are `addDefinition` (223),
`structFor` layouts (about 200 across call sites), the shared definitions' destruction (62),
`globalFor` (46) and closures (89). The 717 running samples are:
- the emitter's function bodies (`emitFunction`), 64%;
- the compile-time evaluator folding constants and global initializers (`constOutermost`), 21.5%
  (`constValueOfDecl` alone is the first kppc frame of 13.5%);
- the resolver's walks of instance bodies for codegen, 10%;
- module assembly (`moduleText`), 7%;
- malloc and free together, about 30% inclusive.
Text building is spread across the emitter: string growth 5.9%, memmove 3.6%, memchr 2.4%,
`qualifiedName` and `TypeTable::toString` about 1.5% each. Formatting the text is about 10 to 15% of
codegen's running work, 2.5 to 3.5 s of the 23.6 s. At 32 threads, the 8.9 s is set by the locks.
(compile-speed.md has the codegen thread's history with this lock; it is thread B's design.)

**The IR, 967 MB in 128 modules.** A median module is 7.4 MB; the range is 5.8 to 16.0 MB. Of
module 0 (8.0 MB):
- function bodies, 4.06 MB (50%);
- globals and constants, 2.15 MB (27%). 1.75 MB of it is 11,762 `linkonce_odr` definitions, nearly
  all of which every other module also defines: modules 0 and 50 share 11,568 by name.
- declarations, 0.76 MB;
- COMDAT lines, 0.55 MB;
- types, 0.44 MB.
Over the whole program there are 60,878 distinct `linkonce_odr` globals, each defined in every
module that names it (module 0 is the first home of 12,069). Names are 41% of the text: quoted
global names 23.4%, local value names 17.4%.

**clang's work, one module at a time.** All 128 modules on one thread, in process (cgproto `emit
1`, the load low but not measured per run): 56.8 s (with `-time-passes`) and 61.5 s (without, loaded):
- reading the files, 0.3 s;
- **parsing the text IR, 11.2 s (20%)**;
- code generation, 43 s;
- writing the objects and freeing, 2 s.
`-time-passes` splits the code generator's 29.5 s of timed passes (`cg\tp1.err`):
- instruction selection, 14.2 s (48%), of which SelectionDAG's fallbacks 6.9 s;
- the assembly printer (MC emission), 4.0 s;
- the fast register allocator, 1.0 s;
- prologue and epilogue, 0.9 s;
- Windows EH preparation, 0.74 s;
- two-address, 0.7 s.
The rest of code generation (about 13 s) is outside the pass timers, mostly the object's layout and
writing. The middle end at -O0 is nothing (5 ms per module: the always-inliner, remarks).

**Processes cost little.** Over 16 modules on one thread, interleaved, clang processes took 5.52 to
5.67 s and in-process LLVM 4.67 to 4.81 s (parse 1.3 s of it): a process, its start and the driver
are about 0.05 s per module. Over all 128 at 32 threads the summed thread time is 110 s for processes
and 101 s in process, inflated about 2.4 times by SMT against one thread's 45 s.

**The modules are balanced enough.** At 32 threads the largest module (21, 16 MB, 2.2 times the
median, many large functions rather than one) takes 2.1 to 2.6 s against the phase's 3.7 s; the
phase is the work divided by the cores. Taking the largest modules first gains about 0.2 s
(compile-speed.md, "Clang behind module assembly").

**The leaf edit's back end** (module 71, 7.0 MB, the one holding `AnimationBank::getAnimation`;
module 100 holds `MapPosition::operator+`), 3 rounds:
- clang process, 0.305 s;
- in process on text, 0.27 s (parse 0.08);
- in process on bitcode, 0.22 s;
- in process with one home per global, 0.23 s.
The rest of the edit loop is thread A's numbers (compile-speed.md): server and front end about 0.3 s,
kppc codegen 0.21 to 0.25 s, the incremental relink 0.44 s; 1.39 to 1.51 s on a quiet machine. The
wide edit emits 457 functions in 1 module; 1.9 s quiet.

## 2. The options

All back-end runs below cover the 128 modules at 32 threads, 3 rounds interleaved, quiet machine;
min to max wall (`cg\measure.ps1 -Rounds 3 -Variants ...`).

| variant | wall | objects | what it shows |
|---|---:|---:|---|
| clang per module on kppc's text (today) | 3.68 to 3.70 s | 475 MB | |
| clang per module on bitcode | 3.56 to 3.65 s | 475 MB | (a) |
| clang per module, one home per linkonce global | 2.93 to 3.02 s | 260 MB | IR volume |
| LLVM in process, text | 3.37 to 3.39 s | 475 MB | (b) without in-memory IR |
| LLVM in process, bitcode | 3.02 to 3.04 s | 475 MB | (b) minus most of the parse |
| LLVM in process, one home per global | 2.55 to 2.56 s | 240 MB | (b) with the volume fixed |
| the same, text as LLVM prints it (the control) | 3.33 to 3.40 s | 475 MB | |

The full link of the two object sets (link.exe, the game's link line, 4 rounds interleaved,
`cg\linkab.ps1`): **1.66 to 1.70 s against 1.06 to 1.09 s** with one home per global. Both link; the
home objects are cgproto's, without `-ffunction-sections`, as are their control's.

### (a) Bitcode instead of text: not a cheap win

Through clang processes it saves 0.05 to 0.1 s of 3.7 (3%). Parsing bitcode is cheaper than parsing
text, but the driver, the verifier and the back end dominate, and kppc would need a bitcode writer of
its own: a binary format with abbreviations, value numbering and a symbol table, versioned with LLVM.
In process, bitcode saves 0.35 s of 3.4, but in process the IR needs no format at all. **Skip (a);
it is subsumed by (b).**

### IR volume: one home per global (not in kovarex's list; measured because it showed up)

Every `linkonce_odr` constant, vtable and string is defined in every module that names it, so
clang parses it, emits it into each object (475 MB of objects), and the linker throws the copies
away. Defining each in one home module, declared `external` elsewhere (the prototype gave each
global the first module that defines it: `cg\homes.py`, `cgproto home`):
- clang's wall time: 3.70 -> 2.95 s (−20%);
- in process: 3.38 -> 2.56 s (−24%);
- objects: 475 -> 260 MB;
- the full link: 1.67 -> 1.07 s (−36%).
It costs one placement rule in kppc's codegen (thread B's), like the home modules instantiations
already have. Incremental builds need it to be stable: a global's home must not move when an
unrelated edit adds a user (hash its name to a module, as instantiations are).

### (b1) LLVM inside kppc, our own scheduling

What changes: kppc links LLVM's libraries (IR, X86 back end, MC, lld) statically. Each codegen worker
owns an `LLVMContext` and builds its functions as IR through the C++ API. A unit, a "piece" of
functions, goes to the back end on the same thread pool as soon as it is complete. The back end
writes each object to memory, and lld links in process. A piece's object is cached by a hash of its
IR, so an edit compiles only the pieces whose IR changed.

Measured or estimated gain on a clean build:
- **No text written or parsed.** The parse is 20% of LLVM's work (11.2 of 56.8 s on one thread); in
  process at 32 threads, bitcode already saves 0.35 s, and no format at all about 0.8 s (estimated:
  the parse is 27 of the 101 thread-seconds). kppc stops
  formatting text (2.5 to 3.5 s of its 23.6 s of work) but builds LLVM objects instead. That costs
  memory allocations per instruction and uniquing per type and constant: probably similar work, and
  Zig's contributors name IRBuilder's overhead as a reason to emit bitcode instead. **Net: about 0.5 to 1 s.**
- **No processes:** about 0.05 s per module, 0.2 s of wall at 128 modules.
- **Streaming, the larger part.** kppc's codegen leaves about 60% of the cores idle for 8.9 s (its
  samples wait on locks); the back end is about 45 s of single-thread work (less with home
  globals and no parse: about 30 s). Run on those idle cores as pieces complete, it ends shortly
  after codegen does. What remains after codegen is the pieces of the last levels and the largest
  piece: estimated 0.3 to 0.6 s instead of 3.9 s. Caveat: the back end competes with codegen for
  memory bandwidth and the allocator (malloc is already 30% of codegen's running samples), so
  codegen itself may slow by some tenths of a second; that is the first thing a prototype must
  measure. **Gain: about 3 s.**
- **In-process lld** gains nothing for the debug game: lld's PDB merges the Factorio libraries' types
  (795 MB, 10 s), so the debug link stays link.exe `/INCREMENTAL` (compile-speed.md, "The link:
  link.exe stays"). lld in process is right for release, other targets and small programs.

Costs and risks:
- **kppc's size:** VS's `LLVM-C.dll` with every target is 52 MB. x86-64 only, statically linked, is
  roughly 30 to 40 MB more in kppc.exe (estimate). The self-contained release (design.md 17) ships
  clang (87 MB) and lld (53 MB) today. With LLVM inside kppc, the release still needs clang for the C
  import's preprocessor, until libclang is in-process too.
- **LLVM version coupling:** the C++ API changes every release (opaque pointers replaced typed pointers in
  LLVM 15 to 17, the legacy pass manager is going, debug-info records replaced intrinsics in 19). kppc would build against one pinned LLVM version and
  move with it twice a year. Textual IR is only a little more forgiving.
- **Building it:** LLVM's libraries are a CMake build of 20 to 40 minutes on this machine for
  X86, MC and lld, once per LLVM version. CI would cache them or download the release's static libraries.
- **Determinism:** pieces must be defined by content (a function goes to piece hash(name) % P), never
  by which thread finished first. The back end is deterministic per module. The object list for the
  link is sorted. Then the output is byte-identical at any thread count, as today.
- **Debug info and EH:** both are LLVM's, unchanged. kppc's codegen must emit through the API what it
  emits as text today: a rewrite of the emitter's output layer (thread B's file), large but mechanical.
  The text path can stay as `--emit-llvm` through `Module::print`.

The prototype that remains to be built is the streaming one: kppc's emitter building IR through the
API, on a fork of codegen.cpp. That needs thread B's agreement, which the coordinator arranges. Until
then, the numbers above are the back end measured on kppc's real modules.

### (b2) A kpp-owned LLVM that generates code in parallel within one context

**What makes one LLVMContext single-threaded:**
- **Use-lists on shared values.** Every use of a `Constant`, `GlobalVariable` or `Function` links into
  that value's use-list. Two threads building two functions that call the same function or name the
  same constant race on one list. That makes nearly every IR construction a write to shared state.
  The 2016 RFC that removed use-lists from `ConstantData` (`i32 0`, `null`) was motivated by
  determinism, not threads, and globals and functions keep theirs.
- **Uniquing tables in the context:** types (`StructType`, function types), constants (`ConstantInt`,
  `ConstantExpr`, aggregates), attributes and metadata (`MDNode`, `MDString`) are uniqued in
  `DenseMap`s and `FoldingSet`s without locks.
- **The Module:** its value symbol table and its function and global lists.
- **The pass managers:** the legacy code-generation pipeline runs passes per module, then per function.
  Analyses can be module-wide. One `MCContext`, `MCStreamer` and object writer per module receive all
  functions' machine code in order (sections, symbols, fixups).

Making these thread-safe means locks or sharding in the uniquers (MLIR's `StorageUniquer` does this
by design), use-lists that are not shared or are made atomic, a per-function machine-code buffer merged
into one object at the end, and a pass manager that runs function pipelines in parallel. MLIR built
exactly this, from scratch, as a new IR (functions isolated from above, constants as attributes
without use-lists). When Intel proposed function-parallel code generation inside LLVM in 2013, the
objection (Chandler Carruth) was that the code generators are one set of function passes and much of
LLVM is not thread-safe. Nothing came of it, and LLVM's answer since has been separate contexts:
- `SplitModule` and `splitCodeGen` for regular LTO's parallel code generation;
- ThinLTO's back ends;
- ORC's `ThreadSafeContext`;
- Rust's codegen units, one context each.
None of the projects surveyed (Rust, Zig, Swift, the ORC users) runs one context on many threads.
Zig instead moved its debug builds off LLVM (its own x86-64 back end, the default for debug builds
on Linux and macOS since 0.15) and links LLVM only as a separate bitcode compilation for release.

**What a fork costs:**
- **The patch** touches `Value`, `Use`, `Constants`, `LLVMContextImpl`, the Module and the MC layer:
  core files, changed upstream every week. Every six-monthly release would be a merge of hundreds of
  conflicts in exactly those files. Estimate: several engineer-months for a first working version and
  weeks per release after.
- **Build:** the same LLVM build as b1, but no prebuilt libraries: CI builds the fork. The licence is
  fine: Apache-2.0 WITH LLVM-exception, the same as kpp.
- **kppc's size:** as b1.
- **Risk:** a subtle race in a fork no one else runs is a silent miscompile in the field. That is the
  class of error the project ranks above all others.

**What b2 would add over b1 with streaming:**
- one set of types, constants and declarations instead of one per worker context (b1 builds each
  declaration once per context that uses it: 32 contexts, cheap in memory, about what the 128
  modules' declarations cost as text today, 0.8 MB each);
- functions handed to the back end one by one with no piece boundary.
The first is a few percent of the back end. The second b1 gets nearly in full with small pieces,
since a piece is "the functions one worker emitted since its last piece". So the gain is small and
the cost large.

**The middle way, stock LLVM at function granularity from our scheduler, is b1 with small pieces.**
Each worker keeps its own context and an open module. Its functions are built into it as they are
emitted, and every few hundred functions (or at the end of a codegen level) the module goes to the
back end and a new one opens. That is kovarex's "generate code multithreaded and in parallel as it
is generated by our first step" with stock LLVM. **I recommend this over a fork.**

A small LLVM patch that is upstreamable instead of a fork: X86 FastISel does not lower Win64
arguments, so every function of the game falls back to SelectionDAG for its entry (2,804 fallback
reports for module 0's 2,803 functions; for a Linux target 1,089). SelectionDAG is 6.9 s of the
back end's 29.5 s of timed passes; the share from arguments alone is not measured. Implementing
`fastLowerArguments` for Win64 would be a contribution to LLVM, not a fork.

### (c) Finer-grained incremental codegen

Today an edit compiles again each module whose text changed: the leaf edit 1 module of 7 MB
(0.3 s), the wide edit 1 module (457 functions emitted). With pieces cached by the hash of their IR,
a leaf edit compiles one small piece. How others do it:
- **Rust's codegen units:** two per source module, stable and generic code, merged to a maximum
  count. A unit whose items or inlined callees changed is rebuilt whole, since LLVM cannot update a
  module in place.
- **Zig:** a declaration is its own unit, patched into the output in place.
- **Swift:** batch mode per file.
For kppc, pieces of 50 to 200 functions by hash of name keep the per-piece declarations' overhead
small and make the leaf edit's back end about 0.01 to 0.03 s instead of 0.3 s. **Gain: about 0.25 s
on the leaf edit, about 0.5 s on the wide edit.**

### (d) A faster debug back end

- **FastISel or GlobalISel:** clang at -O0 already uses FastISel. Turning it off costs 21% (16
  modules, one thread, 2 rounds: 5.52 to 5.58 s -> 6.72 to 6.75 s); GlobalISel at -O0 costs 30%
  (7.04 to 7.28 s). Nothing faster is available inside LLVM. The Win64 FastISel gap is noted under b2.
- **TPDE** (TU Munich, CGO 2026): a back end for LLVM IR, 8 to 24 times faster than LLVM -O0 on
  SPECint 2017 (geomean 13x), code 10 to 49% larger. ELF only (x86-64, AArch64): no COFF and no
  Windows EH, so not for the game today. It takes the same in-memory LLVM IR that b1 builds, so it
  would become an option for Linux targets once b1 exists.
- **Cranelift** (rustc_codegen_cranelift): 5 to 15% faster than LLVM on typical Rust debug builds
  (the Rust project's 2026 goal), up to 20 to 40% on some crates; it would need its own IR from kppc.
- **Zig's self-hosted x86-64 back end:** 5 to 20 times faster than LLVM for debug builds, generated
  code 1.5 to 3 times slower than LLVM -O0, years of work, and not on Windows.
- **Go's compiler:** fast, with its own ABI and runtime; not applicable.
A non-LLVM back end is the only route to an order of magnitude, at the price of a second code
generator to keep correct.

### (e) Incremental linking and binary patching

The debug relink after one changed object is 0.45 s with link.exe `/INCREMENTAL` and stays the
largest single item of a leaf edit once the back end is small. Two ways below it:
- **Zig:** in-place patching of the output, per declaration, through indirection tables.
- **Live++ and blink:** hot patching of a running process. A changed function's new code is loaded
  and the old entry jumps to it, so there is no relink at all for a body edit.
kppc's compile server already knows which functions changed. A patch writer, an object of only the
changed functions plus a jump table, is a design of its own (thread B with E's IDE side). It would
take the leaf edit's 0.45 s link to about 0.05 s. It is the step after (c).

### (f) Partitioning and load balance

The 128 modules are balanced enough that the back-end phase is the total work over the cores, not
its largest module. The largest-first order is worth 0.2 s (measured by thread A). With b1's pieces,
balance becomes a work-stealing queue's job. Home globals also shrink the outliers: module 21's
bodies are 12.4 MB, the rest being declarations.

## 3. The IR's volume: is 967 MB normal?

kovarex's question: is 967 MB of IR normal for 39 MB of Factorio source (31 MB of .cpp, 9.3 MB of
headers in src, plus the libraries kppc compiles with it)? Each build was measured by `cg\volume.py`:
kppc's 128 modules, and clang's own `-O0` IR for Factorio's C++.

**The comparison** (`cg\cpp\sample.py`, `cg\cpp\ir.cmd`). Factorio's C++ was compiled by clang-cl
19.1.5 at `/Od` to textual IR with the game's defines, include paths and forced includes
(vs2022\factorio.vcxproj; `Common.hpp`, `win_fp_flags.hpp`). Two shapes were measured. One is 40
random .cpp files of src compiled one by one (seed 20261102), 226 KB of source giving 32.7 MB of IR,
145 bytes of IR per byte of .cpp. The other is 3 unity files of 33 consecutive .cpp files each, as the
C++ build's 100 unity files group the game's 3,269 (FASTBuild `UnityNumFiles = 100`): 51.9 MB, about
17 MB per unity file.

| | kppc, game and libraries, 128 modules | clang, game .cpp one by one (scaled) | clang, game as 100 unity files (scaled) |
|---|---:|---:|---:|
| IR | 967 MB | about 4.5 GB | about 1.7 GB |
| instructions | 9.78 M | | about 15.7 M |
| function bodies | 55.6% | 70.5% | 79.8% |
| globals | 24.2% | 8.4% | 5.8% |
| COMDAT lines | 7.4% | 10.4% | 8.2% |
| declarations | 6.9% | 1.6% | 0.6% |
| types | 5.9% | 3.3% | 1.9% |
| global names, local names | 23.6%, 18.8% | 33.3%, 5.2% | 32.3%, 5.9% |
| alloca, load, store of the instructions | 10.6%, 21.5%, 13.6% (45.7%) | 15.5%, 22.7%, 17.3% (55.5%) | 14.1%, 23.2%, 15.4% (52.7%) |
| instructions per function: median, p99 | 17, 376 | 12, 437 | 12, 162 |

So kppc's IR is about half of clang's for the same game built the C++ way, with fewer instructions,
and a smaller share of -O0's alloca, load and store pattern. The C++ estimates cover the game's src
alone (100 unity files and one file compiled on its own, `CurrentVersion.cpp`); the libraries kppc
compiles with the game would add to them. C++ pays for each inline function and template once per translation unit (11,345 repeated
function definitions in the 40 files); kppc defines each function once in the program, apart from
57,531 copies of member-pointer thunks (`pmf.*`, a few lines each) and kppc's six runtime helpers in
every module. kppc emitted 257,250 functions (83,201 function template instances among them, the
front end's count), median 17 instructions.

**What kppc's 967 MB is made of, and what could be cut:**

| part | MB | cut | also faster |
|---|---:|---|---|
| globals: 64,779 names defined 1.57 million times, each in 24 modules on average | 234 | about 224 MB with one home per global | clang −20%, the link −36% (measured above); objects 475 -> 260 MB |
| COMDAT lines (`$x = comdat any`), mostly the repeated globals' | 71 | most, with the globals | parse only |
| global names: quoted readable names (`@"FlatMap<...>::binary_private_lower_bound#...::operator()(...)"`) | 229 | perhaps half, with short hashed symbols and the readable name kept for the crash report's map | clang's parse a little; the link's symbol table and the PDB's names (not measured) |
| local value names (`%v123`, `%self`) | 182 | all, by numbered values; or nothing to cut in memory (b1 discards them) | clang's parse a little |
| types: 37,000 types defined 681,000 times, each in 18 modules | 57 | none in text (each module needs its types); none in memory per context | |
| declarations: 218,000 names declared 720,000 times, each in 3.3 modules | 67 | none (needed) | |
| functions never named anywhere (no call, no address, in no vtable): 8,166 | 7.7 | all; the debug link keeps them (`/OPT:NOREF`) | a little |
| member-pointer thunks' copies | about 17 | one home each, as the globals | a little |

So about 300 MB (31%) comes off with one home per global and its COMDAT lines. That cut speeds clang
and the link and needs no LLVM integration. Names are another 20 to 40% of the text, which matters
only while there is text: in memory (b1) local names cost nothing and global names are needed once.
The function bodies themselves are not out of line with clang's.

## 4. The integration in three steps

kovarex's order. Each step has its measured or prototyped gain, and the next is tried only after it.

**Step 1: stop writing textual IR (bitcode or another binary form).**
- Measured: clang processes on bitcode take 3.56 to 3.65 s against 3.68 to 3.70 s on text, about
  0.1 s of a clean build. For the leaf edit's module it is about 0.03 s (in process: 0.27 -> 0.22 s
  including the parse; through a process less of it shows). Writing the files is 0.11 s today either way.
- Cost: kppc would need its own bitcode writer, LLVM's only binary input. That is a binary,
  abbreviated, value-numbered format, versioned with LLVM; LLVM's own writer is LLVM's library,
  which is step 2.
- Needs nothing from before.
- **Verdict: not worth doing on its own.** Bitcode stays useful for caches and `--emit-llvm`, made by
  step 2's in-memory module.

**Step 2: LLVM integrated into kppc, everything in memory.**
- Measured in process (cgproto) on the same modules: 3.70 -> 3.38 s on text, 3.02 s on bitcode. With
  no format at all, estimated 2.6 to 2.8 s, since the parse is 27 of the 101 thread-seconds.
  **About −1 s on a clean build.** With one home per global (independent, measured 2.56 s in process
  on text), about 1.9 to 2.0 s, −1.7 s against today.
- Leaf edit: its module 0.305 -> about 0.18 s (−0.12 s). Wide edit: one module, the same −0.1 s.
- The link stays link.exe for the debug game (the PDB). In-process lld is for release and other targets.
- Cost: LLVM's libraries in kppc (30 to 40 MB, a pinned LLVM version, a 20 to 40 minute LLVM build per
  version); kppc's emitter writing through the C++ API instead of text (thread B's file, a rewrite of
  its output layer); kppc's codegen work changes little (text formatting is 10 to 15% of it, and
  building LLVM's objects costs about as much).
- Needs nothing from step 1. It makes step 1 unnecessary.

**Step 3: LLVM in kppc's thread model, generating in parallel.**
- b1 with streaming, the near end. Each worker builds its functions into its own context; a piece
  goes to the back end on kppc's pool as soon as it is complete, and is cached by the hash of its
  IR. Estimated **−2.5 to −3 s on a clean build** (the back end on the cores kppc's lock-bound
  codegen leaves idle; what remains is the last pieces, 0.3 to 0.6 s). The leaf edit's back end
  becomes one small piece, 0.18 -> about 0.02 s; the wide edit's about 0.2 to 0.5 s less.
- Cost: piece scheduling and caching in kppc's codegen and driver (thread B's); placement by content
  for determinism. The first prototype must measure how much the back end slows codegen itself
  (memory bandwidth, the allocator).
- Needs step 2's in-memory IR.
- b2, the far end: one LLVM context shared by all threads, an LLVM fork made thread-safe. It adds
  little over b1 with streaming (section 2, b2), at the cost of a fork of LLVM's core.
  **Not recommended**; the upstreamable Win64 FastISel fix is.

## 5. Recommended order, with expected gains

Today: clean debug 17.1 s (quiet machine), leaf edit 1.4 to 1.5 s, wide edit 1.9 s (thread A's
quiet numbers). The gains overlap; each line is against the line before.

| step | owner | clean | leaf edit | wide edit | measured or estimated |
|---|---|---:|---:|---:|---|
| 1. One home per linkonce global (codegen placement) | B | −1.3 s (clang −0.75, link −0.6) | −0.05 s | −0.1 s | measured on the prototype's modules |
| 2. Largest module first (driver) | B | −0.2 s | 0 | 0 | measured by thread A |
| 3. Integration step 2: LLVM in kppc, the IR in memory, still per module; no processes, no parse (step 1, bitcode, skipped) | B with D | −0.7 to −1 s | −0.1 s | −0.2 s | in-process and bitcode runs bound it |
| 4. Integration step 3: streaming pieces on kppc's pool (b1) | B with D | −2.5 to −3 s | 0 | −0.2 s | estimate; the first prototype must measure codegen's slowdown |
| 5. Pieces cached by IR hash (c) | B | 0 | −0.2 s | −0.5 s | from the leaf module's 0.3 s |
| 6. kppc codegen's lock scaling | B | −4 to −6 s | −0.1 s | −0.1 s | 23.6 s of work at 2.65x today |
| 7. Patching instead of relinking (e) | B, E | 0 | −0.4 s | −0.4 s | link 0.45 s -> about 0.05 s |

End state: clean debug about 11 to 12 s after steps 1 to 4, about 6 s after step 6. Leaf edit about
0.6 s after step 7 (front end and server 0.3 s, codegen 0.2 s, back end and patch 0.1 s). Wide edit
about 0.7 s. Step 1 is the cheapest and does not depend on the rest. Steps 3 and 4 are the
integration kovarex asked about. b2 (a fork) is not recommended. Upstreaming the Win64 FastISel
gap is.

Release builds are not covered here: their time is ThinLTO's link (compile-speed.md, "The release
edit"), where b1's in-process back ends and a per-function cache key are a separate study.

## 6. What was built, step by step (thread D, from 2026-11-02)

Each step: paired clean builds of the game against master, interleaved (`cg\paired.ps1`, the game as
run.cmd builds it, `cg\build.cmd`, from C:\factorio-in-kpp), on a quiet machine unless marked; the
disk use of the build's output directory (`cg\disk.ps1`); and the gate. The gate is run_tests and
spec (`ALL PASSED`, exit 0), the game's modules compared with master's, determinism at 1, 8 and 32
threads, and, for a step that changes the IR, the game itself: `--dump-data`, the 45 menu maps and
the unit tests against C++ Debug's CRCs.

**Step 1: codegen's serial start (b9dcb7ba, 0303a2fb, 9aaf2cdb).** The lock was not the cause. A
timeline of the sampling profile showed the first 4.5 s of the 9 s on one thread: the driver defined
the program's startup globals one by one before any function, nearly all of it the evaluator folding
their initializers.
- Now the startup globals are the head of level 0's queue, on all threads. A global's initializer is
  evaluated before codegen's shared lock is taken (it held the lock 6.2 s of codegen). A global being
  evaluated is claimed, and a second user waits for it instead of evaluating it again. The errors the
  early evaluation records are reported when its value is taken (B's review; spec
  language/global_initializer_with_failing_layout_is_error).
- The evaluator reads a const global's value once per evaluation, not at every read: the game's
  `InputActionList::luaNameToEditableActionMapping` rebuilt a 700-entry table per element, 1.6 s of
  the critical path.
- Tried and left out, no gain measured: buffering each emitter's definitions until the end of a level
  (4.92 to 4.97 s without, 4.96 to 5.21 s with); publishing layouts at any lock depth.

| | master | step 1 |
|---|---:|---:|
| codegen at -j 32 | 8.86 to 9.03 s | 4.72 to 4.84 s |
| codegen at 1 / 8 / 32 threads | 23.6 / 9.7 / 8.9 s | 23.1 / 5.7 / 4.9 s |
| clean debug build, wall | 17.0 to 17.1 s | 12.8 to 13.2 s |
| IR, objects | unchanged: the 128 modules byte-identical to master's | |

**Step 2: one home module per global (debug builds).** Each linkonce_odr global of the shared
declarations is defined in one module (a hash of its name) and declared `external` in the modules
that name it. Release builds are unchanged (ThinLTO imports constants), and so are `--debug-info`
builds. Paired against master with step 1, 3 rounds:

| | master | step 2 |
|---|---:|---:|
| IR | 967 MB | 708 MB |
| clang -O0, 128 modules | 3.76 to 4.13 s | 3.10 to 3.19 s |
| link (link.exe, full) | 2.15 s | 1.65 s |
| clean debug build, wall | 12.9 to 15.1 s | 12.0 to 12.2 s |

Equivalence (`cg\homecheck.py`): every module's text apart from its global, comdat and type lines
is the same as master's; every linkonce_odr global is defined at least once and no more often than
master defines it, with master's line; every external declaration names a known global. The game:
`--dump-data` checksums 7/7 equal; the 45 menu maps' checksums all equal; the unit tests against C++
Debug's CRCs 9,501/9,501 with 0 mismatches (1,115.7 s).

**Disk use of a build** (the output directory of `cg\build.cmd`):

| | total | .ll | .o | exe | PDB | .ilk | ThinLTO cache |
|---|---:|---:|---:|---:|---:|---:|---:|
| debug, master (step 1) | 2,095 MB | 924 MB | 510 MB | 101 MB | 116 MB | 444 MB | |
| debug, step 2 | 1,457 MB | 675 MB | 313 MB | 101 MB | 116 MB | 252 MB | |
| debug, step 3 (LLVM in kppc) | 785 MB | 0 | 315 MB | 102 MB | 116 MB | 252 MB | |
| release (unchanged by steps 1 to 3) | 1,674 MB | 775 MB | 195 MB (bitcode) | 62 MB | 441 MB | | 201 MB |

**Step 3: LLVM's back end inside kppc (integration step 2, section 4).** A kppc built with
`KPP_LLVM_DIR` links the official clang+llvm-19.1.7 release's static libraries (`compiler\llvm.ps1`
fetches them: the archive's SHA-256 checked, 256 MB of it unpacked). A debug build's module goes from
its text in memory to its object on kppc's own threads: the same -O0 pipeline and target options as
clang's cc1 (`UseInitArray`, without which the constructor list came out reversed; the `.file` name
clang writes), no IR file written, no clang started. A release build (ThinLTO), Linux,
`--clang-flags` and `--debug-info` stay clang's; `KPP_BACKEND=clang` gives clang's path for comparing.
Thread B's review added: each module's LLVM errors fail that module with LLVM's message (a handler on
its context), a fatal error names the module before LLVM ends the process, the verifier checks
every module after the parse, and the triple and data layout are compared with LLVM's, never
replaced. run_tests' "LLVM back end" (tests\backend.ps1) breaks module 0 for the verifier and for
LLVM's diagnostics (an instruction the assembler rejects), on windows and on windows-gnu.
- windows-gnu, the self-contained release's default, is compiled in kppc too, with llvm-mingw's cc1
  settings where they differ from Visual Studio's clang (`clang -###`): `-fno-use-init-array` (the
  `.ctors` section, which LLVM writes reversed for MinGW-w64's runtime), `-exception-model=seh`, and
  no `-mincremental-linker-compatible`; llvm-mingw's clang is the same LLVM 19.1.7. Equivalence
  (`cg\gnu-ab.ps1`, `cg\objtree.py`): built through llvm-mingw's clang and in kppc, every object is
  the same apart from the COFF timestamp: the 45 end-to-end programs' 3,017, kppsite's 85 and the
  spec suite's 61,056 (`spec.cmd --target windows-gnu`, 1,005 pass and 9 pending both ways).
- Equivalence: the 128 objects are clang's byte for byte, apart from the COFF timestamp
  (`cg\objcompare.py`), and the same at 1, 8 and 32 threads. The game: `--dump-data` 7/7 equal, the 45
  menu maps' checksums equal, the unit tests 9,501/9,501 with 0 CRC mismatches (1,254.4 s).
- Time, quiet machine, before the verifier: the back end 3.07 to 3.11 s (clang per module) against
  2.86 to 2.92 s (in kppc), the clean build's wall about 10.0 -> 9.8 s. The verifier is 5.0% of the
  back end's work (3.8 of 76.1 thread-seconds, measured by thread CPU time on a loaded machine: the
  parse 20.7, the verifier 3.8, the -O0 pipeline 0.9, code generation 50.7), about 0.12 s at 32
  threads, so most of the gain goes to checking what clang did not: clang's per-module path does not
  verify its input (a release clang passes `-disable-llvm-verifier`), and an IR module the verifier
  rejects compiles there with exit 0.
- Cost: kppc.exe 8.0 -> 41.6 MB; kppc's own clean build 32 -> 36 s.
- Disk: no `.ll` files, 1,466 -> 785 MB for the debug build (the table above).

## Reproduce

All in `C:\kpp-sandbox\scratch\templates\cg` (thread D's scratch, not committed):

| file | what it does |
|---|---|
| `build.cmd OUT [kppc options]` | the game's debug build as run.cmd builds it, from C:\factorio-in-kpp |
| `proto\cgproto.cpp`, `proto\build.cmd` | LLVM through VS's `LLVM-C.dll`; modes `emit`, `spawn`, `tobc`, `home`, `write`; `CGPROTO_PASSES`, `CGPROTO_HOMES`; `-- llvm options` |
| `measure.ps1 -Rounds N -Threads T -Variants spawn:out,inproc:bc,...` | interleaved runs over the module sets `out`, `bc`, `ident`, `home` |
| `homes.py` | each linkonce global's home module (homes.txt) |
| `volume.py DIR` | what a build's IR is made of (section 3) |
| `cpp\ir.cmd`, `cpp\sample.py` | Factorio's C++ to -O0 IR by clang-cl, the sample and the unity files |
| `link.ps1`, `linkab.ps1` | the full and incremental link; the two object sets linked interleaved |
| `sampler\sampler.cpp`, `kppc-pdb.cmd` | the sampling profiler, and kppc with a PDB |
| `paired.ps1`, `determinism.ps1`, `disk.ps1`, `homecheck.py` | paired builds against master; 1/8/32 threads; disk use; step 2's equivalence |
| `timeline.py`, `inflate.py`, `lockprobe.py`, `lockrank.py` | a profile's timeline; thread time at 1 and 32 threads; a temporary lock probe (never committed) |
| `objcompare.py A B`, `objtree.py A B`, `backend-ab.ps1`, `backend-four.ps1`, `gnu-ab.ps1`, `backendprobe.py` | two builds' objects compared (the COFF timestamp masked), flat or by tree; the back end in kppc against clang, interleaved; windows-gnu built both ways (end-to-end, kppsite, spec); a temporary probe of each module's CPU time per back-end phase (never committed) |
| `game.cmd`, `bench-menu.py` | a build of the game run from a scratch game directory (data by junction); the 45 menu maps |
| `prof-codegen.txt(.stacks)` | the codegen profile |
| `tp1.err` | `-time-passes` over all modules |

## Sources

- TPDE: [the LLVM Discourse announcement](https://discourse.llvm.org/t/tpde-llvm-10-20x-faster-llvm-o0-back-end/86664),
  [the paper](https://arxiv.org/abs/2505.22610), [Phoronix](https://www.phoronix.com/news/TPDE-Faster-Compile-Than-LLVM)
  (ELF only).
- LLVM's back-end speed at -O0, 18 to 20: [Engelke, EuroLLVM 2025](https://www.llvm.org/devmtg/2025-04/slides/technical_talk/engelke_faster.pdf).
- Cranelift for Rust: [the 2026 project goal](https://rust-lang.github.io/rust-project-goals/2026/improve-cg_clif-performance.html),
  [LWN](https://lwn.net/Articles/964735).
- Zig: [the x86 back end the default for debug](https://ziggit.dev/t/self-hosted-x86-backend-is-now-default-in-debug-mode/10447),
  [Mitchell Hashimoto](https://mitchellh.com/writing/zig-builds-getting-faster),
  [Zig's new relationship with LLVM](https://kristoff.it/blog/zig-new-relationship-llvm/),
  [in-place binary patching](https://lists.puremagic.com/pipermail/digitalmars-d/2020-September/309242.html).
- Rust's partitioning: [rustc_monomorphize::partitioning](https://doc.rust-lang.org/nightly/nightly-rustc/rustc_monomorphize/partitioning/index.html).
- LLVM threading: [function-based parallel back end (2013)](https://lists.llvm.org/pipermail/llvm-dev/2013-July/063825.html),
  [ConstantData without use-lists (2016)](https://discourse.llvm.org/t/rfc-constantdata-should-not-have-use-lists/42606),
  [MLIRContext](https://llvm.googlesource.com/llvm-project.git/+/refs/heads/main/mlir/lib/IR/MLIRContext.cpp).
- Textual IR parsing: [Timing the IRReader (2013)](https://lists.llvm.org/pipermail/llvm-dev/2013-March/060064.html).
- Hot patching: [blink](https://github.com/crosire/blink).
