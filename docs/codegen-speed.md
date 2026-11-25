# Faster code generation through tighter LLVM integration

**Status: a proposal for kovarex (thread D, 2026-11-02). Nothing in it is decided.** It answers
kovarex's question "Since these tools are opensource, can't we figure out a tighter integration of
clang and our compiler for better performance?" with measurements on the converted game and throwaway
prototypes in thread D's scratch (`C:\kpp-sandbox\scratch\templates\cg`). Nothing in compiler/ was
changed for it; codegen.cpp and the driver are thread B's. kovarex picked the direction: option (b),
LLVM inside kpp, as b1 (LLVM as a library, our own scheduling) and b2 (LLVM changed to fit kpp).

## The answer in brief

- **Today (2026-11-02, section 7): kovarex's clean build, `kpp factorio debug`, was 17.5 s here**
  at master 44d0f0fa (16.8–17.0 s on his machine). It builds with debug info, which steps 1 and 3
  below (one home per global, LLVM in kpp) did not reach: the back end was 5.4 s through clang, the
  link 2.4 s, codegen 4.9 s. Both reach it now (item 3 of section 7's ranking): on master's own
  15.7–16.2 s it takes 13.2–13.6 s. Section 7 ranks the next steps.
- **When this was written, LLVM was 6 s of a 17 s clean debug build (kpp direct, no debug info); kpp's own codegen was 9 s.** The 9 s is not text
  formatting. kpp's codegen is 23.6 s of work that runs only 2.65 times faster on 32 threads, and
  60% of its samples are threads waiting on its shared locks. Building IR in memory instead of as
  text would not shorten it much.
- **A third of clang's time is the shape of our IR, not clang.** Each of the 128 modules defines its
  own copy of about 11,500 `linkonce_odr` constants, plus whatever constants its own functions use.
  Defining each constant once cuts clang's wall time by 20% (3.70 -> 2.95 s), the objects in half
  (475 -> 260 MB), and the full link by 36% (1.67 -> 1.07 s). It needs no LLVM integration and is
  the cheapest step.
- **967 MB of IR is not abnormal for Factorio** (section 3). clang's own -O0 IR for the game's C++
  alone is about 1.7 GB as the 100 unity files the C++ build compiles, and 4.5 GB as 3,269 separate
  files. kpp emits fewer instructions for the game and its libraries together (9.8 million against
  about 15.7 million for the game alone). What is ours to cut is duplication: globals repeated per
  module are 24% of kpp's IR against 6% of clang's unity IR.
- **b1 (LLVM in-process) is worth about 1 to 1.5 s by itself:** no text written and parsed again
  (parsing is 20% of LLVM's work) and no processes. Its larger gain is **streaming**: LLVM's back end
  can then run on the cores kpp's codegen leaves idle, which hides most of the 3 to 4 s of clang.
- **b2 (a kpp-owned, thread-safe LLVM)** adds little over b1 with streaming. What makes one LLVMContext
  single-threaded (shared constants' use-lists, the uniquing tables, one object writer per module)
  is in LLVM's core. Changing it is years of work, LLVM rejected it before, and every LLVM release
  would have to be merged into it. The middle way, stock LLVM driven by function from kpp's own
  scheduler (one context per worker), gets nearly all of b2's benefit.
- **Expected:** clean debug build 17 s -> about 11 to 12 s with b1, home globals and streaming. That
  leaves kpp's codegen as the floor, and its lock scaling is the next lever, thread B's. Leaf edit
  1.4 s -> about 1.1 s, the link then the largest part. Wide edit 1.9 s -> about 1.1 s with
  per-function caching. The order of steps is at the end.

## 1. Where a clean debug build's codegen time goes

**Method.** The game as run.cmd builds it (incremental link, `/DEBUG` names), from C:\factorio-in-kpp
441967bc (the conversion of C:\Factorio 4c178a5b915 by kpp a5839f47), kpp master f9dbfa24..975355ac,
128 modules, -j 32. `cg\build.cmd OUT [kpp options]` builds into OUT. Wall times are from a quiet
machine (CPU load 0 to 4% before each run) unless marked; rounds are interleaved. The back end
was measured by `cg\proto\cgproto.exe`, a prototype that drives LLVM through the `LLVM-C.dll` Visual
Studio ships (LLVM 19.1.5, the same LLVM as kpp's clang, no headers needed), and by its `spawn`
mode, which runs kpp's own clang command per module through `system()` on N threads as kpp does.
`cg\measure.ps1` interleaves the variants. kpp's codegen was profiled with `cg\sampler\sampler.exe`:
it suspends each busy thread every millisecond, walks its stack with dbghelp, and splits samples by
kpp's report lines. The profiled kpp was the same Release code built with a PDB in cg\kpp-pdb.

**The build, phase by phase** (`cg\build.cmd`, three clean builds at `--codegen-threads` 1, 8 and 32):

| phase | wall at -j 32 | notes |
|---|---:|---|
| read, lex, imports, parse, index, resolve | 1.5 s | resolve 0.75 s |
| kpp codegen (IR emission and module assembly) | 8.9 s | 23.6 s at `--codegen-threads 1`, 9.7 s at 8 |
| writing the 128 `.ll` files (967 MB) | 0.11 s | inside kpp's clang phase; `cgproto write 32` |
| clang -O0, 128 processes | 3.9 s | 3.68 to 3.70 s alone (`spawn:out`, below) |
| link (link.exe, `/INCREMENTAL`, PDB) | 2.1 s | a relink after one object changed: 0.45 s |
| **wall** | **17.1 s** | thread A measured 18.0 to 19.6 s the same day (compile-speed.md) |

**kpp's codegen: lock waits, the heap and the evaluator, not text.** Of 1,778 samples in the
codegen window, 1,061 (60%) are threads blocked in a lock. The takers are `addDefinition` (223),
`structFor` layouts (about 200 across call sites), the shared definitions' destruction (62),
`globalFor` (46) and closures (89). The 717 running samples are:
- the emitter's function bodies (`emitFunction`), 64%;
- the compile-time evaluator folding constants and global initializers (`constOutermost`), 21.5%
  (`constValueOfDecl` alone is the first kpp frame of 13.5%);
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
kpp codegen 0.21 to 0.25 s, the incremental relink 0.44 s; 1.39 to 1.51 s on a quiet machine. The
wide edit emits 457 functions in 1 module; 1.9 s quiet.

## 2. The options

All back-end runs below cover the 128 modules at 32 threads, 3 rounds interleaved, quiet machine;
min to max wall (`cg\measure.ps1 -Rounds 3 -Variants ...`).

| variant | wall | objects | what it shows |
|---|---:|---:|---|
| clang per module on kpp's text (today) | 3.68 to 3.70 s | 475 MB | |
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
text, but the driver, the verifier and the back end dominate, and kpp would need a bitcode writer of
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
It costs one placement rule in kpp's codegen (thread B's), like the home modules instantiations
already have. Incremental builds need it to be stable: a global's home must not move when an
unrelated edit adds a user (hash its name to a module, as instantiations are).

### (b1) LLVM inside kpp, our own scheduling

What changes: kpp links LLVM's libraries (IR, X86 back end, MC, lld) statically. Each codegen worker
owns an `LLVMContext` and builds its functions as IR through the C++ API. A unit, a "piece" of
functions, goes to the back end on the same thread pool as soon as it is complete. The back end
writes each object to memory, and lld links in process. A piece's object is cached by a hash of its
IR, so an edit compiles only the pieces whose IR changed.

Measured or estimated gain on a clean build:
- **No text written or parsed.** The parse is 20% of LLVM's work (11.2 of 56.8 s on one thread); in
  process at 32 threads, bitcode already saves 0.35 s, and no format at all about 0.8 s (estimated:
  the parse is 27 of the 101 thread-seconds). kpp stops
  formatting text (2.5 to 3.5 s of its 23.6 s of work) but builds LLVM objects instead. That costs
  memory allocations per instruction and uniquing per type and constant: probably similar work, and
  Zig's contributors name IRBuilder's overhead as a reason to emit bitcode instead. **Net: about 0.5 to 1 s.**
- **No processes:** about 0.05 s per module, 0.2 s of wall at 128 modules.
- **Streaming, the larger part.** kpp's codegen leaves about 60% of the cores idle for 8.9 s (its
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
- **kpp's size:** VS's `LLVM-C.dll` with every target is 52 MB. x86-64 only, statically linked, is
  roughly 30 to 40 MB more in kpp.exe (estimate). The self-contained release (design.md 17) ships
  clang (87 MB) and lld (53 MB) today. With LLVM inside kpp, the release still needs clang for the C
  import's preprocessor, until libclang is in-process too.
- **LLVM version coupling:** the C++ API changes every release (opaque pointers replaced typed pointers in
  LLVM 15 to 17, the legacy pass manager is going, debug-info records replaced intrinsics in 19). kpp would build against one pinned LLVM version and
  move with it twice a year. Textual IR is only a little more forgiving.
- **Building it:** LLVM's libraries are a CMake build of 20 to 40 minutes on this machine for
  X86, MC and lld, once per LLVM version. CI would cache them or download the release's static libraries.
- **Determinism:** pieces must be defined by content (a function goes to piece hash(name) % P), never
  by which thread finished first. The back end is deterministic per module. The object list for the
  link is sorted. Then the output is byte-identical at any thread count, as today.
- **Debug info and EH:** both are LLVM's, unchanged. kpp's codegen must emit through the API what it
  emits as text today: a rewrite of the emitter's output layer (thread B's file), large but mechanical.
  The text path can stay as `--emit-llvm` through `Module::print`.

The prototype that remains to be built is the streaming one: kpp's emitter building IR through the
API, on a fork of codegen.cpp. That needs thread B's agreement, which the coordinator arranges. Until
then, the numbers above are the back end measured on kpp's real modules.

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
- **kpp's size:** as b1.
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
For kpp, pieces of 50 to 200 functions by hash of name keep the per-piece declarations' overhead
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
  (the Rust project's 2026 goal), up to 20 to 40% on some crates; it would need its own IR from kpp.
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
kpp's compile server already knows which functions changed. A patch writer, an object of only the
changed functions plus a jump table, is a design of its own (thread B with E's IDE side). It would
take the leaf edit's 0.45 s link to about 0.05 s. It is the step after (c).

### (f) Partitioning and load balance

The 128 modules are balanced enough that the back-end phase is the total work over the cores, not
its largest module. The largest-first order is worth 0.2 s (measured by thread A). With b1's pieces,
balance becomes a work-stealing queue's job. Home globals also shrink the outliers: module 21's
bodies are 12.4 MB, the rest being declarations.

## 3. The IR's volume: is 967 MB normal?

kovarex's question: is 967 MB of IR normal for 39 MB of Factorio source (31 MB of .cpp, 9.3 MB of
headers in src, plus the libraries kpp compiles with it)? Each build was measured by `cg\volume.py`:
kpp's 128 modules, and clang's own `-O0` IR for Factorio's C++.

**The comparison** (`cg\cpp\sample.py`, `cg\cpp\ir.cmd`). Factorio's C++ was compiled by clang-cl
19.1.5 at `/Od` to textual IR with the game's defines, include paths and forced includes
(vs2022\factorio.vcxproj; `Common.hpp`, `win_fp_flags.hpp`). Two shapes were measured. One is 40
random .cpp files of src compiled one by one (seed 20261102), 226 KB of source giving 32.7 MB of IR,
145 bytes of IR per byte of .cpp. The other is 3 unity files of 33 consecutive .cpp files each, as the
C++ build's 100 unity files group the game's 3,269 (FASTBuild `UnityNumFiles = 100`): 51.9 MB, about
17 MB per unity file.

| | kpp, game and libraries, 128 modules | clang, game .cpp one by one (scaled) | clang, game as 100 unity files (scaled) |
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

So kpp's IR is about half of clang's for the same game built the C++ way, with fewer instructions,
and a smaller share of -O0's alloca, load and store pattern. The C++ estimates cover the game's src
alone (100 unity files and one file compiled on its own, `CurrentVersion.cpp`); the libraries kpp
compiles with the game would add to them. C++ pays for each inline function and template once per translation unit (11,345 repeated
function definitions in the 40 files); kpp defines each function once in the program, apart from
57,531 copies of member-pointer thunks (`pmf.*`, a few lines each) and kpp's six runtime helpers in
every module. kpp emitted 257,250 functions (83,201 function template instances among them, the
front end's count), median 17 instructions.

**What kpp's 967 MB is made of, and what could be cut:**

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
- Cost: kpp would need its own bitcode writer, LLVM's only binary input. That is a binary,
  abbreviated, value-numbered format, versioned with LLVM; LLVM's own writer is LLVM's library,
  which is step 2.
- Needs nothing from before.
- **Verdict: not worth doing on its own.** Bitcode stays useful for caches and `--emit-llvm`, made by
  step 2's in-memory module.

**Step 2: LLVM integrated into kpp, everything in memory.**
- Measured in process (cgproto) on the same modules: 3.70 -> 3.38 s on text, 3.02 s on bitcode. With
  no format at all, estimated 2.6 to 2.8 s, since the parse is 27 of the 101 thread-seconds.
  **About −1 s on a clean build.** With one home per global (independent, measured 2.56 s in process
  on text), about 1.9 to 2.0 s, −1.7 s against today.
- Leaf edit: its module 0.305 -> about 0.18 s (−0.12 s). Wide edit: one module, the same −0.1 s.
- The link stays link.exe for the debug game (the PDB). In-process lld is for release and other targets.
- Cost: LLVM's libraries in kpp (30 to 40 MB, a pinned LLVM version, a 20 to 40 minute LLVM build per
  version); kpp's emitter writing through the C++ API instead of text (thread B's file, a rewrite of
  its output layer); kpp's codegen work changes little (text formatting is 10 to 15% of it, and
  building LLVM's objects costs about as much).
- Needs nothing from step 1. It makes step 1 unnecessary.

**Step 3: LLVM in kpp's thread model, generating in parallel.**
- b1 with streaming, the near end. Each worker builds its functions into its own context; a piece
  goes to the back end on kpp's pool as soon as it is complete, and is cached by the hash of its
  IR. Estimated **−2.5 to −3 s on a clean build** (the back end on the cores kpp's lock-bound
  codegen leaves idle; what remains is the last pieces, 0.3 to 0.6 s). The leaf edit's back end
  becomes one small piece, 0.18 -> about 0.02 s; the wide edit's about 0.2 to 0.5 s less.
- Cost: piece scheduling and caching in kpp's codegen and driver (thread B's); placement by content
  for determinism. The first prototype must measure how much the back end slows codegen itself
  (memory bandwidth, the allocator).
- Needs step 2's in-memory IR.
- b2, the far end: one LLVM context shared by all threads, an LLVM fork made thread-safe. It adds
  little over b1 with streaming (section 2, b2), at the cost of a fork of LLVM's core.
  **Not recommended**; the upstreamable Win64 FastISel fix is.

## 5. Recommended order, with expected gains, and where each step is

**Stage 1's target (kovarex, 2026-11-07): 5x C++ on the clean and the whole builds.** In his words:
"having 5X on clean build would be great, if it can be done in reasonable time, the current around 2.5
is already convincing, but lets try for 5x for some time". So about 8.4 s for the clean build (C++ 42 s,
a precompiled-header change), 9.2 s for the whole factorio.exe (46 s) and 12 s for the whole
factorio-test.exe (60 s); the edits stay 10x or better (2.5 s and 5.5 s). 10x everywhere stays the
long-term goal; release builds (their ThinLTO link) are a separate study. The target is worth real time,
not a gate on stage 1. The whole builds stay cold builds: no object cache for the C libraries (kovarex,
2026-11-07: incremental builds never rebuild them, and a full rebuild is mainly for measurements). Where it stands, kovarex's `kpp measure` (the readme's method; for 0.2.937 the
machine's load at the time is not known):

| | C++ | kpp 0.2.171 (readme) | kpp 0.2.937 (2026-11-07) | target |
|---|---:|---:|---:|---:|
| Edit of one function | 25 s | 1.4 s | 1.8 s (undone 1.7 s) | 2.5 s (10x) |
| Edit of an inline function used across the game | 55 s | 2.0 s | 2.5 s (undone 2.0 s) | 5.5 s (10x) |
| Clean build, libraries already built | 42 s | 10.7 s | 15.0 s | 8.4 s (5x) |
| Whole build of factorio.exe, libraries included | 46 s | 19.5 s | 23.4 s | 9.2 s (5x) |
| Whole build of factorio-test.exe, libraries included | 60 s | 23.9 s | 24.9 s | 12 s (5x) |

On the development machine, B's quiet window of 2026-11-07 measured that day's debug information work: 22:01
to 22:12, `kpp measure --rounds 1` with each kpp in turn, two interleaved rounds, factorio-in-kpp d97dc7389,
the machine 2 to 10% busy before each build. The columns are master before cc3b286c and 8f48fb95 (one round:
the slot ended), master at 8f48fb95, and that with b3d8abf5 and d534c3b6 (and largest first, which measured
nothing: step 2's row). A's 8e481d22 (the instance check on every thread) is in none of them.

| | before | 8f48fb95 | b3d8abf5, d534c3b6 |
|---|---:|---:|---:|
| Clean build, libraries already built | 16.1 s | 14.0 to 14.5 s | 12.2 s |
| Whole build of factorio.exe, libraries included | 20.0 s | 18.0 to 18.6 s | 16.4 to 16.5 s |
| Whole build of factorio-test.exe, libraries included | 24.6 s | 21.6 to 23.8 s | 20.1 to 20.2 s |

The edits did not move: 2.3 to 2.5 s and 3.1 to 3.5 s on this machine, in every column.

B's next window, 2026-11-08 00:10 to 00:15, split the clean build by phase and timed the whole builds on the
current game: `kpp TARGET debug --verbose` with `KPP_STEP_TIMES=1` (B's whole.py), the same three kppcs in
turn, two rounds, the machine 0 to 2% busy before each build. The clean builds, on d97dc7389 (medians, the
rounds within 0.7 s):

| | before | 8f48fb95 | b3d8abf5, d534c3b6 |
|---|---:|---:|---:|
| Wall | 15.9 s | 14.6 s | 12.4 s |
| kpp | 13.6 s | 12.5 s | 10.8 s |
| of it codegen, 120 of the 128 program pieces compiled during it | 9.0 s | 8.0 s | 6.2 s |
| of it the back end after codegen | 2.5 s | 2.5 s | 2.6 s |
| of it the rest (read, lex, imports, parse, index, resolve, check) | 2.0 s | 2.0 s | 2.0 s |
| Link | 1.35 s | 1.2 s | 1.2 s |
| Before the build (build.kpp and its libraries described) | 0.9 s | 0.9 s | 0.4 s |
| The build's CPU | 285 s | 309 s | 257 s |

The cuts were in codegen (9.0 -> 6.2 s; its IR 1.67 -> 1.56 GB); the back end after it stayed at 2.5 s. The
first two kppcs described build.kpp again before each build (0.5 s of their walls), the third found its
description kept, so kpp's own time is the comparison: 13.6 -> 10.8 s. The whole builds, on factorio-in-kpp
5176c864e, whose SDL and curl have precompiled headers (d0ca6988), one value per round:

| | before | b3d8abf5, d534c3b6 |
|---|---:|---:|
| Whole build of factorio.exe, libraries included | 18.3, 18.3 s | 14.8, 15.0 s |
| Whole build of factorio-test.exe, libraries included | 21.9, 21.7 s | 18.7, 18.5 s |

In a whole build kpp takes 13.1 to 13.3 s instead of 10.8 s: the C libraries, 6 to 7 s beside it and about
100 s of CPU, slow it by 2.4 s. So against the targets, 12.4 / 14.9 / 18.6 s: codegen 6.2 s, the back end
after it 2.6 s, the front end 2.0 s, the link 1.2 s, and in the whole builds the C libraries' 2.4 s.

E's window of 2026-11-08 (02:35 to 02:42) scheduled the C library compiles: B's whole.py (each build
after three one-second samples under 10% load), `kpp factorio debug --verbose` on factorio-in-kpp 1aaa71668
(kpp 696d72af plus the change), the variants of one kpp in turn, four rounds of whole builds, two of clean:

| | whole build, wall | kpp's part | job CPU | clean build, wall |
|---|---|---|---|---|
| as before: the compiles at the normal priority, one per hardware thread | 16.75, 15.42, 15.70 (20.38: 522 s of CPU, disturbed) | 14.07-14.93 s | 375-380 s | 12.95, 12.95 |
| below normal throughout (a job object's priority class) | 15.05, 14.82, 14.96, 14.97 | 13.43-13.67 s | 373-375 s | |
| below normal until kpp is done, then normal (built) | 15.01, 14.90, 14.94, 15.15 | 13.49-13.78 s | 374-378 s | 12.90, 13.09 |
| 8 at once | 15.80, 16.32, 16.59, 16.12 | 13.00-13.90 s | 358-370 s | |

The whole build is 0.7 s faster (15.70 -> 14.97 s, the medians), and steadier; the C libraries now cost kpp
0.6 to 0.9 s instead of 2.4 s. Below normal throughout was no faster on a quiet machine and starved the
compiles under another load (a smoke round on the busy machine waited 38.8 s for them after kpp was done),
so the job is raised to the normal class once kpp ends and the link waits for the libraries alone. Eight at
once left the link waiting 0.8 to 2.2 s for them. A clean build compiles no C library: unchanged. Windows
only (host::PriorityGroup); KPP_CLIB_PRIORITY=normal builds as before.

B's window of 2026-11-08 (03:20 to 03:22) compared master with A's 8e481d22 (53f9b819, LLVM inside) and the
third column's compiler above, on the clean build of factorio-in-kpp 5176c864e: `kpp factorio debug --verbose`
with `KPP_STEP_TIMES=1`, each in turn, two rounds, the machine 2 to 9% busy before each build (medians):

| | b3d8abf5, d534c3b6 | master 53f9b819 |
|---|---:|---:|
| Wall | 12.6 s | 14.0 s |
| kpp | 11.0 s | 11.8 s |
| of it codegen | 6.3 s | 6.6 s |
| of it the back end after codegen | 2.7 s | 2.8 s |
| of it the rest | 2.0 s | 2.4 s |
| Link | 1.25 s | 1.45 s |
| Before the build | 0.4 s | 0.8 s (0.4 s of it build.kpp described again: the window alternated compilers) |

Master is about 1.0 s slower, for more work on the same game: 40% more function template instantiations
(87,117 -> 122,132), 28% more class ones (19,521 -> 25,057), the instantiations' CPU 18.8 -> 24.7 s, 15% more
distinct types, and in codegen 4.5% more functions and IR (1.56 -> 1.63 GB) and 17% more structs. 8e481d22's
gain does not show beside that. Thread D's sampler on master's compile alone (the command its project driver
runs, from its first line to the back end's last): the back end after codegen is 97% LLVM, a third of it
parsing the IR text (13% its debug information's metadata), half the machine passes, 15% writing the object.
Codegen's samples are 30% blocked, mostly on a global another thread builds (`globalFor` under
`emitStartupGlobal`, 6%) and a mutex in the interpreter's `enumValueRange` (4.5%); of the rest the emitter is
55%, LLVM's streamed pieces 36% (28% of them parsing the text), and at the leaf the heap and memory copies 7.5%
each. The front end's leaf is TypeTable::intern (11%) and malloc (9%). So, largest first: the instantiation
growth, the IR in memory (a third of the back end after codegen), codegen's waits. The growth, bisected by
the counts (deterministic, the front end alone on the same tree): K's 745dc03a, the Vector's exception-safety
guards for every element type, 34,532 of the 35,015 function instantiations added and all 5,536 class ones;
K is making them conditional on the element's nothrow construction.

Codegen's waits, timed in B's window of 2026-11-08 (10:36 to 10:41): each column adds one commit to the one
before it, every compiler with LLVM inside, on the same tree with the same command, three rounds in turned
order, the machine 4 to 10% busy before each build (medians; each round's codegen in brackets; the wall
without build.kpp's description, 0.53 s, which the last column's builds found cached and the others did again):

| | master 5e550d72 | and 77dc0b11 (the startup globals on a quarter of the threads) | and 2293858f (the emitted functions in 64 shards) | and D's 07f330fb (the interpreter's caches per thread) |
|---|---:|---:|---:|---:|
| Wall | 14.09 s | 13.81 s | 13.88 s | 12.78 s |
| kpp | 12.19 s | 12.03 s | 12.07 s | 11.02 s |
| of it codegen | 6.84 s (6.66, 6.88, 6.84) | 6.62 s (6.57, 6.62, 13.04) | 6.70 s (6.70, 6.67, 7.05) | 5.69 s (5.69, 5.75, 5.38) |
| of it the back end after codegen | 2.97 s | 3.04 s | 3.00 s | 2.94 s |

D's commit takes 1.0 s off codegen and 0.1 s off the front end: the interpreter's `enumValueRange` and
`declaredUnderlying` (under `constConverted`) answer from a thread's own copy of their caches, where every
thread took one mutex (D's sampler: 6.75% and 8.19% of codegen's samples blocked there before, 0.36% and
0.72% after). The startup globals' schedule and the shards measure nothing beyond master's own spread (6.66
to 6.88 s), and one round of the schedule took 13.04 s in codegen, unexplained (the six other builds with it
inside were normal); neither is on master on these numbers. The schedule's stacks show why (B's sampled
compiles of 2026-11-08, 11:22 to 11:37: ten each with and without it, on master with D's commit, compilers with
PDBs, the machine 75 to 100% busy, so no timing): it moves the convoy at codegen's start instead of removing
it. In codegen's first second the threads waiting in `globalFor` under `emitStartupGlobal` fall from 27.8% of
the samples to 4.1%, and those waiting in `needsDestruction` rise from 3.6% to 16.4%: every one of them waits
for the shared lock, under which each emitter computes its first answers while the shared caches are empty.
Waiting in all of codegen: 39% without it, 41% with it. Both are dropped. What would remove the convoy is the
first answers computed outside the shared lock and published once, as D's commit does for the interpreter.

That lever, B's 2399dea7, does three things, each now without the shared lock:
- needsDestruction is computed by the emitter that asks;
- a defined global's name and thread_local building function are published once;
- string literals are kept in 64 shards.

B timed it in a window on 2026-11-08 (17:39 to 17:42):
- **Compilers:** master ba6d1906 against the lever on it, both with LLVM inside.
- **Tree:** factorio-in-kpp converted from C:\Factorio 4c178a5b915.
- **Command:** `kpp factorio debug --verbose` with `KPP_STEP_TIMES=1`, five clean builds each in turned order.
- **Load:** the machine 3 to 14% busy before each build, but one at 85%.

The table gives medians, with each round's codegen in brackets. The wall leaves out build.kpp's
description: master's builds did it again (0.54 to 0.76 s each) and the lever's found it cached, as in the
windows above.

| | master ba6d1906 | and the lever |
|---|---:|---:|
| kpp | 10.10 s | 9.90 s |
| of it codegen | 5.33 s (5.35, 5.39, 5.33, 5.29, 5.29) | 5.30 s (5.08, 5.38, 5.10, 5.30, 5.74) |
| of it the back end after codegen | 2.67 s | 2.67 s |
| of it the rest | 2.00 s | 2.03 s |

Codegen does not move beyond the spread, and neither does kpp (9.70 to 10.90 s against 9.90 to 10.20 s).
The waits did move. One sampled compile each with D's sampler, in codegen's first second:
- The waiting samples fall from 48% to 35% (over all of codegen, 23% to 19%).
- The waits the lever targets are gone: in needsDestruction (5.0% of that second's samples), buildThreadLocal
  (2.7%) and the literals (1.3%).
- Those in `globalFor` under `emitStartupGlobal` rise from 16.6% to 21.4%.

The convoy moved onto the startup globals' definitions: two shared-lock acquisitions per new global (the
known-check, then the definition), the fourth part of the lever, not done. Removing three waits did not make
codegen faster, so that part's gain is not counted on.

Lever 2 (the tail after codegen) began with two cuts, timed in B's window of 2026-11-08 (21:22 to 21:27):
- **Early debug classes** (a125a943): the debug classes defined during the levels, by the threads done with a
  level's functions, not by homeDebugClasses at the end.
- **Largest first** (0fb10eff): the back end takes the largest modules first, and level 0's barrier hands the
  program pieces over largest first.

The compilers were master 64848340, that with the early classes, and that with both cuts, each with LLVM
inside. Each made five clean builds of `kpp factorio debug --verbose` with `KPP_STEP_TIMES=1`, in turned
order, on factorio-in-kpp converted from C:\Factorio 4c178a5b915, the machine 0 to 13% busy before each.
Medians:

| | master | and the early classes | and both cuts |
|---|---:|---:|---:|
| kpp | 9.70 s | 9.80 s | 9.80 s |
| of it codegen | 5.17 s (5.11 to 5.19) | 5.25 s (5.24 to 5.29) | 5.29 s (5.25 to 5.29) |
| of it the back end after codegen | 2.56 s | 2.57 s | 2.53 s |

- **The early classes made codegen 0.08 s slower,** in every round, so they are reverted. homeDebugClasses
  already ran on every core (about 23 thread-s). The work moved into level 1, whose 46,319 functions keep every
  core busy. B's level timeline, with its own statistics made cheap, gives levels 6 to 17 about 0.3 s in all, so
  there was little idle time to fill. (A first timeline whose per-level statistics hashed every definition had
  made those levels ten times longer.)
- **Largest first is neutral on a quiet machine** (2.57 to 2.53 s, inside the spread) and stays. In a run under
  load, the 17.2 MB program piece of module 93 (ValueToWidget's generated functions), taken 93rd in module order,
  had ended the build alone.
- **The wall times mislead.** The both-cuts compiler found build.kpp's description cached (0.36 s before the
  build, against 0.89 s for the others), as in the windows above.

The tail itself is the instance pieces' work: 128 pieces, 938 MB of IR, 79 to 92 thread-s after codegen. What
would move it is compiling instance code during codegen, so how many cores codegen leaves idle was measured
next, in B's window of 2026-11-08 (23:03 to 23:04):
- **Setup:** three clean inner compiles of the game with master 37bcd403, kpp's CPU time polled every 50 ms
  beside codegen's level marks.
- **Idle cores:** codegen (6.1 to 6.7 s there) left 66 to 83 of its ~200 core-seconds idle. Of those, 30 to 33
  were in level 0, where no instance exists yet; its first ~0.6 s ran at 2 to 7 cores. Another 19 to 25 were in
  the end phase, ~0.5 s of it at 1 to 2 cores.
- **Reach of the big piece:** instance code could use only the idle time after level 5, about 25 to 35 core-s,
  against 84 to 99 thread-s of instance work after codegen.

So lever 2's big piece would gain about 1 s at best, for instance pieces keyed by depth and a server that
recomputes the depths of the whole program. It is shelved (the coordinator, 2026-11-08); the design and the
measurements are in B-lever2-tail-2026-11-08.md in the coordinator's notes. The two idle stretches the timeline
showed are cut directly instead:
- **An enumeration's enumerators computed once.** The end phase's serial stretch was not the debugger's ~50,000
  class names (a timer gave all of them 50 ms) but one class's debug type: D3D11_MESSAGE, whose member's type
  D3D11_MESSAGE_ID has thousands of enumerators, each written from the one before it in Windows' SDK
  (`D3D11_MESSAGE_ID_X = ( D3D11_MESSAGE_ID_W + 1 )`). The interpreter computed every enumerator it was asked for
  again from the first one, finding each earlier one by a scan of those before it: the type's enumerator list took
  1.57 s on one thread, and the end phase's last ~0.5 s ran on that thread alone. The interpreter now keeps an
  enumeration's values once computed (a thread's own copy, as D's 07f330fb keeps its caches): that type takes
  22 ms, and homeDebugClasses 160 ms.
- **The constant text of numbers made before the shared lock.** Level 0's slow start was the startup globals'
  convoy. The constant texts of Tile's 65,536-entry tables (8.7 MB each) were written under the shared lock,
  100+ ms each, while every emitter's first uses of globals waited. A global without an initializer was also
  evaluated twice: once before the lock and again under it. Now a constant made only of numbers gets its text
  before the lock, and such a global takes the value evaluated there. The tables hold the lock 11 to 12 ms, and
  the startup globals' holds fell from 549–589 ms to 338 ms in all. These hold times come from B's diagnostic
  build on a busy machine.

B timed both cuts together in a window on 2026-11-09 (01:44 to 01:47):
- **Compilers:** master 8ca54606 against both cuts on it, each with LLVM inside.
- **Tree:** factorio-in-kpp converted from C:\Factorio 4c178a5b915 by kpp 8ca54606.
- **Command:** `kpp factorio debug --verbose` with `KPP_STEP_TIMES=1`, five clean builds each in turned order.
- **Load:** the machine 0 to 8% busy before each build, but one at 71%.

The table gives medians, with each compiler's range in brackets:

| | master 8ca54606 | and both cuts |
|---|---:|---:|
| kpp | 9.99 s (9.78 to 10.16) | 9.15 s (9.06 to 9.24) |
| of it codegen | 5.26 s (5.26 to 5.41) | 4.57 s (4.54 to 4.62) |
| of it the back end after codegen | 2.58 s | 2.54 s |
| of it the rest | 2.01 s | 2.02 s |

- **Codegen is 0.69 s faster (13%)** in every round, and kpp 0.84 s. How the gain splits between the two cuts
  was not measured.
- **The wall leaves out build.kpp's description,** which the cuts' compiler found cached: 0.37 s before the
  build, against 0.90 s for master's, as in the windows above.
- **The output is unchanged.** The game's IR and its 256 debug objects are byte-identical with and without the
  cuts.

B's window of 2026-11-09 (13:13 to 13:15) tried one builder per class layout, and it was dropped:
- **What it was.** Every emitter builds a class's layout the first time it needs one and the first copy published
  wins, so at the start of level 0 the threads build the same classes at once. A diagnostic build counted 43,062
  layouts built and 17,133 dropped, 4.6 of 6.0 thread-seconds. The change made an emitter that holds no layout of
  its own wait a moment (2 ms at most) for one another emitter is building: about 26,100 built and 200 to 300
  dropped.
- **Compilers:** master a571b23d against that change on it, each with LLVM inside. **Tree:** factorio-in-kpp
  converted from C:\Factorio 4c178a5b915 by kpp 8ca54606. **Command:** as above, five clean builds each in turned
  order, the machine 0 to 12% busy before each build.
- **Codegen did not move:** 4.30 s (4.26 to 4.47) on master, 4.32 s (4.31 to 4.43) with the change; kpp 8.5 s
  both, the back end after codegen 2.49 and 2.47 s. Those duplicates run on cores with nothing else to do. It
  saved CPU and no time, so nothing of it is on master.
- **Where the clean build is now** (master a571b23d, this window's medians): kpp 8.5 s = codegen 4.30 + the back end
  after it 2.49 + the rest 1.70; the link 1.2 s; the build.kpp description 0.35 to 0.87 s before it.

B's window of 2026-11-10 (04:15 to 04:18) timed the back end's cuts that window 12's profile chose (16370e7b: one
verifier run, LLVM's parser's second one off, and `asm "nop"` made the nop instruction before the asm printer;
af7b66b1: each module's metadata written before what names it):
- **Compilers:** master 3eb819d5 against af7b66b1 (the two cuts, with the local typedef fix 1aee85f6 between
  them), each with LLVM inside. **Tree:** factorio-in-kpp a9f363870 (C:\Factorio 4c178a5b915, converted by kpp
  24a67d03). **Command:** as above, five clean builds each in turned order; the window started after a minute
  under 5% load, and each build started at 0 to 5%.
- **The back end after codegen is 0.29 s faster (2.48 to 2.19 s, 12%) and kpp 0.40 s (8.50 to 8.10 s, 4.7%)**
  in every round: kpp 8.5 to 8.6 s against 8.1 to 8.2 s; codegen 4.29 against 4.25 s; the link 1.2 s both.
  The game's 256 debug objects are byte-identical with and without the cuts.
- **The wall gained 0.89 s (10.60 to 9.71 s),** but about 0.5 s of it is build.kpp's description before the build,
  which a round after the other compiler's makes again (0.87 against 0.35 s): not the cuts.
- **Window 12's samples estimated 5 to 6% of the build's CPU;** 4.7% of kpp's time went, since the back end after
  codegen runs on every core and what it saves shows almost whole.

B's window of 2026-11-10 (20:58 to 21:00, the coordinator holding the other threads) timed the module text's cuts of
B-module-text-plan-2026-11-10.md (6bfcae41: the debug types' and globals' placeholders noted as the definitions are
numbered, not found by four scans of the whole module text; ids written without snprintf; each type looked up once):
- **Compilers:** 6bfcae41 against its parent's three files, both built from 6bfcae41 (the same version in the IR).
  **Tree:** fik, factorio-in-kpp bf68c7278. **Command:** `kpp factorio debug --no-server --verbose`, six rounds each in
  turned order, objects kept (no module compiled: the two compilers' 256 module texts are the same), the machine 1 to
  2.5% busy.
- **Codegen 3.33 to 3.36 s before, 3.28 to 3.32 s with them (medians 3.351 and 3.302 s, 1.5%); kpp 5.1 to 5.2 s with
  both.** appendDebugTypes was 8% of a debug --check's samples (A's profile of 2026-11-09), but module text is made on
  every core at once, and so little of it shows as wall time.

When this plan was written, a clean debug build of the game was 17.1 s, the leaf edit 1.4 to 1.5 s,
and the wide edit 1.9 s. These were thread A's quiet numbers: kpp run directly, without debug info.
The gains overlap; each line is against the line before. The status column is as of 2026-11-02.

| step | owner | clean | leaf edit | wide edit | status |
|---|---|---:|---:|---:|---|
| 1. One home per linkonce global (codegen placement) | D | −1.3 s (clang −0.75, link −0.6) | −0.05 s | −0.1 s | **done**, 0998f157: IR 967 -> 708 MB, clang 3.76–4.13 -> 3.10–3.19 s, link 2.15 -> 1.65 s (section 6). With debug info too since 1a8e88c5 (section 7) |
| 2. Largest module first (driver) | B | −0.2 s | 0 | 0 | measured by thread A (c263459f), **not landed**; measured again with debug information in B's quiet window of 2026-11-07 (`kpp measure`, two rounds each): clean 13.98–14.46 -> 13.99–14.35 s, whole factorio.exe 18.00–18.61 -> 17.91–18.32 s, inside the rounds' spread: **dropped** |
| 3. Integration step 2: LLVM in kpp, the IR in memory, still per module | D | −0.7 to −1 s | −0.1 s | −0.2 s | **done**, 54da91a8, 265df4c2, 0ab4574e (windows-gnu): back end 3.07–3.11 -> 2.86–2.92 s (before the verifier, 5% of its work). With debug info too since 8b5c902c (section 7) |
| 4. Integration step 3: streaming pieces on kpp's pool (b1) | D | −2.5 to −3 s | 0 | −0.2 s | **done**: P1 4d884644, P2 91671241 (section 6): the program pieces compiled during codegen; the game's clean build 13.7 -> 11.3 s (back end after codegen 2.8 -> 1.3 s, 120 of the 128 program pieces during codegen), peak memory unchanged |
| 5. Pieces cached by IR hash (c) | B | 0 | −0.2 s | −0.5 s | not started |
| 6. kpp codegen's lock scaling | D | −4 to −6 s | −0.1 s | −0.1 s | **first round done**, b9dcb7ba, 0303a2fb, 9aaf2cdb: codegen 8.9 -> 5.0 s at -j 32 (its serial start). **Second round** (section 6, steps 5 to 7), each step against its own master: the spin locks 9ce78be2 −0.2 to −0.3 s, the collection between levels 0c2a863d −0.23 to −0.26 s, the reserved maps and the modules finished on the threads (L2) −0.36 to −0.53 s; codegen 4.09 to 4.18 s at ca061cc6, 3.64 to 3.73 s with L2. The lock profile of today's build is in section 7 |
| 7. Patching instead of relinking (e) | B, E | 0 | −0.4 s | −0.4 s | not started |

Together, steps 1, 3 and 6 took the clean build without debug info (kpp direct) from 17.1 s to about
9.8 s. **Steps 1 and 3 did not reach kovarex's own build**, `kpp factorio debug`: projectlib's debug
configuration builds with debug info (`--debug-info`), which kept every global in every module and
the back end on clang (section 7). They reach it since 1a8e88c5 and 8b5c902c: −2.6 to −2.9 s on it.
Step 6 reaches it. kovarex measured the whole builds between kpp
0.2.13 and 0.2.28 on his machine: factorio.exe 21.6 -> 20.6 s, factorio-test.exe 40.2 -> 36.4 s.
Today, profiled end to end through kpp, with the next steps ranked: section 7.

End state of the original plan: a clean build without debug info of about 11 to 12 s after steps 1 to
4, and about 6 s after step 6. A leaf edit of about 0.6 s after step 7 (front end and server 0.3 s,
codegen 0.2 s, back end and patch 0.1 s), and a wide edit of about 0.7 s. b2 (a fork) is not
recommended; upstreaming the Win64 FastISel gap is.

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
| debug, step 3 (LLVM in kpp) | 785 MB | 0 | 315 MB | 102 MB | 116 MB | 252 MB | |
| release (unchanged by steps 1 to 3) | 1,674 MB | 775 MB | 195 MB (bitcode) | 62 MB | 441 MB | | 201 MB |

**Step 3: LLVM's back end inside kpp (integration step 2, section 4).** A kpp built with
`KPP_LLVM_DIR` links the official clang+llvm-19.1.7 release's static libraries (`compiler\llvm.ps1`
fetches them: the archive's SHA-256 checked, 256 MB of it unpacked). A debug build's module goes from
its text in memory to its object on kpp's own threads: the same -O0 pipeline and target options as
clang's cc1 (`UseInitArray`, without which the constructor list came out reversed; the `.file` name
clang writes), no IR file written, no clang started. A release build (ThinLTO), Linux,
`--clang-flags` and `--debug-info` stay clang's; `KPP_BACKEND=clang` gives clang's path for comparing.
Thread B's review added: each module's LLVM errors fail that module with LLVM's message (a handler on
its context), a fatal error names the module before LLVM ends the process, the verifier checks
every module after the parse, and the triple and data layout are compared with LLVM's, never
replaced. run_tests' "LLVM back end" (tests\backend.ps1) breaks module 0 for the verifier and for
LLVM's diagnostics (an instruction the assembler rejects), on windows and on windows-gnu.
- windows-gnu, the self-contained release's default, is compiled in kpp too, with llvm-mingw's cc1
  settings where they differ from Visual Studio's clang (`clang -###`): `-fno-use-init-array` (the
  `.ctors` section, which LLVM writes reversed for MinGW-w64's runtime), `-exception-model=seh`, and
  no `-mincremental-linker-compatible`; llvm-mingw's clang is the same LLVM 19.1.7. Equivalence
  (`cg\gnu-ab.ps1`, `cg\objtree.py`): built through llvm-mingw's clang and in kpp, every object is
  the same apart from the COFF timestamp: the 45 end-to-end programs' 3,017, kppsite's 85 and the
  spec suite's 61,056 (`spec.cmd --target windows-gnu`, 1,005 pass and 9 pending both ways).
- Equivalence: the 128 objects are clang's byte for byte, apart from the COFF timestamp
  (`cg\objcompare.py`), and the same at 1, 8 and 32 threads. The game: `--dump-data` 7/7 equal, the 45
  menu maps' checksums equal, the unit tests 9,501/9,501 with 0 CRC mismatches (1,254.4 s).
- Time, quiet machine, before the verifier: the back end 3.07 to 3.11 s (clang per module) against
  2.86 to 2.92 s (in kpp), the clean build's wall about 10.0 -> 9.8 s. The verifier is 5.0% of the
  back end's work (3.8 of 76.1 thread-seconds, measured by thread CPU time on a loaded machine: the
  parse 20.7, the verifier 3.8, the -O0 pipeline 0.9, code generation 50.7), about 0.12 s at 32
  threads, so most of the gain goes to checking what clang did not: clang's per-module path does not
  verify its input (a release clang passes `-disable-llvm-verifier`), and an IR module the verifier
  rejects compiles there with exit 0.
- Cost: kpp.exe 8.0 -> 41.6 MB; kpp's own clean build 32 -> 36 s.
- Disk: no `.ll` files, 1,466 -> 785 MB for the debug build (the table above).

**Step 4, P1: each module in two pieces (4d884644, hunks 1, 2a and 2b; B reviewed).** A debug build of
several modules (not optimized: an optimized build's back end is ThinLTO's link) writes each module as
two: its program piece, what the functions the program writes own (their definitions, their lambdas'
call operators, catch filters and static locals), and its instance piece, the rest. A program piece is
assembled only from texts that do not change once written, so level 0's barrier and the end give one
text; P2 compiles it at the barrier. KPP_CHECK_BARRIER (run_tests, spec, the game gate) assembles it
at the barrier too and compares.
- What the game showed: a program function can hold a closure that only the lambda's writer, an
  instance of a later level, lays out (FusionGeneratorPrototype::addToDescription's): its program
  piece names a type with no line at the barrier. Such a piece (3 of the game's 128) is late: P2
  compiles it at the end.
- Instance pieces keep only the module's own declarations they reach: with all of them the IR grew
  7.9%, now 2.6% (P0's offline split: 1.0%).
- Paired clean builds of the game (`cg\p1-measure.ps1`, 3 rounds, load 0 to 8%), master a6b62b44
  against the pieces, and the leaf edit through the compile server (`cg\leafedit.ps1`, `kpp factorio
  debug` in the project copy, 3 edits and their undos each):

  | | master (128 modules) | pieces (256) |
  |---|---:|---:|
  | clean build, wall | 13.25 to 16.07 s | 13.25 to 13.96 s |
  | codegen | 5.70 to 5.90 s | 5.69 to 5.91 s |
  | IR | 690.4 MB | 708.6 MB (+2.6%) |
  | back end | 2.90 to 3.37 s | 2.83 to 3.13 s |
  | full link | 1.46 to 3.64 s | 1.47 to 2.06 s |
  | leaf edit, wall | 2.13 to 2.57 s | 2.19 to 2.27 s |
  | its back end | 0.35 to 0.36 s (a module) | 0.17 to 0.18 s (a program piece) |
  | its incremental link | 0.5 s | 0.7 to 0.8 s |

  P1 is neutral, as planned: the pieces cost the clean build nothing measurable, and the leaf edit's
  halved back end goes to the incremental link of 256 objects. The gain is P2's: the program pieces'
  back end during levels 1 to 17.
- The gate: run_tests and spec with and without LLVM in kpp and spec `--debug-info`; the game's 256
  objects identical at 1, 8 and 32 threads with the barrier check on; `--dump-data` 7/7 equal, the 45
  menu maps' checksums equal, the unit tests 9,501/9,501 with 0 CRC mismatches against C++ Debug.

**Step 4, P2: streaming (91671241; B reviewed).** The program pieces are compiled while codegen emits the
instances.
- **The hand-over.** At level 0's barrier, codegen hands the driver each program piece it has whole
  (PieceStream). That happens only when no error came before; a late piece is not handed over.
- **The pool.** The driver's BackEndPool compiles each piece with LLVM's back end in kpp, on
  `options.threads` threads below normal priority while codegen runs (normal since 2026-11-21, below). Once codegen ends, the other
  modules follow on the same threads at normal priority.
- **The check at the end.** The end assembles every program piece again and compares it with what it
  handed over, by size and FNV-1a hash.
  - The stream's object is linked only when the two texts are equal.
  - When they differ, the end's text is compiled: a --keep-going build's declarations of what failed,
    or a program function's lambda emitted at a later level.
  - In a build without errors, a difference is kpp's error, never a silently linked barrier object.
- **Not streamed:** the clang path, a release build and the server's rebuildLLVM.
- **Measured:** paired clean builds of the game (`cg\p2-measure.ps1`, 3 interleaved rounds, load 0 to 3%,
  0 stderr lines on both), master a31c239e against P2:

  | | master (256 objects) | P2 |
  |---|---:|---:|
  | wall | 13.66 to 13.70 s | 11.26 to 11.35 s |
  | codegen | 5.67 to 5.70 s | 4.82 to 4.90 s |
  | back end after codegen | 2.75 to 2.78 s | 1.30 to 1.32 s (120 of the 128 program pieces during codegen) |
  | link | 1.42 to 1.45 s | 1.43 to 1.45 s |
  | peak working set | 1.85 to 1.89 GB | 1.84 to 1.89 GB |

- **What the numbers say.**
  - The back end after codegen is 1.46 s shorter, as P0 estimated.
  - Codegen is 0.85 s shorter too, which P2's own work does not explain: its end assembles every program
    piece again. With streaming off (a build with an error before the barrier), P2's codegen equals
    master's. A guess still to test on a quiet machine: the busy back-end threads keep the cores awake
    between codegen's levels.
- **Measuring needs a build without errors.** An error before level 0's barrier switches streaming off,
  as it should. With bd4ee165's zstd passed as plain sources instead of the scoped library it is,
  5 codegen errors did that in the first rounds.
- **The gate:**
  - run_tests and spec with and without LLVM in kpp, and spec `--debug-info`;
  - the game's 256 objects identical at 1, 8 and 32 threads with the barrier check on, the streamed
    build identical to the barrier-checked one, and P2's objects identical to master's;
  - `--dump-data` 7/7 equal;
  - on the first base (b83dbe94), the 45 menu maps and the unit tests 9,501/9,501 with 0 CRC mismatches
    against C++ Debug.

**Step 4's P2 at normal priority (2026-11-21).** kovarex's build of the game took 18 s where the readme
says 11 to 14. Today's commits were not it: yesterday's kpp (f098f191b) and master (ccea2e331) built
yesterday's game (8cd8320d04) in the same time, and master built yesterday's and today's game
(8cd00415e0) in the same time, 12.4 to 14.0 s on a quiet machine. What made 18 s was a stall:
- **Seen.** In 8 of 62 clean builds on a quiet machine, every codegen thread stopped at the same moment for
  about 5 s (twice that once): in the level after the barrier, in the barrier's own piece assembly, or
  in the back end after codegen. Timing points in codegen (not landed) showed it: level 1's 47,590
  functions took 5.6 s instead of 0.5 s, every thread's user CPU in it was 3.0 s instead of 0.2 s, and 32
  functions took 4.9 to 5.5 s each, all ending together.
- **Why.** The pool's threads ran below normal priority beside codegen's 32 threads at normal priority on
  32 cores. A pool thread preempted while it held a lock that codegen's threads take spinning (LLVM's
  allocator, rpmalloc, which a kpp with LLVM takes as its malloc, spins without sleeping) waits until
  Windows boosts a thread starved for about 4 s: priority inversion.
- **What.** The pool's threads keep normal priority (BackEndPool); setThreadBelowNormal is gone.
- **Measured:** `kpp factorio debug --no-server --verbose`, clean (build\debug removed), 20 interleaved
  pairs of one kpp (ccea2e331 with an environment switch for the priority), each round started at 3 to
  11% load:

  | | below normal (before) | normal (now) |
  |---|---:|---:|
  | total, median | 13.7 s | 14.0 s |
  | total, mean | 14.59 s | 14.07 s |
  | total, worst | 21.6 s | 14.8 s |
  | builds over 15 s | 4 of 20 (15.5, 17.9, 18.4, 21.6 s) | 0 of 20 |
  | codegen, median | 5.80 s | 6.46 s |
  | back end after codegen, median | 2.99 s | 2.86 s |

  The pieces now take cores from codegen while it runs, 0.3 s of the median build; the stalls are gone.

**Typed unions destroyed through helpers (2026-11-23).** kovarex: the whole build of factorio-test took about
22 s where the readme says 15.9 s. The kpp releases of the two days before were not it: 0.2.1670, 0.2.1703 and
0.2.1738 built the same game (wube/kpp-factorio kpp 8cd00415e0) in 22.5 to 25.1 s. Each day's game built with
that day's nightly kpp showed the step: 2.18 GB of IR on 11-14 (f5b0ea31c7, kpp 0.2.1471), 2.68 GB on 11-16
(377c9d4ccd, 0.2.1523) and 2.81 GB on 11-20, the function count the same (332,000 in the IR). The tests' bodies
alone went from 333 to 718 MB: the game's InputAction had become a union enum of 120 alternatives (kpp-factorio's
#8778), and its destruction, a switch over every alternative, was spelled out inline wherever an InputAction
ended, at each end of scope and in each cleanup. One test's body went from 64 KB to 1.18 MB of IR, 30 such
switches. And InputAction's loading sets the alternative it reads (`this[a.tag] = input.load<a.type>()` in a
`for constexpr`), each setting destroying the one held first: 120 switches of 120 cases in one function, 14 MB.
- **What.** A typed union is destroyed through its destructor helper (`@kpp.dtor.<struct>`, as a class with
  many members already was), and the alternative it holds through a helper of its own (`@kpp.dtor.alt.<struct>`)
  where more than two alternatives need destroying: the switch is spelled out once, in the helper.
- **Measured:** `kpp factorio-test debug --no-server --verbose`, whole builds (build\debug and build\clib
  removed), kpp-factorio kpp 0c18a3229e, kpp 0.2.1738 against it with the change, interleaved, each round
  started at 4 to 10% load:

  | | 0.2.1738 | destruction through the helper | and the alternative's helper |
  |---|---:|---:|---:|
  | IR | 2,809 MB | 2,228 MB | 2,213 MB |
  | codegen | 11.5 to 12.7 s | 9.2 to 10.0 s | 9.1 to 9.9 s |
  | kpp | 21.1 to 21.6 s | 17.6 to 18.2 s | 18.0 to 18.6 s |
  | total | 23.4 to 24.5 s | 20.2 to 20.5 s | 20.3 to 21.6 s |

  The two columns with the change are two windows of three rounds each, 0.2.1738 in both (23.2 to 24.7 s
  in the second): the alternative's helper removes 15 MB more, its time inside the noise. The build is back where the
  game of 11-14 was (kpp 17.3 to 18.0 s); the readme's 15.9 s (0.2.1340, factorio-in-kpp 8dc0c25e2) is
  still 4 to 5 s away.

**A release build's shared definitions only where they are reached (2026-11-23).** kovarex, on generated
functions written into every module: "this should be rule for all functions ... make the IR as small as
possible". A debug build gives each shared function and each global of the shared declarations one home module
(step 2 above, the destructor helpers before); a release build (ThinLTO) kept every one of them in every module,
whether the module named it or not. The game's release IR (kpp-factorio kpp c0ffbea92) was 2,462 MB, of it 1,253
MB copies in modules that never named them: 1,098 MB the two tint tables `Tile::waterEffectTints` and
`pollutedWaterEffectTints` (65,536 colours each, 8.7 MB of IR) in all 64 modules, then constants, string
literals, vtables and the generated enum functions. clang threw them away (the objects were the same bytes), but
read them first.
- **What.** prepareModuleTexts indexes the shared units (a global's line, a comdat, a function's definition) by
  the names that define them and the names they use; moduleText keeps those its own definitions, declarations and
  startup list reach, closed over what the kept units name. A unit in a section, an appending one and LLVM's own
  stay in every module, as declarations do, and what they name is reached. The objects stay what they were: every
  module that names a unit has it, as before.
- **Measured:** `kpp factorio release --no-server --verbose`, the game's module outputs and the LTO cache removed
  before each build, kpp master 171c3bef2 against it with the change, two interleaved pairs under
  `heavy.cmd --exclusive`:

  | | 171c3bef2 | shared units where reached |
  |---|---:|---:|
  | IR | 2,458 MB | 1,229 MB |
  | codegen | 4.80 to 4.93 s | 4.17 to 4.61 s |
  | clang | 11.73 to 12.09 s | 10.56 to 10.77 s |
  | kpp | 19.5 to 19.6 s | 17.4 to 17.6 s |
  | link | 57.8 to 58.0 s | 56.9 to 57.2 s |
  | total | 77.4 to 77.5 s | 74.5 to 74.6 s |

**Typed unions copied, moved and assigned through helpers (2026-11-23).** kovarex: "this should be rule for all
functions ... isn't it the reason why single InputAction generated destructor slowed down the compilation so much?".
A measurement of the special members codegen writes where they are used (a temporary count of the bytes copyConstruct,
moveConstruct, assignObject and destroy add to the function being emitted, never committed): in the game's debug
build 69 MB of its 1,639 MB of IR, in factorio-test's 94 of 2,244 MB, most of it single calls (a String's destructor,
190,000 times in the tests); the bodies of over 2 KB at their places were 25 and 29 MB, and InputAction's copies,
moves and assignments about half of them: 0.29 MB of IR at each of its 21 copies, 0.24 MB at each of its 17 moves,
0.6 to 0.8 MB at each of its 5 assignments, the switch over 120 alternatives spelled out each time (the next were
classes copied member by member: ElementImageSet 1.4 MB at 54 places, MapSettings and ScenarioExecutionContext 0.6 MB
each).
- **What.** A typed union of more than two alternatives is copied, moved, copy-assigned and move-assigned through a
  helper of its own (`@kpp.copy.`, `@kpp.move.`, `@kpp.assign.` and `@kpp.moveassign.` and its struct), as it is
  destroyed: the switch spelled out once, in the helper. The call cannot throw where the union's copy (or move) cannot.
- **Measured:** `kpp factorio-test debug --no-server --verbose`, build\debug's outputs removed before each build,
  three interleaved pairs under `heavy.cmd --exclusive`: IR 2,244 -> 2,225 MB, clang 9.51 to 9.78 -> 9.09 to 9.36 s,
  codegen, kpp and the total within the noise (kpp 17.2 to 18.3 s and 17.2 to 18.0 s).

**Classes copied member by member through helpers (2026-11-24).** kovarex, after the typed unions: "Do the rest as well".
- **What.** A class copied, moved, copy-assigned or move-assigned member by member (no copy or move constructor, or
  operator=, of its own), whose member-wise copy spells out more than 8 calls where it is written, goes through a helper
  of its own as its destruction does (`@kpp.copy.` and the others with its struct): memberwiseCalls counts a call for a
  part copied by a function (a copy constructor written for it, a typed union's helper), none for one copied as bytes,
  a member-wise part's own, an array's per element. A class whose member-wise copy stores a vtable pointer (a
  polymorphic one, a member or base of such, a typed union holding one) stays copied where it is used: a vtable is
  defined in the modules whose functions name it, a helper in its home module only (the game's build did not link
  with them in helpers). The same keeps a typed union with such an alternative inline, whose helper (above) named the
  vtable from the helper's module.
- **Measured:** `kpp factorio-test debug --no-server --verbose` on kpp-factorio kpp c0ffbea92, build\debug's outputs
  removed before each build, master 334cba2f5 against this (scratch\enumnames\tools\irstats.py): IR 2,203.1 ->
  2,196.1 MB, invokes 2,333,365 -> 2,316,958, calls 3,016,157 -> 2,998,813, cleanup pads 301,704 -> 298,593, function
  definitions 357,351 -> 357,412 (61 helpers). Smaller than the typed unions' step: most of the 94 MB of special members
  written where they are used are single calls, and the big member-wise copies were few (ElementImageSet, MapSettings,
  ScenarioExecutionContext). The game's tests pass in debug and release (factorio-test, 9,572 tests, 0 failed).


**Step 5: codegen's mutexes spin before they sleep (item 6's first lever; thread D, 2026-11-04).**
- **Why.** A's profile of the clean build (A-clean-build-profile.md in the coordinator's notes) had
  codegen at 20.5 of 32 cores, its threads waiting 66.7 s on locks held 3.05 s in all: 1.3 million
  short holds, 2.3 µs on average, and 305 µs per contended wait. A waiter on std::mutex sleeps at
  once, and the kernel's wake-up takes longer than the hold it waits for.
- **What.** SpinMutex spins a while (1,000 pauses) before it sleeps (std::atomic's wait,
  WaitOnAddress); SpinRecursiveMutex gives the shared lock std::recursive_mutex's semantics on it.
  They are the shared lock, definitionsMutex, constantsMutex and the shards of NameSet and
  FinalAnswers. No critical section changed, and the output is byte-identical.
- **Measured.** Paired clean builds of the game (`cg\lockspin-measure.ps1`, master b93b3731, 3
  interleaved rounds with the order alternating, load 0 to 3%, 0 stderr lines on both):

  | | master | spin |
  |---|---:|---:|
  | codegen | 4.76 to 4.78 s | 4.48 to 4.56 s |
  | wall | 9.98 to 10.51 s | 9.70 to 9.82 s |
  | kpp's kernel CPU | 7.4 to 8.3 s | 6.5 to 8.0 s |

- **What is left.** The probe (`cg\lockprobe-any.py`, one build each): 55.1 s waited on 2.72 s held,
  against 47.1 s on 2.63 s. The kernel's wake-ups were about a sixth of the waits. The rest is
  queueing, on definitionsMutex (14.9 s) and on the shared lock in structFor (11.4 s) first: item 6's
  next levers.
- **Those next levers, measured: no gain (the same day; not landed).** Each was added on the spin
  locks at 9ce78be2, and each one's objects were identical to the spin locks'. The patches are in thread
  D's scratch: `cg\lockdefs.py`, `lockglobals.py`, `lockstruct.py`.
  - Per-emitter definitions: an emitter keeps its definitions during a level, and they are added once
    its threads are joined, so no definitionsMutex while emitting.
  - Published globals: a defined global's name and thread-local initializer go in FinalAnswers, so an
    emitter's first use of one takes no shared lock.
  - Canonical layouts: a layout is published under its canonical class once its build returns, at any
    lock depth, and a new spelling is found there without the lock. structFor's locked lookups went
    from 77,650 to 35,949, and 25,738 of those are the builds.

  The probe's waits fell from 46.1 s to 32.6, 33.4 and 31.1 s, and its acquisitions from 1.29 to 0.97
  million. Codegen did not get faster. Paired clean builds, 3 interleaved rounds on a quiet machine,
  the undisturbed ones (`cg\lockways-measure.ps1`):

  | kpp | codegen |
  |---|---:|
  | the spin locks | 4.39 to 4.59 s |
  | + per-emitter definitions | 4.44 s |
  | + published globals | 4.48 to 4.57 s |
  | + canonical layouts | 4.47 to 4.61 s |

  So after the spin locks, the threads' waits are not what limits codegen, as step 1's trial of
  per-emitter definitions had found. Where its time goes is the next measurement. Candidates: the
  levels' tails, the work between levels, and contention the probe does not see (the type table, the
  heap).

**Step 6: codegen's time level by level, and the collection between levels on the emitters' threads
(thread D, 2026-11-04).**
- **The timeline.** `cg\timeline-levels.py` is temporary instrumentation, never committed. It records,
  per level and per thread, the time running, waiting and idle. It counts the waits on codegen's spin
  locks, on SharedText's mutex, on another emitter's global and on the type table's interning, and
  the CPU cycles each thread's busy time got. The game's clean build at master 360712a2, 3 runs,
  codegen 4.42 to 4.43 s:

  | part | wall |
  |---|---:|
  | before the levels | 50 ms |
  | 18 levels | 3.40 s: level 0 1.21 s, level 1 0.84 s, level 2 0.48 s, level 3 0.18 s, the rest 0.70 s |
  | serial, between the levels | 513 to 537 ms (157 to 168 ms after level 1) |
  | serial, after the last level | 447 to 453 ms |

  - In the levels the threads are busy 98% of their time: their tails are 1.7 s of 108.6 s. But they
    are on CPU for only 44.3 to 44.8 s of their 106.2 to 106.4 s busy. Of that busy time, 44.4 to
    45.9 s is waits on the spin locks, and about 17 s is neither.
  - What the lock probe does not see is small: SharedText 3 ms, another emitter's global 46 ms, the
    type table's interning 0.5 s. The interning's thread caches, made anew with each level's threads
    (529 of them), cost 11 ms in all.
  - Level 12 (784 items) stalls for 155 to 157 ms in every run: the other 31 threads wait 4.7 s while
    0.19 s is on CPU. No hold of the shared lock is over 15.3 ms (the longest is
    LuaBinder<LuaCommandable>::getFunctionMap()'s).
  - The stall is a hold of definitionsMutex (`cg\timeline-l2.py`: every spin-lock hold over 20 ms):
    156.5 ms in level 12, 106.6 ms in level 1 and 29.8 ms in level 0. The likely cause, confirmed in
    step 7: addDefinition inserts each function's name into definedIn and declareLines, maps that
    reach about 260 k entries. MSVC rehashes such a map as it passes 65,536, 131,072 and 262,144
    entries, re-hashing every name under the lock. That predicts about 26, 50 to 100 and 157 ms for the
    two maps.
  - The 475 ms after the last level are mostly two parts: 254 ms of module assembly (already on the
    threads), and a 130 ms serial loop over the 256 modules (each one's definitions sorted, its
    declarations finished).
- **The collection between levels on the emitters' threads (collectInParallel).**
  - **Before:** collectNextLevel walked every emitter's requests on one thread, against a map of the
    next level's names (copied), then freed them and sorted the level. That took 157 ms for the 42,420
    items after level 1, 103 ms of it the map (`cg\collect-timing.py`).
  - **Now,** for a level of at least 4,096 functions:
    - each emitter's thread scans its own requests against an index of the names (views) and frees
      them;
    - each chunk of the level takes its items' first requesters and is sorted on a thread of its own;
    - the sorted chunks are merged.

    The result is the same level, item for item. The parallel way costs about 5 ms of its own, the
    serial one 2.5 µs an item. The game's objects are identical to master's.
  - **Measured.** Paired clean builds of the game (`cg\l1-pairs.ps1`, 3 interleaved rounds, the order
    alternating, load 0 to 4%, 0 stderr lines), master 12432596 against the collection on that base:

    | | master | the collection on the threads |
    |---|---:|---:|
    | codegen | 4.396 to 4.432 s | 4.146 to 4.203 s |
    | wall | 8.84 to 8.92 s | 8.62 to 8.75 s |

    That is −0.23 to −0.26 s of codegen. With the timeline, the serial gaps between levels went from
    513 to 537 ms to 307 to 329 ms, and the one after level 1 from 157 to 168 ms to 59 ms.

**Step 7: the definition maps reserved, and the modules finished on the emitters' threads (L2; thread
D, 2026-11-04).**
- **Why.** Step 6's timeline left two serial costs:
  - definitionsMutex held 156.5 ms in level 12, 106.6 ms in level 1 and 29.8 ms in level 0, and every
    emitter adding a definition meanwhile waited: level 12's 784 items took 160 ms;
  - after the last level, a 130 ms loop over the 256 modules on one thread.
- **L2a: the definition maps reserved.** Every definition's name goes into definedIn and declareLines,
  and level 0's requests into requestDeclarations, under definitionsMutex (Emitter::addDefinition).
  MSVC's unordered_map rehashes as it passes its bucket count, which doubles (65,536, 131,072, 262,144
  at these sizes), and hashes every name again under the lock. Before the first level, definedIn and
  declareLines are now reserved for twice the program's own functions and startup globals and the
  function instances the resolver made, at most 2^21 names, and requestDeclarations for the own ones.
  A program past the estimate rehashes as before.
  - The game, counted with temporary prints (`cg\mapcount.py`, one build): 2 x (80,632 + 81,533) =
    324,330 names, which MSVC rounds up to 524,288 buckets. At the end definedIn holds 266,292 names
    and declareLines 266,293, both still in 524,288 buckets, and requestDeclarations 48,777 in its
    131,072. None of the three rehashed, so step 6's guess holds: the holds were the rehashes.
- **L2b: each module's own texts finished on the emitters' threads.** The loop after the last level
  (each module's definitions sorted by their text, its declarations given its owned entries and joined,
  the check for a late program piece) takes the modules from a shared counter on the emitters' threads.
  A module's step touches only its own data, and reads the program's functions and the shared literals.
- **The timeline** (`cg\timeline-run.ps1`, one build each; master's is step 6's, at ca061cc6, whose
  level 0 had 19 items and 1 startup global fewer):

  | | master | L2 |
  |---|---:|---:|
  | spin-lock holds over 20 ms | 3, all definitionsMutex: 156.5, 106.6, 29.8 ms | none |
  | level 0 / level 1 / level 12 | 1,238 / 813 / 160 ms | 1,153 / 724 / 3 ms |
  | the modules' declarations finished | 130.0 ms, on one thread | 12.4 ms |
  | after the last level | 475 ms | 362 ms |
  | the levels' waits on the spin locks, thread time | 47.7 s | 37.5 s |
  | codegen, the timeline's total | 4,229 ms | 3,823 ms |

- **Measured.** Paired clean builds of the game (`cg\l2-pairs.ps1`, 3 interleaved rounds, the order
  alternating, load 0 to 6%, no round run again, 0 stderr lines on both), master ca061cc6 against L2
  on that base:

  | | master | L2 |
  |---|---:|---:|
  | codegen | 4.090 to 4.183 s | 3.644 to 3.731 s |
  | wall | 8.08 to 8.15 s | 7.67 to 7.71 s |

  That is −0.36 to −0.53 s of codegen a round, −0.44 s between the medians, more than the −0.25 to
  −0.35 s estimated: about what the timeline removed, 293 ms of holds and 118 ms of the loop.
- **The gate** (on master 1420e41d): run_tests and spec with and without LLVM in kpp (spec 1,087
  pass, 13 pending), spec `--debug-info` and `--release`; the game's 256 objects identical to
  master's, and at 1, 8 and 32 threads with the barrier check on.

**Step 8: layouts off the shared lock (thread D, 2026-11-05; B agreed to the shape).**
- **Why.** Level 0 was serialized by the shared lock's holds, 77% of them class layouts (a lock timeline
  at 23890b5c). A throwaway kpp that splits the lock's layout time exclusively by kind
  (`cg\probe-kinds.py`, master 01c6d615, the game's debug build at -j 32, two runs, codegen 7.6 and 7.7
  s; ms over all levels, calls):

  | kind | held |
  |---|---:|
  | useFunction from vtable slots | 513 / 508 ms (316,273) |
  | descriptorFor | 534 / 571 ms (10,035) |
  | collectVtables | 227 / 213 ms (4,260) |
  | emitVtables' own text | 222 / 220 ms |
  | destructor thunks' bodies | 175 / 169 ms (3,047) |
  | the layouts themselves (buildStruct without its vtables) | 395 / 371 ms (25,743) |
  | classSizeAndAlign's sums | 39 ms (15,209) |
  | needsDestruction's | 21 ms (23,040) |

  About 2.1 s a build held the lock, 80% of it vtables: claims on the layouts alone would have freed
  0.4 s. descriptorFor and useFunction also take other locks inside (constantsMutex, the request and
  declaration shards, decls), so part of their time is waits made while holding the shared lock.
- **Step 1 (efcb5112).** A class's size and alignment are set when its layout completes, so
  classSizeAndAlign reads a complete layout without the lock (the contains-itself guard is per emitter);
  needsDestruction lays the class out before it locks; irTypeName's names have a mutex of their own.
- **Step 2.** structFor builds a layout, its vtables and its descriptor included, as the emitter's own
  copy without the shared lock, and publishes it whole under sh.structsMutex. When two emitters build
  one class at once, the copy published first is the class's (the same either way) and the other is
  kept aside, so nothing points at freed memory. Nobody waits for a layout another emitter builds, so
  there is no lock order to keep. The struct's type line and result.structs are added once, by the
  copy published; requests and declarations are by name, constants by their contents, destructor
  helpers once by name. A published layout's descriptor is made on demand under sh.descriptorsMutex.
- **Checks.** gate.cmd (spec 1,145); determinism.ps1 -Full's test programs at 1, 8 and 32 threads;
  incremental.ps1 (its 12 layout edits); the game (factorio-in-kpp 3d7d6f99, debug): its IR identical
  with step 1's, and its 256 objects identical at --codegen-threads 32, 32, 8 and 1 (KPP_CHECK_BARRIER)
  and with step 1's (`cg\gate-spin2.ps1`).
- **Measured.** Interleaved clean builds of the game (`cg\multi-codegen.ps1` with per-level timers,
  `cg\probe-levels.py`, never committed; 6 rounds, the order rotated, each build after five samples under 15%
  load where that came within 2 minutes; another thread's work loaded the machine in about half of them).
  The builds started quiet (load under 10%) and undisturbed:

  | kpp | codegen | level 0 |
  |---|---:|---:|
  | master a11f5c72 | 4.759, 4.776, 4.784 s | 1,164, 1,180, 1,198 ms |
  | step 1 efcb5112 | 4.663 s | 1,128 ms |
  | step 2 | 4.252, 4.310, 4.311, 4.445 s | 766, 782, 796, 837 ms |

  Step 2 against master: −0.47 s of codegen between the medians (−0.31 to −0.53 s), and −0.39 s of
  level 0 (−0.33 to −0.43 s), B's estimate −0.3 to −0.5 s. Step 1 alone is within the noise, as expected.
  Counting every build, the loaded ones too, the medians were 4.84 s (master), 6.21 s (step 1, four of its six
  builds under load) and 4.43 s (step 2).
- **What is left, measured: no gain (the same day; not landed).** After step 2 a per-call-site probe of the
  shared lock at level 0 (`cg\probe-sites.py`, master 946f42f7, two runs on a machine others loaded to 93-100%)
  had it held 387 / 440 ms and waited 12.9 / 12.6 s of thread time: a global's definition
  (ConstantData:425) 147 / 165 ms, destructorThunk 97 / 105, closureFor 56 / 66, needsDestruction 23 / 29
  (24.5k holds), useVtable 22 / 24. Three levers took those out of the lock: destructor thunks claimed by name
  and emitted without it, defined globals published (FinalAnswers) so another emitter's first use takes no lock,
  and closures laid out without it, the first published winning. Their objects were identical to master's and
  at 32, 32, 8 and 1 threads, and concurrency.ps1 passed. Interleaved clean builds on a quiet machine
  (`cg\multi-codegen.ps1`, 8 rounds, load 0 to 5% before each, master b6d05bcb against the three on it):

  | kpp | codegen | level 0 |
  |---|---:|---:|
  | master | 4.211 to 4.255 s, median 4.238 | 747 to 778 ms, median 758 |
  | the three levers | 4.188 to 4.273 s, median 4.221 | 738 to 806 ms, median 764 |

  On a quiet machine the shared lock no longer limits level 0; what the probe saw was the load. The patches
  are in thread D's scratch: `cg\levers-nogain.patch`.
## 7. Today: kovarex's clean build through kpp, end to end (thread D, 2026-11-02)

**The build.** kovarex measured it on his second machine (PC-PRACOVNA's hardware): 16.8 to 17.0 s for
`kpp --stop-server`, build\debug removed (build\clib kept), then `kpp factorio debug`. kpp reported
"total 16.4 s: kpp 12.3 s, link 2.3 s".

**How it was measured here.** On PC-PRACOVNA, on a quiet machine: the other threads were held, and
the load was 0 to 9% before each round.
- **kpp:** the release package built from master 44d0f0fa with `tools\release\package.cmd
  -NoSmokeTest` (kpp 0.2.54, kpp.exe with LLVM's back end), so the client, build.kpp's run and the
  C-library check are the release's code.
- **The project:** a copy of factorio-in-kpp e4cfd9ba: build.kpp, src, libraries and tests, with data
  by junction. build\clib was built once and kept.
- **Each round:** a clean build with a fresh compile server, as kovarex runs it. One more
  configuration, `debugnoinfo` (debug with `debugInfo = false`), was added to the copy's build.kpp to
  measure what debug info costs.

| | rounds | wall | kpp's own line |
|---|---:|---:|---|
| `kpp factorio debug` (kovarex's build) | 6 | 17.4 to 19.2 s, median 17.5 s | total 16.5–18.3 s: kpp 12.5–13.3 s, link 2.3–3.5 s |
| `kpp factorio debugnoinfo`, back end in kpp | 6 | 13.2 to 13.8 s (one round 16.6 s) | kpp 9.8–10.0 s, link 1.1–1.3 s |
| the same, `KPP_BACKEND=clang` | 3 | 13.96 to 14.03 s | kpp 10.5–10.6 s, link 1.1 s |

**Where the time goes.** A temporary probe (cg\marks.py, never committed) stamps every phase
boundary in the kpp client and in the compile server with QueryPerformanceCounter. That is one clock
for every process and the outer timer (PowerShell's Stopwatch), so the phases add up to the wall
time. The table gives the medians of 3 rounds each: they add up to 17.39 s of a 17.48 s median wall,
and 13.42 s of 13.42 s. CPU is the process's own (the server's for its phases).

| phase | debug | without debug info | CPU (debug) |
|---|---:|---:|---:|
| kpp.exe starts | 0.02 s | 0.02 s | |
| build.kpp compiled and run (through build.kpp's own server) | 0.32 s | 0.31 s | |
| the C libraries checked (up to date) | 1.47 s | 1.48 s | 1.47 s (one thread) |
| the build's compile server started and connected | 0.05 s | 0.05 s | |
| read the stdlib and the sources | 0.07 s | 0.07 s | 0.42 s |
| lex | 0.09 s | 0.09 s | 1.64 s |
| parse | 0.05 s | 0.05 s | 1.20 s |
| the C imports (cached) | 0.20 s | 0.19 s | 1.47 s |
| index, out-of-line definitions | 0.22 s | 0.22 s | 0.66 s |
| resolve, check, instantiate | 1.56 s | 1.53 s | 31.5 s |
| codegen (emission, module assembly) | 4.94 s | 4.72 s | 70.2 s |
| the back end, 128 modules | 5.41 s (clang) | 2.88 s (in kpp) | clang's own: 149 s (below) |
| the link (link.exe /INCREMENTAL, /DEBUG) | 2.43 s | 1.25 s | |
| after the build: build.kpp's after-build copy of the exe (108 MB) and PDB (218 MB) to bin\Debugx64, and kpp's exit | 0.55 s | 0.55 s | |
| **total** | **17.39 s** | **13.42 s** | |

- **Debug info costs 4.1 s of the 17.5 s.** The back end takes 2.5 s more, the link 1.2 s and
  codegen 0.2 s. The IR is 1,360 MB against 720 MB without it. The reason: projectlib's debug
  configuration builds with `--debug-info`, and a `--debug-info` build keeps every shared global in
  every module (no homes, step 1) and compiles through clang (not in kpp, step 3). Neither step has
  reached kovarex's build.
- **Codegen's locks** (cg\lockprobe-at.py, temporary; this run's codegen took 7.1 s instead of 4.9 s
  under the probe): its threads waited 62.6 s in all on locks held 3.4 s in total. They are many
  short holds that the threads queue on:

  | site | acquisitions | contended | waited | held |
  |---|---:|---:|---:|---:|
  | addDefinition's definitionsMutex | 262,720 | 89,950 | 15.9 s | 0.84 s |
  | structFor (the shared lock) | 52,369 | 29,027 | 15.6 s | 0.95 s |
  | needsDestruction | 36,729 | 16,670 | 5.9 s | 0.06 s |
  | closureFor | 23,033 | 11,503 | 4.6 s | 0.15 s |
  | globalFor (two sites) | 33,564 | 14,725 | 7.0 s | 0.19 s |
  | useVtable | 11,605 | 5,483 | 1.8 s | 0.04 s |
  | classSizeAndAlign | 10,532 | 4,562 | 1.7 s | 0.24 s |

- **The back end through clang** (cg\backend-profile.py on the build's 128 modules, quiet, as kpp
  compiles them):
  - 5.58 s of wall at 32 at a time; the clang processes take 148.9 s of CPU.
  - Summed over the processes: the IR parse and setup about 41 s; code generation 112 s, of which
    instruction selection is 28.2 s (SelectionDAG's own work 13.0 s), the assembly printer 12.5 s,
    and debug values 5.1 s.
  - FastISel falls back to SelectionDAG for the arguments of every function: 330,430 of 330,430.
    The other fallbacks are 466,227 terminators and 72,896 calls (`-Rpass-missed=sdagisel`).

**The next speedups, ranked by measured gain per cost.** Each gain is against kovarex's 17.5 s. The
gains overlap: 3 and 4 both shorten the back end.

| | what | gain | cost | owner | basis |
|---|---|---:|---|---|---|
| 1 | build.kpp's after-build: a hard link or kpp writing bin\Debugx64 directly, not a copy of the exe and PDB | −0.5 s | one line | factorio-in-kpp's build.kpp, C | 0.55 s measured after the build |
| 2 | the C-library check: once per header (E's 0418c983, after the measured build), then in parallel if still long | up to −1.3 s | small | E | 1.47 s measured at 44d0f0fa, one thread; to be measured on master |
| 3 | `--debug-info` builds with homes and the back end in kpp | **done**: −2.6 to −2.9 s measured (estimate −2.3 s) | medium | D, B reviewed | 1a8e88c5, 8b5c902c; below |
| 4 | streaming (step 4: done, P1 4d884644 and P2 91671241, measured −2.4 s; clang's processes not streamed) | about −2.5 s (−1.8 s after 3) | large | D, B reviewing | P0: instance pieces alone take about half the back end; the back end after codegen would be about 2.7 s instead of 5.4 s |
| 5 | the debug link: `/DEBUG:FASTLINK` | **none**: this link.exe (MSVC 14.44) no longer supports it ("LNK4315: /DEBUG:FASTLINK is no longer supported. Using /DEBUG:FULL instead"); both links 1.54–1.59 s, the same 207 MB PDB (cg\linkfast.ps1) | — | — | struck |
| 6 | codegen's locks, second round: mutexes that spin before they sleep (**done**, step 5), then shorter holds (per-emitter definitions, published globals, layouts found by their canonical class without the shared lock: measured, no gain, step 5) | **−0.2 to −0.3 s** of codegen measured (the estimate was −0.5 to −1.5 s) | medium | D | before: 4.94 s at 70 s CPU, 62.6 s of waits on 3.4 s of holds; after the spin locks: 47.1 s of waits on 2.63 s, and cutting a third of them gained no codegen time |
| 7 | FastISel lowering Win64 arguments, upstream in LLVM | −0.3 to −0.5 s | an LLVM patch and a release | D | SelectionDAG is 13 s of the back end's 149 s CPU |

**Item 3, done (thread D, 2026-11-02; B reviewed).**
- **Homes with debug info (1a8e88c5):** kpp's debug info is line tables only (DICompileUnit's
  LineTablesOnly), and no global's line carries metadata: none of the game's 1,572,280 linkonce_odr
  global lines in a --debug-info build. So the homes needed only the exclusion lifted.
  - The game with --debug-info: IR 1,317.8 -> 1,056.4 MB (−19.8%).
  - cg\homecheck.py against master's modules: 0 problems. Every module's text, its debug metadata
    included, is the same apart from its global, comdat and type lines. Each of the 61,905 globals
    is defined once with master's line and declared external elsewhere.
  - Spec language/homed_globals_with_debug_info.
- **The back end in kpp with debug info (8b5c902c), B's should-fix (4) of 265df4c2:** each object is
  clang's apart from what names the compiler in CodeView, object by object (cg\cvcompare.py):
  - COFF: every section's bytes but .debug$S and .debug$T, every section's relocations, the symbol
    and string tables are the same.
  - CodeView (llvm-readobj --codeview) is the same once three things are set aside: S_COMPILE3's
    BackendVersion (Visual Studio's clang 19.1.5, kpp's LLVM 19.1.7), LF_BUILDINFO's build tool
    and command line (left empty by kpp, B: tools re-run that line), and S_BUILDINFO's index of it.
  - The game: 128 of 128 objects. windows-gnu: the 45 end-to-end programs' 3,017 of 3,017.
- **The game, both changes:**
  - `--dump-data` 7/7 equal, and the 45 menu maps' checksums equal;
  - the unit tests built with --debug-info: 9,501/9,501 with 0 CRC mismatches against C++ Debug;
  - KPP_CHECK_REBUILD=37 and =7 on its --debug-info build: the modules the compile server would
    assemble again are the build's, 0 of 128 different (2,683 and 13,827 functions emitted again).
- **Time, kovarex's build** (`kpp factorio debug` in the project copy, a fresh server per round,
  interleaved, load 0–10%):

  | | wall | kpp's line | back end | link | IR |
  |---|---:|---|---:|---:|---:|
  | master ac36aa71 | 15.7, 16.2 s | total 14.8/15.3 s: kpp 12.5/12.9 s | 5.3–5.5 s | 2.2–2.3 s | 1,325 MB |
  | with item 3 | 13.2, 13.3, 13.6 s | total 12.6–13.0 s: kpp 10.9–11.2 s | 3.8–4.0 s | 1.6 s | 1,064 MB |

  (Master was already below section 7's 17.5 s: P1's homed shared functions and E's 0418c983, the
  C-library check, landed between.)

Not ranked here: resolve, check and instantiate (1.56 s, 31.5 s CPU: thread A's), and kpp's own
overhead in the leaf edit (3.6–3.8 s through kpp: thread E's breakdown).

**Commands and tools** (thread D's scratch, cg\):
- **The package:** `tools\release\package.cmd -NoSmokeTest` at master 44d0f0fa. No download: npm
  was off the PATH (no kpp.vsix), and llvm-mingw came from an existing unpack.
- **The rounds:** `fikprofile.ps1 -Rounds 3 -Variants ...` (pkg, pkgnodi, pkgclang, mark,
  marknodi). Each round stops every kpp server: `kpp --stop-server` with the build's own options,
  since the server's key includes them. It removes build\<configuration> until it is gone (the PDB
  server can hold it a while), waits for 3 load samples under 5%, then runs `kpp factorio
  <configuration> --verbose`.
- **The breakdown:** `marks.py` (the probe, applied to a checkout and built, then reverted),
  `marksummary.py profile mark`, `marktimeline.py profile\mark-1`.
- **Codegen's locks:** `lockprobe-at.py CODEGEN.cpp` (a probe build), run with
  `KPP_LOCK_PROBE=1 kpp factorio debug --no-server --verbose`; ranked by `lockrank.py`.
- **The back end:** `backend-profile.py DIR` (clang's -ftime-report over the modules) and
  `backend-profile.py DIR --remarks` (FastISel's fallbacks).

## 8. Where a clean build waits (2026-11-23)

kovarex asked how close a full rebuild of the game can come to 5 s. The study's answer in short: a clean
build is limited by its work, not by its schedule. On one thread (`-j 1`) it is 155 s: the front end 17 s,
codegen 37 s (26 s emitting, 10 s assembling the module texts), LLVM 98 s (26 s of it parsing the IR text,
62 s making machine code). On this machine perfectly parallel work (LLVM's) runs 19 times faster than on
one thread, not 32, and kpp's own phases 9 times. What a better schedule can still give is the time the
machine waits, measured here.

**Method.** `kpp factorio debug` as the readme builds it (the compile server stopped, build\debug removed
until it is gone, a new server), the game at kpp-factorio 0c18a3229e, every run inside
`heavy.cmd --exclusive`. Temporary timers, never committed, mark each phase's end on one clock for the
client and the server, and a second process samples the machine's CPU use every 50 ms. Two compilers are
compared in interleaved rounds; the machine was 5 to 10% busy with other threads' work before each build.

**Where it waits** (of 32 hardware threads; one 13.3 s build, sampled with the changes below in: they
shorten the rows of the barrier and of codegen's end, by 0.1 s and 0.3 s):

| from | to | busy | what runs |
|---:|---:|---:|---|
| 0 s | 0.25 s | 5 to 7 | the client: build.kpp's description (kept), the server started |
| 0.25 s | 1.05 s | 12 to 15 | read, lex, parse, the cached C import, index |
| 1.05 s | 2.0 s | 27 to 32 | resolve, check, instantiate |
| 2.0 s | 2.4 s | 9 to 12 | codegen's setup, level 0's start (the startup globals) |
| 2.4 s | 3.2 s | 26 to 29 | level 0 |
| 3.2 s | 3.6 s | 9 to 17 | level 0's last functions, its requests collected, the barrier |
| 3.6 s | 5.8 s | 32 | levels 1 and 2, LLVM on the streamed program pieces |
| 5.8 s | 6.8 s | 17 to 26 | levels 3 to 16, the instances' static asserts; LLVM's threads have nothing left |
| 6.8 s | 7.6 s | 10 to 16 | codegen's end: shared functions homed, module texts prepared |
| 7.6 s | 8.2 s | 32 | the modules assembled |
| 8.2 s | 11.0 s | 32 | LLVM on the instance pieces |
| 11.0 s | 13.3 s | 3 to 8 | the link |

- **LLVM's threads wait 2 s for the instance pieces.** They finish the streamed program pieces 6.5 s into
  the build and get the instance pieces at 8.5 s. Part of codegen's end ran on one thread meanwhile: the
  shared functions homed (0.29 s), the .natvis made (0.1 s), and of the module texts' preparation (0.34 s)
  all but the debug classes' definitions.
- **At level 0's barrier** every emitter and the back end waited 0.1 s for the program functions' names to
  be gathered on one thread.
- **Before the resolve** the cached C import asks clang for its version and search list in a process of
  its own (0.07 s), then lexes (0.11 s) and parses (0.12 s, one thread) the imported text, and the
  index's out-of-line definitions take 0.09 s on one thread.
- **After the back end** kpp asks clang for link.exe's command line (0.07 s: the answer is kept beside
  the executable, which a clean build deletes) and frees the module texts (0.05 s).
- **The link** is 1.7 to 2.2 s on a few threads. No linker is faster while a PDB is written: link.exe not
  incremental 1.64 s, lld-link 2.04 s (1.70 s with type hashes in the objects), against 1.90 s; without a
  PDB link.exe takes 1.11 s and lld-link 0.71 s (the same 256 objects, the median of three each).

**What was changed.** Each change is exact: the game's 256 objects, its .natvis and its link response are
the same bytes with it and without (the COFF time stamp aside).

- **The shared functions homed on the threads.** Each entry's own part (the function it holds, its
  declaration, its home, its definition's text scanned for what it names) runs on the emitters' threads.
  The three tables they go into (definedIn, declareLines, the modules' definitions) share nothing, so
  each of the two maps is filled on a thread of its own while a third places the definitions, all in
  the entries' order.
- **The program functions gathered beside each other.** The set of their names and the map of their
  files are two tables, filled on two threads.
- **The .natvis made while the back end compiles,** once the modules are handed to it, not before.
- **NameHash for codegen's tables keyed by names** (src/NameHash.hpp). The standard library hashes a
  string a byte at a time (MSVC's FNV-1a), and codegen's keys are IR names hundreds of bytes long: 6.6% of
  the front end's and codegen's samples on one thread were that hash, 15% of module assembly's. NameHash
  takes sixteen bytes a step. Where a hash decides what is written (a function's module, a global's
  home), std::hash and FNV stay.

**Measured** (five interleaved pairs, master d197097d0 against the four changes on it; medians, each
round's wall time in brackets):

| | master | with the changes |
|---|---:|---:|
| wall | 14.31 s (14.34, 14.31, 13.67, 14.02, 14.67) | 13.55 s (13.58, 13.55, 13.46, 13.54, 13.64) |
| kpp | 11.69 s | 10.96 s |
| of it codegen | 6.58 s | 5.97 s |
| the shared functions homed | 0.285 s | 0.079 s |
| the program functions gathered | 0.097 s | 0.019 s |
| the link | 1.98 s | 1.78 s |

The build is 0.76 s shorter, in every pair. (This session's builds were slower than the 13.3 s of the
timeline's: the machine was busier.)

## 9. What the game's IR is made of, and what in it was for nothing (2026-11-23)

kovarex: "search the code we generate ... to find hidden things bloating the IR and generated code. As
the less code we have to process, the less all the other steps have to do." A census of the game's debug
IR, then what each kind of text costs LLVM, then what was removed.

**The census** (kpp 171c3bef2, the game at kpp-factorio 0c18a3229e, `kpp factorio debug`: 256 modules,
1,609.6 MB of text, 271,707 functions, 10,724,975 instructions in 1,511,050 blocks):

| what | text | share | count |
|---|---:|---:|---:|
| function bodies | 832.7 MB | 51.7% | 271,707 functions |
| debug information's nodes | 556.6 MB | 34.6% | 5,197,413 |
| function declarations | 73.8 MB | 4.6% | 767,965 lines for 222,908 functions |
| global definitions | 70.5 MB | 4.4% | 167,782 |
| type definitions | 67.0 MB | 4.2% | 779,956 |

Inside the bodies: the `, !dbg !N` suffixes are 189 MB (23% of the bodies), getelementptr 1,847,622
instructions (757,488 of them with every index zero), calls 1,618,591 (655,003 of them the asm `nop`
around an invoke), invokes 327,504, loads 2,171,689, stores 1,451,644, allocas 1,088,124. 9.4% of the
whole text is padding: the zeros and spaces that keep a metadata reference's width fixed so that module
assembly can renumber it in place. Template instances are half of the bodies (lambdas in them included),
std::Function's five functions per lambda 4.3%. The largest 556 functions (0.2%) hold 10% of the bodies,
the largest 4,369 a quarter. 2.8% of the bodies are the same text as another body but for their name.
Only 2.2% of the bodies are code nothing can reach from the program's roots (9,965 functions). A
module's compile time goes with its size (42 to 84 ms per MB): no function is slow out of proportion.

**What was found in the bodies:**

- **Two asm `nop`s around every call that may throw**, 6.1% of the instructions (Emitter::emitCall:
  the byte after such a call belongs to its `__C_specific_handler` scope entry and must be no other
  block's). Both are needed on Windows, see below; the one after was also written where exceptions are
  Itanium's, which have no such byte.
- **A third of the invokes call a function that cannot throw** (about 106,000 of 327,504, by the least set
  over the whole program's call graph: a function may throw if it throws, calls through a pointer, or
  calls one that may). Nearly half of those are the standard library's (String's constructors and
  assignment from a String&&, StringView from a String, empty, c_str, UniquePtr::release, Optional's
  constructors), the rest the game's (MapVersion's comparison, LogSite, agui::hold). 9,736 call a
  function declared `noexcept`; 15,488 of the 60,220 functions with an invoke would have none left.
- **4.3% of the instructions computed a value nobody used or wrote a slot nobody read**: 157,075
  conversions of a constant (`sext i32 1 to i64` for `size + 1`), 96,503 allocas that were only stored
  to with 139,770 stores into them (three quarters of them the `this` slot each default member
  initializer got, named or not), 19,191 allocas never named, 19,138 loads and 17,867 address steps
  never used.
- **One copy of an InputAction is 370 KB of IR** wherever one is copied (std::insertionSort of them
  1.8 MB, Vector<InputAction>::assignCopies 1.07 MB, about thirty functions of 368 to 390 KB): the
  typed unions' copies and moves spelled out at each place, as their destruction was until 0e30e21db.
  Thread "Write generated functions once per build" has it.

**What each kind costs the back end.** A quarter of the modules (64, every fourth) rewritten without one
kind of text, each module compiled with every variant back to back on a core of its own, three to five
rounds, each (module, variant)'s least time kept and summed. First by Visual Studio's clang.exe (the
game's compile line, CPU time; a difference under 1.5% says nothing):

| variant of the IR | clang.exe's time | text | objects |
|---|---:|---:|---:|
| no asm `nop` at all (not correct code: the floor) | -9.0% | -2.3% | 0 |
| no `nop` after an invoke, the one before kept | -5.2% | -1.4% | -0.1% |
| that, and the `nop` before as a volatile load of a stack byte | -10.6% | -1.2% | +0.1% |
| the classes' member function lists left out of the debug information | -3.7% | -3.5% | -3.8% |
| jumps assembled in their long form at once (`-mrelax-all`) | -3.4% | 0 | +0.5% |
| invokes of the functions that cannot throw as calls | -9.4% | -1.5% | -2.5% |
| no getelementptr with every index zero | -0.1% | -4.6% | 0 |
| metadata numbers without padding | -0.1% | -11.2% | 0 |
| the metadata nodes before the functions (no forward references) | -0.6% | 0 | 0 |
| a block of its own for each invoke (FastISel for what comes before it) | -0.4% | +1.3% | 0 |

Then by kpp's own back end (a temporary entry point, never committed; wall time of a pinned process,
whose noise is about 2%): `-mrelax-all` -2.5%, no `nop` after an invoke -1.7%, the `nop` before as a load
too -1.9%, both with `-mrelax-all` -3.6%, and with the invokes of the functions that cannot throw as
calls -5.9%; the member function lists, the address steps of offset zero and the padding: nothing.

**The two disagree where the work is allocation.** An asm statement is assembled by a parser LLVM
makes for that one statement, a class's member functions are many small type records. clang.exe
allocates with the C runtime, kpp's LLVM with rpmalloc: what was 9% of clang's time is under 2% of
kpp's. kpp's own back end decides here, and whole builds on one thread (below) are its measure; by
clang.exe alone the `nop`s would have been the largest find by far.

**What was changed** (each with the program's behaviour unchanged; the game's tests, 9558 of them, pass):

- **The `nop` after a call that may throw is written only for Windows' own exceptions**, not on
  every target.
- **A direct call of a function whose definition is `noexcept` is a plain call**: no unwind edge, no
  `nop`, no cleanup pad (Emitter::callMayThrow). Its own body ends the program where an exception
  would leave it. Only `noexcept` with no condition: a condition is evaluated for the instance's own
  copy of the function when that is emitted.
- **An integer literal converted to another integer type is written converted.**
- **The `this` slot of a default member initializer is made and filled where the initializer names
  this.**
- **An unoptimized module's jumps are assembled in their long form at once** (`-mrelax-all`, clang's
  own default at -O0 until LLVM 19, in the back end in kpp and on clang's compile line): the
  assembler's layout relaxes nothing. It gains more the larger a module's code section is.

**A change taken back.** d55ff8e62 also dropped the `nop` after an invoke on Windows, as covered by
the one before the next invoke. It is not: where the call does not return (a throw, an allocation's
failure) nothing of its block follows it, the block laid out next may be a handler's, and
`__C_specific_handler` runs no cleanup of a scope whose entry the unwind's target lies in. With the
handler's first byte inside the call's entry (its return address + 1) the objects of the try block
were not destroyed before the handler ran: the differential suite's life/5 and life/6 failed in
release builds (an array's elements left alive by a constructor that threw in a new-expression beside
them), on master for one CI run, 2026-11-24. The gate does not run that suite; a change to what a
call or a scope compiles to needs `compiler\tests\differential\run.py` in debug and release before
its push, as a parser change does. The numbers below are from before the `nop` came back. With it
back, at master bef9cafe9 (which also has another thread's 8a6a69bc0, a typed union copied through a
helper): 1,575.6 MB of text, 10,180,876 instructions, 629,457 `nop`s, 314,731 invokes, 154,643 cleanup
pads, 1,012,348 allocas, 1,328,466 stores, and three builds on one thread with LLVM's part 71.6 s
(71.62, 69.88, 71.66) and all of it 124.3 s. Those builds are a later session's, not paired with the
table's: the 1.7% that the `nop` costs kpp's back end by the variant timing is less than two sessions
differ by.

Not changed: the `nop` before an invoke as a volatile load (inside the noise in kpp's back end, for
larger code), the address steps of offset zero and the padding (they cost the back end nothing).

**Measured** on the game (kpp-factorio 0c18a3229e):

| | master 2a12f755a | the four emitter changes | with `-mrelax-all` too |
|---|---:|---:|---:|
| IR text | 1,609.6 MB | 1,572.2 MB | 1,572.2 MB |
| instructions | 10,724,975 | 10,044,015 | 10,044,015 |
| asm `nop`s | 655,003 | 317,766 | 317,766 |
| invokes | 327,504 | 317,768 | 317,768 |
| cleanup pads | 160,630 | 155,713 | 155,713 |
| allocas | 1,088,124 | 1,012,351 | 1,012,351 |
| stores | 1,451,644 | 1,341,859 | 1,341,859 |
| a build on one thread (`-j 1`, three each): LLVM | 75.65 s | 74.27 s | 71.90 s |
| a build on one thread: all of it | 130.5 s | 127.6 s | 127.1 s |

At 32 threads LLVM is 5.2 s of the build's 13.5 s and part of it runs beside codegen, so 5% of its work
is about 0.2 s of the build. Six rounds of whole builds did not resolve that: their medians (14.08 s
for master, 13.14 s with the emitter changes) are closer than one compiler's own rounds (13.4 to
15.2 s for master, on a machine other threads were using). The one-thread builds are the measure.

**Left, by what it would give:**

- **The functions that cannot throw found by the compiler**: a third of the invokes, their `nop`s and
  12% of the cleanup pads, objects 2.5% smaller; about 2% of the back end's time. The emitter knows
  what a function calls only once it has emitted it, and its callers are emitted before it: the
  invokes would be rewritten in the functions' text at module assembly, from each function's list of
  calls. A part comes for less: `noexcept` on the standard library's functions the C++ standard
  declares so (nearly half of those invokes are the library's).
- **The member function lists in the debug information** (301,359 declarations no definition in their
  module names, with a type node and an argument list each): no time in kpp's back end, 3.8% of the
  objects' size and the linker's work on them. Visual Studio uses them to call a member function from
  the watch window. (Done on 2026-11-25 for the member functions that have no code, the bulk of them:
  section 10. What the variant here did not show is kpp's own time making the lists, and the classes
  their types alone named.)

## 10. What LLVM does with the IR, by phase, by pass and by function (2026-11-24)

Section 9 counted the text. This one measures the back end itself on it: where its time goes, what in
that is work for nothing, and what the build as a whole waits for.

**Method.** A quarter of the game's modules (64, every fourth of the 256 of kpp bef9cafe9's debug
build of kpp-factorio 0c18a3229e, 385.6 MB of IR), compiled by kpp's own back end through a temporary
entry point (never committed) that marks each phase of a module's compile and hashes its object. Six
processes, each on a core of its own, compile a module from every variant in turn, three rounds, and
each (module, variant)'s least time is summed; the machine does nothing else (`heavy.cmd --exclusive`).
A difference under 2% says nothing. Beside it, Visual Studio's clang over four of the modules with
`-ftime-report` (time by pass) and `-Rpass-missed=sdagisel -mllvm -fast-isel-report-on-fallback` (what
the fast instruction selector does not select), and a sampling profile of that entry point over sixteen
modules (kpp linked with a PDB: the linker's public symbols name LLVM's functions).

**The phases** of the 15.50 s the 64 modules take:

| phase | time | share |
|---|---:|---:|
| the text parsed | 4.70 s | 30.3% |
| verified | 0.71 s | 4.6% |
| the target made, clang's -O0 pipeline (the always-inliner) | 0.09 s | 0.6% |
| machine code and the object | 9.39 s | 60.6% |
| the module freed | 0.61 s | 3.9% |

**The passes of the machine code** (clang's own timers, four modules; a pass that does nothing shows
about 0.2%, its timer's own cost): instruction selection 41.5%, the assembly printer (with the object's
debug information) 20.9%, live debug values 7.8%, the fast register allocator 4.0%, Windows exception
handling preparation 3.4%, prologue and epilogue insertion 2.5%, "expand large div/rem" 2.1%, the
two-address pass 1.9%; some eighty others under 1% each.

**By function** (2,636 samples): no function of the machine code's holds 1.5%. The one that stands
out is the parser's lexer: `LLLexer::LexIdentifier` 5.2% alone, about 10% with the rest of the lexer
and the `memcmp` under it. It finds a keyword by comparing the token against LLVM's 400 keywords one
after another, and `ptr` and the instructions' names are near the end of that list. Then `malloc` 3.8%,
the parser's map of metadata numbers (`parseMDNodeID` and the `std::map` under it, one look per
`!dbg`) 3.0%, and the names of local values (`Value::setValueName`, `getName`, the symbol table's
`StringMap`) 4.4%.

**The fast instruction selector's fallbacks** (four modules, 4,437 functions). It selects a block from
its end, and where it cannot select the terminator the whole block goes to SelectionDAG: 2,913 blocks
ending in an `invoke` (it has none), 1,550 in a `cleanupret`, 491 in a `ret i1` (it returns an `i1`
only where the function says how the rest of the byte is filled, `zeroext`). Every function's
arguments are lowered by SelectionDAG on Win64, 321 calls go there alone (207 of them kpp functions:
an `i1` argument, a struct by value), 254 instructions take the rest of their block with them
(`catchpad`, a struct loaded or stored as one value, atomics). It costs less than it sounds: with every
`invoke` in a block of its own, so that the code before it is selected fast, clang's time fell 0.4%
(section 9), and `zeroext` on every returned `i1` gains 0.7% here.

**The variants**, against the modules as kpp writes them:

| variant | time | parsed | machine code | freed | objects |
|---|---:|---:|---:|---:|---|
| live debug values only where a function has a debug instruction | -5.9% | | -9.3% | | the same bytes |
| instruction results as numbers (`%12`), not names (`%t12`) | -2.4% | -4.6% | -1.2% | -14.7% | the same bytes |
| that, and the labels | -3.7% | -7.4% | -0.6% | -22.5% | the same bytes |
| both of the two above | -9.4% | -8.3% | -9.8% | -27.0% | the same bytes |
| a `ptr` parameter read as the parameter, not from its slot | -1.7% | -3.4% | -0.7% | | 0.25% smaller |
| every parameter so | -3.0% | -3.3% | -2.7% | | 0.23% smaller |
| `zeroext` on a returned `i1` | -0.7% | | -2.3% | | 0.07% larger |
| line tables only (LLVM's own stripping, after the parse) | -11.3% | | -16.3% | | 27.6% smaller |
| no debug information (stripped after the parse) | -25.0% | | -36.8% | | 46.1% smaller |

- **Live debug values.** LLVM's "Live DEBUG_VALUE analysis" extends the ranges of DBG_VALUE
  instructions over a function's blocks, and it walks every instruction of every function to do it,
  with its tables made anew per function. An unoptimized kpp function has no such instruction: a
  variable declared on a slot made at the function's start is a row of the frame table. Of the 68,911
  functions of the 64 modules, 2,664 have one (a lambda's capture, declared on an address the body
  computes). The pass is 5.9% of the back end's time, for 96% of the functions to no effect.
- **Names.** A named local value is an entry of its function's symbol table and of the context's map
  of names, made at the parse and taken out when the module is freed; an unnamed one (`%12`) is a
  slot of a vector. LLVM 19 takes numbers with gaps, but they must rise in the text's order.
- **Parameters.** Reads of a `ptr` parameter from its slot (`this`, references, pointers that are
  never assigned) are 9.1% of these modules' instructions and 41% of their loads. They are nearly
  free: the instruction selector folds such a load into its use.
- **Debug information** is a quarter of the back end's time after the parse, and 46% of the objects.
  What it costs the parse is not in these numbers (the text is parsed, then stripped): it is 46% of
  the text (section 9).

**What was changed.**

- **Live debug values run only for a function that has a debug instruction** (backend.cpp,
  LiveDebugValuesWhereAny: LLVM's own pass, held by a pass of kpp's that looks first). The objects
  are the same, byte for byte: the 64 modules here, and the game's 256 built by the compiler before
  the change and after it.
- **Half of the back end's threads while codegen runs** (BackEndPool, below).
- **Codegen's threads kept between its parallel steps** (KeptThreads, below).
- **clang asked for the C import's toolchain beside the lexing and the parsing** (ImportToolchain, below).
- **link.exe's command asked of clang's driver without the objects, beside the back end** (directLinkCommand,
  below).
- **A class lists the member functions that have code in its debug information, and a class template's
  instance's member functions name their declarations** (debugComposite and debugLines, below).

**The back end's threads while codegen runs.** Section 8's timeline has LLVM's threads compile the
program pieces beside codegen's levels 1 and 2, every hardware thread busy, then wait 2 s for the
instances' modules. Codegen's threads are the build's critical path there, and as many back end
threads as the machine has hardware threads took half the machine from them, to finish work early that
nobody waited for. With the back end's width limited until codegen ends (a temporary environment
variable; four builds each in interleaved rounds, medians, kpp's own time without the link):

| back end threads while codegen runs | kpp | of it codegen | LLVM after codegen | program pieces done before codegen's end |
|---:|---:|---:|---:|---:|
| 32 (all) | 11.25 s | 5.64 s | 2.75 s | 119 of 119 |
| 24 | 11.20 s | 5.40 s | 2.72 s | 119 |
| 16 | 10.45 s | 5.02 s | 2.67 s | 119 |
| 12 | 10.95 s | 4.89 s | 2.72 s | 68 to 83 |
| 8 | 11.05 s | 4.71 s | 3.06 s | 37 to 40 |

Codegen is faster the fewer threads the back end has, and at 16 the program pieces are still done
when it ends; below that they are not, and what is left of them is compiled beside the instances'
modules. Every build at 16 (10.20 to 10.70 s) was shorter than every build at 32 (11.20 to 11.50 s).
The pool takes half of the build's threads while codegen runs, all of them after.

A second session with the changes built (six rounds each, in turn, the machine busier): the compiler
before both changes 11.40 s (12.2, 11.2, 11.4, 11.4, 11.1, 11.4), with both 10.60 s (10.8, 11.2, 10.3,
11.1, 10.4, 10.4), in no round the longer of the two; codegen 5.64 s against 5.10 s, LLVM after codegen
2.83 s against 2.72 s. The width alone, live debug values as before: 11.05 s at 20, 11.10 s at 18,
11.05 s at 16, 11.00 s at 14 (where 9 to 35 of the program pieces were left when codegen ended).
Between 14 and 20 these rounds show no difference; half of the threads is in the middle of that.

The other way round does not help: with fewer threads for codegen itself (`--codegen-threads`; the
back end with all 32) the build is longer, kpp's time 11.15 s with 32 or 24, 11.70 s with 16, 12.20 s
with 12, 12.95 s with 8.

**Codegen's threads kept between its steps.** A timeline of the build with a mark at every parallel
step of codegen (temporary timers, the compile's own clock) showed where the back end's threads hurt it
most. Each step (a level's emission, the three parts of collecting the next level, the barrier's
pieces, the homing, the finishing, the assembling: some forty a build, half of them a few milliseconds
of work) started threads of its own, 31 of them with a 1 GB stack each. On an idle machine the last of
them started 5 to 8 ms into its step. With all of the back end's threads compiling beside codegen the
last thread of level 1 started 0.25 to 1.6 s into the level, which so took 1.28 s instead of 0.56 s,
and the scan of the requests after it, 5 ms of work, took 0.4 s: a thread's start waits for the lock
of the process's address space, which the back end's allocator takes all the time as it maps and
unmaps memory. Over a build the last threads' starts added up to 0.9 to 1.8 s with all of the back
end's threads beside codegen and to 0.5 to 1.0 s with half of them.

Codegen's threads are kept from one step to the next while a build's code is generated
(CodegenSession.cpp, KeptThreads): started by the first step that needs them, woken for each step,
joined when codegen ends. Five builds each in turn, with half of the back end's threads while codegen
runs and with all of them (medians; times from the compile's start):

| | a step's own threads, half | kept, half | own, all | kept, all |
|---|---:|---:|---:|---:|
| the last threads' starts, summed over the build | 0.66 s | 0.11 s | 1.35 s | 0.09 s |
| level 1 | 0.56 s | 0.54 s | 1.28 s | 0.72 s |
| the next level collected after it | 0.07 s | 0.08 s | 0.41 s | 0.11 s |
| levels 3 to 16 | 0.97 s | 0.85 s | 0.66 s | 0.82 s |
| codegen's end | 7.19 s | 6.95 s | 7.47 s | 7.19 s |
| the back end's end | 9.76 s | 9.67 s | 10.16 s | 9.80 s |

Codegen ends 0.24 s sooner with half of the back end's threads beside it and 0.29 s with all of them,
the back end 0.1 s and 0.36 s. Half stays the better width with kept threads too (9.67 s against
9.80 s).

The compilers before and after it without the timers, on a quiet machine at night (eight builds each
in turn, medians): codegen 4.43 s against 4.36 s, the shorter in six of the eight rounds; kpp's part
8.75 s for both (8.6 to 9.5 s before, 8.5 to 8.9 s and one of 10.2 s after), the build 11.0 s. The game's
256 objects are the same bytes. On a quiet machine the late starts cost little, and codegen's own time
is 4.4 s where the busier afternoon of the timelines above had 5.2 s: what the change gives a build is
between nothing and 0.1 s, and what it takes away is the delay that other work on the machine could
cause, the 1.6 s above.

**The C import's question to clang, asked early.** An import's cache key holds what clang says of its
toolchain (`clang -x c -E -v`: its version, the target, the directories its `#include <...>`
searches). That is a process of its own, which the import started after the sources were parsed, with
nothing else running. A thread now asks when the sources are read, beside their lexing and parsing, if
one of them has an import (ImportToolchain in Driver/CImport.cpp; a program without one starts no
clang), and the import takes its answer. Six builds each in turn: the import's step 0.197 s before
(0.191 to 0.208 s) and 0.162 s after (0.159 to 0.183 s), the shorter in every round. That is 0.035 s
of every build that parses, less than whole builds resolve (kpp's part 8.6 s and 8.55 s).

**link.exe's command, asked without the objects.** A link that is no LTO's runs link.exe directly with
the clang driver's translation of the link's response file, kept beside the executable. A clean build
has no kept answer, and asked the driver once its last object was written, with nothing else running:
0.065 s from the back end's end to the link's command (below). The driver cannot be asked sooner with
the objects in the response, since it fails on one that is not written yet. It is asked with a linker
option in the objects' place, which it passes on where an object given there goes (the 312 link.exe
arguments of the game's tests are the same asked either way), on a thread beside the back end: 0.004 s.
The kept answer no longer names the objects either, so a compile server's first edit of a file, which
adds an object, asks nobody (it asked again: 0.07 s on a quiet machine, up to 0.5 s on a loaded one).

**The member functions a class's debug information lists.** A class's definition listed every member
function it declares, so that a watch expression can call one (`shape.area()`): the function's
definition names the class's declaration of it. For a class template's instance that never worked. An
instance is emitted from its own copy of the function, and the copy was looked for among the class's
members, which are the functions as written: the definition named no declaration and kept its
parameters in its name, and the debugger had no function for what such a class listed, called or never
instantiated alike (`std::Vector<int32>` in a small program listed 36 names). Each listed member was
three nodes and a type the resolver worked out, and their parameters' types named classes nothing else
did. Now the function as written is looked for, so an instance's member function is its class's like
any other, and a class lists the member functions that have code (a definition names them), its
virtual ones and its static ones: one that nobody calls is never emitted and is not listed.

Measured on the game (kpp-factorio 0c18a3229e): eight clean debug builds each in turn of master
235ab7c14 with the change taken back out and of master 235ab7c14 as it is (b88217f64 is the change; each
with the temporary timers), medians:

| | without it | with it | |
|---|---:|---:|---:|
| the build, from its start to its exit | 10.23 s | 9.90 s | -3.2% |
| kpp's part of it | 8.35 s | 8.10 s | |
| codegen | 4.31 s | 4.17 s | |
| of it the debug classes defined (homeDebugClasses' levels) | 0.183 s | 0.099 s | |
| LLVM after codegen | 2.23 s | 2.11 s | |
| the link | 1.35 s | 1.30 s | |
| the IR | 1,561.9 MB | 1,510.1 MB | -3.3% |
| the objects | 517.4 MB | 503.8 MB | -2.6% |
| the PDB | 274.1 MB | 257.6 MB | -6.0% |
| CPU time of the build's processes | 206.3 s | 199.4 s | -3.3% |

In each of the eight rounds the build with it was the shorter, by 0.09 to 0.39 s (10.09 to 10.44 s
without, 9.82 to 10.35 s with; in the last two rounds other work ran on the machine, 5 to 8% busy before
three of their four builds, and all four took 10.3 to 10.4 s). The classes that only another class's
definition names, defined at the second level, went from 15,383 to 720: the others were named by the
listed member functions' types alone. With codegen ending 0.14 s sooner, 3 or 4 of the 119 program
pieces were not compiled yet at its end at the median, where 1 was before: the back end's width while
codegen runs (half of the threads, above) was measured for the longer codegen.

An earlier form of the change, in which a class template's instance listed only its virtual and static
member functions, was measured the same way against 87b8d9481, together with the link command's change:
the build 10.19 s to 9.76 s, the IR 1,573.4 MB to 1,492.4 MB, the objects 520.9 MB to 506.1 MB, the PDB
274.1 MB to 258.8 MB, the debug classes' levels 0.182 s to 0.055 s. It gave up the calls of instances'
member functions from a watch, which had never worked for another reason (above), and was replaced: what
the final form lists besides, the instances' member functions that have code, is the difference.

**A type's identifier is its key, not its name a second time (2026-11-25).** Every module that names a
class has a declaration of it in its debug information, which finds the class's definition in another
module by its `identifier:` (CodeView's unique name). The identifier was the type's whole spelling,
`std::Vector<std::Pair<uint8, uint16>>`, beside the `name:` that says nearly the same: the name twice
in each of the game's 288,000 class declarations, in the IR, in each object's type records and in the
PDB. Nothing shows an identifier (a debugger names a type by its name), and any text that is the same
in the declaration and the definition does its work: it is now the 16 hex digits of the spelling's
hash, the key the type's debug node already has (`Emitter::debugTypeIdentifier`). Measured on the game
(kpp-factorio 0c18a3229e), eight clean debug builds each way in turn with one compiler (this change on
master 5f3bec056 and three commits of this branch; a temporary switch gave the spellings back),
medians:

| | the spelling | the key | |
|---|---:|---:|---:|
| the build, from its start to its exit | 9.54 s | 9.43 s | -1.1% |
| kpp's part of it | 8.1 s | 8.0 s | |
| codegen | 4.14 s | 4.12 s | |
| LLVM after codegen | 2.09 s | 2.06 s | |
| the link | 1.25 s | 1.20 s | |
| the IR | 1,508.1 MB | 1,495.7 MB | -0.8% |
| the objects | 501.8 MB | 488.4 MB | -2.7% |
| the PDB | 257.5 MB | 254.8 MB | -1.0% |
| CPU time of the build's processes | 202.7 s | 200.7 s | -1.0% |

The builds with the spelling took 9.44 to 9.89 s, those with the key 9.40 to 9.49 s: in seven of the
eight rounds the one with the key was the shorter, by 0.08 to 0.12 s in six of them (0.49 s in the
first, whose build with the spelling was the 9.89 s one; in the sixth the other way, by 0.05 s).
Visual Studio's debugger check passes with it (48 checks:
class-typed values, natvis, member function calls), and run_tests' debug info check now holds each
class's unique name to 16 hex digits, one for its forward references and its definition alike (with the
spelling it fails: "the unique name(s) of game::Shape's records are [game::Shape]").

**Measured and left.**

- **Numbers for local values.** The emitter makes a value's name before it writes the value's
  instruction, and cleanup pads are written aside and placed after the body, so its numbers do not
  rise in the text's order. Renumbering a function's text when it is finished is a pass over all of
  it on codegen's threads, which the build waits for, to save a fortieth of the time of LLVM's, which
  wait themselves: a loss. Numbers written right at once (a name only where the order is not the
  text's) would be free, for about 2% of LLVM's time: 0.06 s of a build. Not done.
- **Parameters read without a load.** 1.7% for 9% of the instructions; and a scalar parameter's read
  must stay a load, or a value changed in the debugger would not be the one the code uses.
- **`zeroext` on a returned `i1`**: 0.7%, inside the noise.
- **Freeing a module later** (while the linker runs): the freeing is 3.9% of the back end's time, and
  the instances' modules, which the build waits for, would have to be kept all at once.

**Left, by what it would give.**

- **The lexer's keyword search**, about 7% of the back end's time. It is LLVM's code: a faster one
  means kpp carrying its own copy of one file of LLVM's (LLLexer.cpp, built against LLVM's public
  headers and linked in place of the library's). A decision, not a change.
- **The verifier**, 4.6% (section 8's question: off by default, on for kpp's own tests).
- **No text at all** between codegen and LLVM (the IR built in memory): the parse is 30% of the back
  end's time, and writing the text is part of codegen's.
- **Long symbol names** (measured 2026-11-25), about 0.2 s of a clean build. A function's name in the IR
  is its whole readable name, `@"std::Vector<std::Pair<uint8, uint16>>::push_back(std::Pair<uint8,
  uint16>&&)"`: 54 bytes on average, 1,579 at most (an `MplVector` of the prototype lists), 14% of the
  text of sixteen modules. The whole game's IR (master 235ab7c14's, 256 modules) was compiled and
  linked as it is and with every quoted global or comdat name of 40 bytes or more replaced by `@h.`
  and 16 hex digits of its hash, the same in every module (MSVC's own names, `??_7...`, kept):

  | | the names | their hashes | |
  |---|---:|---:|---:|
  | the IR's text | 1,512.2 MB | 1,377.8 MB | -8.9% |
  | kpp's back end over every module, a thread each (two runs) | 52.86 s, 52.67 s | 51.67 s, 51.79 s | -2.3%, -1.7% |
  | of the first run: the parse | 17.28 s | 16.92 s | -2.1% |
  | of the first run: the machine code | 31.11 s | 30.45 s | -2.1% |
  | the objects | 503.8 MB | 429.0 MB | -14.9% |
  | the link, from nothing, five each in turn | 1.13 to 1.16 s | 1.03 s each time | -0.11 s |
  | the PDB | 257.6 MB | 233.1 MB | -9.5% |
  | the .ilk | 171.0 MB | 146.5 MB | -14.3% |

  (A quarter of the modules three times over gave -1.6% for the back end, and -1.9% with the names
  from 24 bytes on. The link is link.exe 14.51's, incremental, with the PDB, as the game's debug
  build links; the executable is the same 120.4 MB.) So a seventh of each object is names, and the
  linker reads, hashes and writes them: that is where the time is, 0.11 s; the back end's one second
  of CPU time is a few hundredths of a build's on 32 threads, and what kpp's own assembly of a text 9%
  shorter would give was not measured. The price: the linker's messages, a map file, dumpbin, the PDB's
  public symbols and the stack of a build without debug information would show hashes (a debugger's
  names come from the debug information and stay), so kpp would have to translate them back where it
  shows the linker's words, and a release would need the table beside it. A decision, not a change.

**The clean build, from the study's start to here.** The compiler this study began with (section 8's:
5bdcfe34b of 2026-11-23, kpp 0.2.1738, with the standard library of that commit) against master's
(e6a09cfe4), on the same game (kpp-factorio 0c18a3229e): eight clean debug builds each in turn, the
machine quiet (under 2.3% busy before each), medians:

| | 5bdcfe34b | e6a09cfe4 | |
|---|---:|---:|---:|
| the build, from its start to its exit | 12.39 s | 10.34 s | -16.6% |
| kpp's part of it | 10.1 s | 8.4 s | |
| codegen | 5.35 s | 4.22 s | -21.2% |
| LLVM after codegen | 2.49 s | 2.21 s | -11.1% |
| the link | 1.7 s | 1.4 s | |
| the IR | 1,710.7 MB | 1,573.4 MB | -8.0% |
| CPU time of the build's processes | 236.3 s | 207.9 s | -12.0% |

Every build with master's compiler (10.22 to 10.43 s) is shorter than every one with the older
(12.12 to 12.48 s). Half a second of each is the six build.kpp files described again, only because
the two compilers take turns (the description's key names the compiler): by kpp's own count, which
leaves that out, a build is 9.8 s where it was 11.85 s. Between the two are master's 31 commits of
those two days: sections 8 to 10's changes, and other threads' work.

## Reproduce

All in `C:\kpp-sandbox\scratch\templates\cg` (thread D's scratch, not committed):

| file | what it does |
|---|---|
| `build.cmd OUT [kpp options]` | the game's debug build as run.cmd builds it, from C:\factorio-in-kpp |
| `proto\cgproto.cpp`, `proto\build.cmd` | LLVM through VS's `LLVM-C.dll`; modes `emit`, `spawn`, `tobc`, `home`, `write`; `CGPROTO_PASSES`, `CGPROTO_HOMES`; `-- llvm options` |
| `measure.ps1 -Rounds N -Threads T -Variants spawn:out,inproc:bc,...` | interleaved runs over the module sets `out`, `bc`, `ident`, `home` |
| `homes.py` | each linkonce global's home module (homes.txt) |
| `volume.py DIR` | what a build's IR is made of (section 3) |
| `cpp\ir.cmd`, `cpp\sample.py` | Factorio's C++ to -O0 IR by clang-cl, the sample and the unity files |
| `link.ps1`, `linkab.ps1` | the full and incremental link; the two object sets linked interleaved |
| `sampler\sampler.cpp`, `kpp-pdb.cmd` | the sampling profiler, and kpp with a PDB |
| `paired.ps1`, `determinism.ps1`, `disk.ps1`, `homecheck.py` | paired builds against master; 1/8/32 threads; disk use; step 2's equivalence |
| `timeline.py`, `inflate.py`, `lockprobe.py`, `lockrank.py` | a profile's timeline; thread time at 1 and 32 threads; a temporary lock probe (never committed) |
| `objcompare.py A B`, `objtree.py A B`, `backend-ab.ps1`, `backend-four.ps1`, `gnu-ab.ps1`, `backendprobe.py` | two builds' objects compared (the COFF timestamp masked), flat or by tree; the back end in kpp against clang, interleaved; windows-gnu built both ways (end-to-end, kppsite, spec); a temporary probe of each module's CPU time per back-end phase (never committed) |
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
