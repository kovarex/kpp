# Compile speed on the Factorio source

What kppc costs on the converted Factorio source today, phase by phase, and what a full build is
projected to cost once the source compiles. Measured 2026-10-28 on PC-PRACOVNA (Ryzen 9 9950X,
32 threads, 128 GB), kppc Release build at commit `de690f4`. The C++ numbers it is compared with
are in factorio-build-baseline.md (debug clean 62 s, one-line edit in an ordinary header 16 s,
one-line edit in a header the PCH includes 55 s).

Reproduce: run `programs\factorio\convert.cmd`, copy `programs\factorio\kpp\` somewhere stable
(another thread running convert.cmd rewrites it mid-measurement, which once left a kppc spinning
for 10 minutes on half-written input), then

```
powershell -ExecutionPolicy Bypass -File programs\factorio\measure-speed.ps1 -Tree <the copy>
```

## A clean build, phase by phase, and what scales (2026-10-30, night)

A clean build of the game as run.cmd builds it, in a new process and a new output directory (no
object cache, a full link), on a frozen copy of the converted tree (C:\factorio-in-kpp d9080751),
master 089dcce5, the same work at every thread count (`--modules 64`):
scratch\resolver\clean.ps1 -Threads N. The -j 1 time is each phase's work.

| phase | -j 1 | -j 2 | -j 4 | -j 8 | -j 16 | -j 32 | speedup at 32 | efficiency |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read, lex, imports (cached), parse, index | 1.77 s | 0.97 | 0.78 | 0.59 | 0.55 | 0.53 | 3.3x | 10% |
| resolve | 7.61 s | 4.04 | 2.18 | 1.33 | 1.27 | 1.31 | 5.8x | 18% |
| codegen | 23.36 s | 12.83 | 8.35 | 6.47 | 6.34 | 6.70 | 3.5x | 11% |
| clang -O0, 64 modules | 39.35 s | 19.84 | 10.47 | 5.86 | 4.89 | 4.60 | 8.6x | 27% |
| link (full) | 1.66 s | 1.64 | 1.65 | 1.64 | 1.59 | 1.66 | 1x | serial |
| wall | 74.3 s | 40.0 | 23.8 | 16.3 | 15.1 | 15.2 | 4.9x | |

(The function-instance checks' CPU, which the instantiate line reports, grows from 5.3 s at -j 1
to 14.6 s at -j 32 while their wall time stays: contention.) Resolve and codegen stop scaling at 8
threads; clang is the one phase still gaining. Why, phase by phase:

- **Resolve: one file is the critical path.** Every file but one is done 0.40 s into the resolve;
  `ValueToWidget.kpp` (728 KB) is walked from 0.05 to 1.32 s, the whole phase (a per-file timeline,
  temporary). Its time is not its instances (none is checked inline) but 18 template functions'
  own generic walks, 50 to 100 ms each, and in them 52 overload resolutions of
  `ValueToWidget::make(*value)` against an overload set of 455 `make`s, 5 to 95 ms each, 1.21 s in
  all (timed around `pickOverload`, temporary): the argument's type is no template parameter the
  walk knows to be dependent, so every candidate, templates deduced, is ranked. Walking a file's
  templates as tasks of their own took the phase to 0.84 s, but it is not sound: a later
  namespace-scope static_assert evaluates a class template member whose generic walk has to have
  run (two spec snippets failed), and the statistics depended on the order (the edit test's
  "deduction" count, 17 against 18). Not landed.
- **Codegen** does 23.4 s of work and is no faster than 6.3 s from 8 threads on: serial or
  serialised parts (to be profiled by the codegen thread).
- **Clang** is bounded by its largest module: module 28, 37 MB of the 739 (the median is 10.8 MB),
  takes 2.92 s alone and about 4.5 s among 32 others (a median module 0.51 s). Starting the largest
  modules first (measured, 4 rounds interleaved: 4.54 to 4.66 s against 4.61 to 4.71 s) changes
  nothing: it starts in the first wave already.
- **The link**, full, 1.66 s; the edit loop's incremental relink is 0.5 s.

**The levers, by size** (the endgame is 4 to 7.5 s for a debug clean build):

| lever | today at -j 32 | estimate | owner |
|---|---:|---:|---|
| codegen scaling past 8 threads (23 s of work) | 6.7 s | 1.5 to 2.5 s | codegen thread |
| clang: modules of balanced size (split module 28's content; more, smaller modules) | 4.6 s | 1.5 to 2 s | codegen thread |
| resolve: the dependent overload resolutions in ValueToWidget's generic walks | 1.31 s | 0.4 s (next: LuaHelper.kpp, 0.29 s) | templates thread |
| the link: lld for a clean build (1.0 s, no PDB) or the incremental first link | 1.66 s | 1.0 s | codegen thread |
| the front end before resolve (index's out-of-line attach, 0.16 s serial) | 0.53 s | 0.4 s | resolver thread |

Together about 4.5 to 6 s, in the endgame's range. The resolver thread's own share is small (the
front end before resolve); its resolve lever is the templates thread's overload code.

The whole game is built the way `programs\factorio\run.cmd` builds it, from a frozen copy of the
converted tree: imports, front end, codegen into 64 modules, clang -O0 per module, and the link
(MSVC link.exe through clang, with `/DEBUG /MAP` for the crash report's names, `/OPT:REF`). The
machine was shared with other threads' builds the whole time (60 to 100% busy before most runs),
so every number is a range over runs and the best of them is the closest to an idle machine.
Master between aecd79b and c088f0d. Wall time is from start to process exit.

| | clean (no objects) | no change | one-line edit, leaf body | one-line edit, widely used inline function | layout of a widely used class |
|---|---:|---:|---:|---:|---:|
| wall | 24 to 36 s | 17 to 22 s | 18 to 20 s | 19 to 29 s | 25 to 32 s |
| read + imports + parse + index | 0.4 to 0.8 s | 0.4 to 0.6 s | same | same | same |
| resolve | 1.3 to 2.3 s | 1.3 to 2.1 s | same | same | same |
| codegen | 11 to 16 s | 11 to 15 s | 11 to 12 s | 12 to 16 s | 11 to 13 s |
| clang -O0 (modules compiled) | 8 to 10 s (64) | 0.1 s (0) | 0.9 s (1 or 2) | 1.3 to 3.3 s (1) | 8 to 10 s (64) |
| link | 3.4 to 6.4 s | 3.4 to 3.7 s | 3.5 to 4.4 s | 3.4 to 6.9 s | 3.8 to 6.4 s |

The edits: a statement added to `BuildEntityAchievement::setup`, the operands of
`MapPosition::operator+` swapped, a field added to `Entity`. The C++ build: 62 s clean, 16 s for
an ordinary one-line edit, 55 s for one in a header the PCH includes (factorio-build-baseline.md).

Until c088f0d no rebuild reused a single object: the IR named local classes and some struct types
by the order they were reached, so every module's text changed from run to run ("0 unchanged");
the codegen thread fixed that (408a881, 9102b77, c088f0d), and now the no-change rebuild compiles
nothing and an edit compiles only the modules whose text changed. What an edit costs now is the
front end and codegen run again in full (13 to 17 s) plus the link; the endgame (2.5 to 4.5 s for a
one-line edit) needs both of them to stop being proportional to the program:

- **Codegen, 11 to 16 s**, measured inside with temporary timers: about 7 s emitting functions on
  all threads, 0.4 s finishing, **4.5 s assembling the 64 module texts on one thread**, and about
  3 s freeing its own state before it returns. Single-threaded (`--codegen-threads 1`) it is 23 s,
  so the parallel part scales only 1.7x so far.
- **The IR is 1.75 GB in 64 modules against 0.6 GB in one**, and **843 MB of the 1.75 GB are
  destructor helpers** (`kpp.dtor.*`): 2,598 distinct helpers defined 170 k times, in every module
  that destroys such an object (13 MB in the one-module build). Defined once in a home module, as
  instantiations are, the IR roughly halves, and with it clang's work and the serial assembly.
- **One function is a quarter of clang's critical path.**
  `ControlSettings::ControlSettings(const PropertyTree&, KeyboardLayout)` is 875 k lines of IR with
  482 k calls of `String::~String`: every cleanup path destroys again every member built before it,
  so the cleanups grow with the square of the members. Its module is 95 MB (the median module is
  24 MB) and takes clang 7.5 s alone, most of the clean build's 8 to 10 s of clang. Chained cleanups
  (each landing pad continuing into the previous one's) make it linear. (Reported to the codegen
  thread with the two items above.)
- **clang per module:** a median module (24 MB) compiles in 1.3 to 1.6 s at -O0 on a loaded
  machine. That is the floor of an edit's compile with 64 modules; smaller modules (256, once the
  destructor helpers are no longer copied into each) would put it near 0.3 to 0.4 s.
- **The link, 3.4 to 6.9 s,** is the largest single item of an edit once the rest is incremental.
  The same objects linked four ways on the loaded machine: link.exe as today 9.8 to 13.1 s,
  link.exe without `/DEBUG /MAP` 6.0 to 8.5 s, lld without them 2.4 to 4.8 s, lld with `/DEBUG`
  15.7 to 21.4 s. An incremental link (link.exe `/INCREMENTAL`, which `/OPT:REF` rules out) is the
  next thing to measure for the edit loop; the names the crash report needs could come from the
  map file alone. (Done: an incremental relink is 0.5 to 0.6 s with the PDB, "The link in the edit
  loop" below.)
- **Process exit freed everything for about 5 s** after the link: aecd79b ends the process without
  freeing (paired, three alternating runs: last output line to exit 4.6, 5.3, 4.8 s -> 0.3 s each;
  wall median 30.3 -> 26.8 s). Codegen's own 3 s of freeing inside `generate` is the same fix there.
- **Imports, 0.65 to 1.4 s before:** reading the cached import and checking the headers' stamps
  took 0.02 s; lexing its 10 MB on one thread 0.3 to 0.6 s (34 MB/s: 119 k distinct identifiers to
  intern); parsing 0.07 s. ecca9df lexes it in 256 KB pieces on all threads (tokens identical to
  one lex: 1,287,194 tokens, 0 mismatches): imports **0.66 to 0.72 s -> 0.16 to 0.22 s**, four
  paired runs. What is left: the parse of the import, 0.07 s on one thread, and the pieces'
  interning.

Memory: the front end peaks at 1.6 GB of working set, front end and one-module codegen at 2.0 GB,
so a process that keeps all of it between builds fits easily.

## Design: inlining trivial functions in kppc (proposed 2026-10-29, for the coordinator's OK)

**Why.** A debug build compiles its 64 modules at -O0, where clang inlines only `alwaysinline`
functions and only within a module. A template instance lands in the module its name hashes to,
a plain function in its file's module, so nearly every call to a library accessor crosses
modules and stays a call. Reading through a UniquePtr costs 2.5 to 2.7 times a raw pointer in a
debug build (unique-pointer-research.md 1.5): with `[[forceinline]]` it was still 2.60x, and only
`--modules 1` brought it down to 1.20x. The same holds for every small accessor. In the game's IR,
249,485 of its 2,166,822 call sites (11.5%) call a trivial function: one basic block of at most
12 instructions and no call (`python compiler/bench/trivial_calls.py headline.ll`, master 819c0fb).
The most called are `forward` 19,771 and `move` 16,574, the two owners' `operator->` 9,931 and
8,983, Vector's `end`/`begin` 5,577/5,343, `[]` 3,993 and `size` 3,861, and the NamedBool and ID
constructors 5,577 and 5,184. design.md section 2 chose this fix on 2026-10-28 as option (4):
kppc inlines them itself when it emits IR. ThinLTO, option (1), already serves release builds.

**What qualifies.** It is decided once per function instance, from its typed body, and cached in
the shared state. The decision depends on nothing but the body, so the IR stays deterministic.
- `move` and `forward` always: they are identity casts, a pointer in and the same pointer out.
  codegen already does that in one narrow case. Step 1 makes it every call.
- A direct call: no virtual dispatch, no function pointer. The callee is not `[[noinline]]`, not
  imported from C, not variadic, and not on the current inlining stack (depth at most 3).
- Its parameters are scalars, pointers or references, and so is its result, or it returns void.
  A class by value needs a copy and a destruction, and a class result needs the caller's slot.
- Its body is one statement, `return E;` or `E;`, with no locals.
- E holds only these: parameters, `this`, fields reached from them (`this->items`,
  `this->p->x`), constants, built-in operators on scalars and pointers, built-in subscripts,
  scalar and pointer casts, and calls to functions that qualify themselves. It holds no
  overloaded operator that does not qualify, no temporary that needs destroying, and nothing
  that can throw.
- Step 3 adds constructors whose body is empty and whose member initializers copy parameters or
  constants into scalar members, of a class with no base or member that needs constructing
  (NamedBool, ID).

**Where.** In codegen's callFunction, which every direct call goes through, before the call is
emitted. The callee's expression is emitted in the caller as its default arguments are emitted
today: under the callee's template arguments and class (ArgsScope, currentClassTy), and with its
names looked up in the callee's own context (typingContext). The arguments are evaluated first,
in order, exactly once, then bound to the parameters as values, a reference parameter as its
address. `this` is bound to the object's address, adjusted to the declaring base as for a call.
No alloca and no store are needed, since a parameter that qualifies is only read. The callee's
function is still emitted when its address is taken or a call to it does not inline. A
`--no-inline` switch keeps every call, for comparisons and to bisect a suspected miscompile.

**With `--release` (ThinLTO).** LLVM inlines these anyway, importing across modules. kppc's
expansion comes before LLVM in both builds, so the IR has one shape and there is one code path.
LLVM has less to import and inline, which should cost a little less link time; run time should
be the same within noise. Both are measured below.

**With debug information (Visual Studio, later).** No debug information is emitted today, since
the PDB carries function names only, so an inlined accessor drops out of a crash stack, as it
does in a release build. When kppc emits line information, the instructions of an inlined body
carry the callee's own locations, with an `inlinedAt` pointing at the call: the DILocation
chain LLVM's inliner writes. The PDB then gets S_INLINESITE records, which Visual Studio shows
as a frame of the trivial function. Stepping into the call enters the accessor's line, and the
call stack and the locals window show it, as for MSVC's own inlined code. The inlining keeps
the callee's nodes for this, the same nodes that give an error inside an inlined body the
callee's line. `--no-inline` remains for a build that wants real calls in the debugger.

**How to measure each step** (A/B against a base built at the same master):
1. Static: trivial_calls.py on the headline's IR. Its 249,485 call sites should fall by those the
   rule accepts, and the IR bytes and the function count are reported next to them.
2. A micro-benchmark: `compiler/bench/uniqueptr/deref.kpp` in a debug build. The read is 2.5 to
   2.7x a raw pointer today and should come close to the 1.2x of `--modules 1`.
3. Containers: `compiler/bench/stdlib/run.cmd` in debug and release builds, against std
   (stdlib-performance.md's table).
4. Compile time: the headline's codegen seconds and wall time, and the release link. Inlining
   adds work at call sites, and the emitted functions do not get fewer.
5. The game: a menu simulation's update time in a debug build,
   `factorio.exe --benchmark <data/base/menu-simulations/X.zip> --benchmark-ticks N`, with and
   without `--no-inline`.
6. Correctness:
   - a spec of inlined accessors (argument order and side effects, reference parameters, `this`
     through a second base, a call that does not inline next to one that does);
   - the spec suite and the determinism check;
   - `--dump-data` still gives the C++ checksums.

**Order.** Step 1 is `move` and `forward` (over 44,000 call sites with all their instances, which then go too).
Step 2 is one-expression getters and setters. Step 3 is trivial constructors. Each lands
measured. Codegen owns callFunction, so the work is codegen's or done with its review.

**Measured** on one converted tree with the commands above, each step against a base built at
its merge-base (the steps before it) and against `--no-inline`; game builds and benchmark runs
interleaved between the builds compared, since the machine is shared.
- Step 2 (1f2c533), against `--no-inline`: deref.kpp's read 1.17x a raw pointer in a debug build
  (3.02x), the debug containers up to 0.76x, 32,474 fewer functions, the headline's codegen 5.5 s
  (7.3 s), the game's build within noise, the menu simulation's fastest tick 0.33 to 0.38 ms
  (0.46 to 0.51 ms).
- Step 3, against step 2 (base e4c218a): the headline inlines 41,331 constructor calls of 6,260
  constructors (ID<QualityPrototype> 1,553, MapTick 1,317, ID<ItemPrototype> 658, Vector2 578,
  agui::GuiDirection 532); 6,390 constructors are no longer emitted (246,491 -> 240,101
  functions; 278,965 with `--no-inline`), and 5,633 calls left the cleanup pads of the invokes the
  inlined calls were. The IR grows 553.6 -> 556.4 MB. Call sites to a trivial function
  (trivial_calls.py) 89,969 -> 65,138 (249,602 with `--no-inline`). Debug containers, interleaved
  best of 7 against step 2: vector.push int 0.93x, string.find char 0.77x, hash.int insert 0.95x,
  fasthash.String lookup 0.95x, the rest within 2% except string.concat 1.07x, whose IR did not
  change (code placement); release within 2%; deref.kpp unchanged (1.29x against 1.27x). Game
  builds, two interleaved rounds: codegen 6.81 and 6.65 s against 6.71 and 6.78 s (7.27 and
  7.42 s with `--no-inline`), clang -O0 4.63 s against 4.53 s in the quiet round; the link (3.7 to
  12.5 s) varied with the game directory more than with the compiler. The menu simulation
  (train-junction, 300 ticks, three runs of each build) stayed within its run-to-run noise
  (average 0.70 to 0.90 ms for all three). `--no-inline`'s IR is the base's byte for byte,
  `--dump-data` gives the C++ checksums, and the unit tests package builds with the same 56 errors.

## Design: the persistent compiler (approved 2026-10-29; step 1 landed)

Goal: a one-line edit costs what the edit touches, not what the program is, with output
byte-identical to a clean build of the same sources. Today's edit costs 13 to 17 s of front end
and codegen before clang and the link; the target is under 0.2 s for an edit inside a function
body, so that clang's one module and the link are what is left.

**The process.** `kppc` with the same arguments as today becomes a thin client: it finds a server
for this (kppc binary, working directory, arguments) by a named pipe whose name is a hash of the
three, starts one (`kppc --serve ...`, detached) if there is none, sends the request and streams
back stdout, stderr and the exit code. `--no-server` builds in-process as today. The server holds
everything a build makes: files (text, tokens, syntax trees), the symbol table, the global index,
the resolved program (types on the nodes, bound declarations, class and function instances),
codegen's per-function results, the module texts and their hashes. It exits after an idle hour,
when its binary changes, or on `kppc --stop-server`. Anything the server cannot handle
incrementally (a new or removed file, a changed option, stdlib or C headers changed, an error it
did not expect) is a full rebuild inside the server, which is never slower than today.

**What changed.** Each build stats every input (4,134 files: a few milliseconds; a directory
watcher later) and re-reads files whose size or time changed; a file whose content hash is
unchanged is unchanged. A changed file is lexed and parsed again (under 1 ms for a typical file),
and its new tree is compared with the old one declaration by declaration, by a hash of each
declaration's tokens with its body's tokens hashed apart. Every change is one of two kinds:

- **Body-only:** only the bodies of functions changed, and none of those bodies can change
  anything outside itself: not a template or a member of one, not a function whose return type is
  deduced, not constexpr/consteval, no local class whose name escapes. This is the common edit,
  and the fast path.
- **Interface:** anything else (a signature, a class's members or bases, a template's body, a
  global's initializer, a new or removed declaration, a using).

**The fast path.** The new body's subtree is moved into the existing FunctionDecl, so every
pointer the rest of the program holds (to the declaration, its parameters, its class) stays
valid; the tokens after the edit shift by a known count and offset, and one walk over that
file's tree moves their indices (the tree of one file: microseconds). Then:

1. The resolver checks that one body again (the walk of one function in its file's scope; the
   per-file state it needs, usings and the file's scopes, is kept), which claims any new
   instances as today; the instance tasks run as in a clean build.
2. Instances nobody requests any more are dropped: each request is recorded with its requester
   (body or instance), and after the check the instances are marked from the program's own
   functions; what is not reached is out of the program, as it would not exist in a clean build.
3. Codegen emits that function again, and the functions of the new instances. Each function's
   result is kept: its IR text, the functions and globals it asked for, the layouts it used. The
   set of functions emitted is the closure of the requests from the program's functions, so it
   is recomputed from the kept request lists without emitting anything else.
4. The module of each changed function is assembled again from the kept texts (one module, not
   64, and never on one thread for all), hashed, compiled by clang if its hash changed, linked.

Expected for a leaf body edit: stat and re-parse 0.01 s, check 0.01 s, emit and assemble one
module 0.05 to 0.1 s, then clang on one module (0.3 to 1.5 s by module size) and the link.

**Interface changes.** The resolver records, per function body and per instance, what it looked
up: each name as (scope, identifier), including the scopes that came up empty (a new declaration
of that name could change the result, and with order-independent symbols it can come from any
file), the classes whose members it searched, and the declarations whose definitions it used
(a class's layout facts, a template's body, a deduced return type, a constant's value). Codegen
records the layouts each function used. A changed declaration invalidates what recorded it or
its name; invalidated bodies are checked again and invalidated instances rebuilt, transitively,
and then everything continues as on the fast path. Declarations themselves change identity only
when their file's declaration list changes; then that file is indexed again (its entries
removed from the global index, the new ones inserted) and everything that recorded a name it
declares or declared is checked again. A changed class layout invalidates every function that
used it, so the layout edit above is close to a clean build in codegen and clang (C++: 55 s);
that is expected, and the target there is a clean build's speed, not the fast path's.

**Correctness, checked rather than trusted.**
- Every incremental build can be checked against a clean one: `--verify-incremental` builds
  clean in the same server after the incremental build and compares stdout, stderr and every
  module's IR byte for byte, reporting the first difference.
- A new test (tests/incremental.ps1, in run_tests) applies a fixed series of edits to the test
  programs, and when present to the headline tree (body edits in 50 functions chosen by a fixed
  seed, a signature change, a new overload in another file, a field added to a class, a file
  added and removed), each through the server and with `--verify-incremental`.
- The determinism check stays as it is: the clean output does not depend on threads or timing,
  and the incremental output must equal it.

**Order of work**, each step landed and measured on its own:

1. The server and client, with a full rebuild for every request. No speed gained yet, except the
   process start and the C import kept in memory; it fixes the protocol, the server's lifetime,
   and `--verify-incremental` (trivially true at this step).
2. Recording the dependencies (resolver lookups, instance requesters, codegen's per-function
   results). Measured: the cost to a clean build, which must stay within a few percent.
3. The fast path for body-only edits, with the incremental test's body edits.
4. Interface edits through the recorded dependencies, starting with the cheap and common ones
   (a new overload, a changed signature), the rest still a full rebuild in the server.
5. Parallel module assembly and smaller modules (with codegen), and an incremental link.

**Decided (review, 2026-10-29).** The server is opt-in (`--server`) until interface edits work
(step 4); the client starts a server itself, which exits after an idle hour. Anything the server
cannot prove local is a full rebuild inside it, `--verify-incremental` must be byte-identical to
a clean build, the incremental test covers at least a body, a signature and a layout edit and a
new file, and a server never builds with state from another kppc binary, stdlib, flags or input
set (they are in its key, or hashed and checked per build).

### The compile server: protocol (step 1, as built)

- `kppc --server ARGS` is a client; `kppc ARGS` without it builds in its own process as before.
  `kppc --stop-server ARGS` stops the server of ARGS.
- **Key:** FNV-1a of the kppc binary's path, size and write time, the working directory
  (lowercased) and the arguments. One server per key, on the named pipe
  `\\.\pipe\kppc-server-KEY` (one instance, local clients only). Another binary, directory or
  argument list is another server, so a server never serves a build it did not start for; the
  files' contents and the stdlib are read again on every build at step 1, and from step 3 on are
  checked by hash on every build.
- **Starting:** a client that finds no pipe copies kppc to `%TEMP%\kppc-server\KEY.exe` (Windows
  locks a running executable, and kppc must stay rebuildable) and starts the copy as
  `KEY.exe --serve KEY` in a new process group with a console of its own and no window
  (CREATE_NO_WINDOW), in the client's directory, inheriting no handles; it then waits
  up to 20 s for the pipe. (It was started without a console, DETACHED_PROCESS, until a small
  program's edit through the server measured slower than a new process: every cmd, clang and link a
  build starts was then given a console of its own, about 30 ms each. A 10-line program, a body
  edit per round, 10 interleaved rounds (C:\kpp-sandbox\scratch\resolver\small): master d98f13b9's server 186 to
  194 ms, with the console 116 to 128 ms; a new process 146 to 157 ms.) A second server for a key fails to create the pipe
  (FILE_FLAG_FIRST_PIPE_INSTANCE) and exits, so racing clients share one. A client that reaches no
  server, or loses it before the answer, says so on stderr and builds in its own process.
- **Request:** `KPS1`, a u32 kind (1 build, 2 stop); for a build the kppc path the client ran
  (argv[0]: the stdlib is found next to it, not next to the copy), the working directory, the
  arguments (u32 count, then each) and the environment block (UTF-16, as
  GetEnvironmentStringsW returns it). A string is a u32 byte count and the bytes. The server sets
  the directory and replaces its environment with the client's (TMP holds the C import cache, LIB
  and INCLUDE are clang's link's, KPP_CLANG...).
- **Answer:** frames of a u8 type (1 stdout, 2 stderr, 3 exit code, 4 the server's process id,
  sent first), a u32 count and the bytes, ending with the exit code's frame. The build's stdout and stderr (the CRT streams, descriptors
  1 and 2, and the standard handles clang inherits through system()) are pipes that two threads
  forward as frames, so clang's own messages reach the client too.
- **One build at a time,** on a new thread with the executable's 8 MB stack, so that thread-local
  state (the resolver's, the evaluator's) starts as in a new process. When the build is done
  (where a process now exits without freeing), the server sends the exit code and lets the client
  go, then frees what the build made; a client arriving meanwhile waits for the pipe.
- **Lifetime:** the server exits after an hour without a request, on a stop request, or when it
  crashes. It logs each of these, and each build's start and exit code, to
  `%TEMP%\kppc-server\KEY.log` (a crash's exception code and address from its crash handler).
  The processes a build starts are in a job that ends with the server (kill on close), so nothing
  of a dead server's build goes on writing. A client that loses its server before the exit code
  waits for the server's process to end (ending it after 30 s), says so on stderr with the read
  error and the log's last line, and builds in its own process. (A run_tests flake of the first
  version: the fallback's link found the executable still locked, LNK1104, by the server's link;
  checked since by killing a server in the middle of a full-game build: the client waited, said
  why, and built the game itself.) A tool that waits for the client's whole
  process tree (PowerShell's `Start-Process -Wait`, a job object without breakaway) waits for the
  server too; the client asks for breakaway from its job and falls back to staying in it.
- **Checked by** tests/incremental.ps1 (in run_tests): a copy of tests/incremental is built through
  one server and by a new process after each edit (a body, a signature, a class's layout, a new
  file, the file removed); stdout, stderr (timings masked) and the IR must be identical and the
  server's program must print what the step expects.

Step 1's numbers (full game as run.cmd builds it, frozen tree, no-change rebuild, three
alternating pairs on the shared machine): fresh process 19.4, 23.1, 19.2 s; through the server
20.4, 24.4, 18.9 s. The same, as expected: the server still builds everything. stdout and stderr
equal a fresh process's apart from timings. Two of the six runs, fresh and through the server
alike, compiled one or two modules again: a temporary's number in one function differs from run
to run (`Vector<...ChunkSet...>::emplace_back`, reported to the codegen thread); the object cache
recompiles those modules, the output is unaffected otherwise.

### Step 2: the dependencies recorded (as built)

A **unit** is one piece of the resolve whose result is kept: a function's or a global variable's
walk outside any other unit (its body, its initializer), a declaration's declared type (what
`declaredTypeOf` caches on the declaration), a function or member instance's walk (its
InstanceRecord), a class instance's check, and a scope's relations (its bases, using-directives
and using-enums). Each records, while it runs, into a list of its own (`RecordInto`, a thread-local
pointer; nested units record into theirs, so that what a cached result's computation read belongs
to that result, not to whoever computed it first):

- **names:** every symbol looked up in any scope, found or not (`Scope::find`, the one place every
  lookup passes). By symbol alone: invalidating by (scope, symbol) would be finer, but recording
  the scopes too cost 1.3 -> 2.1 s of resolve at 32 threads. A lookup visiting a chain of scopes
  is recorded once.
- **uses:** the declarations whose declared types it read (cached or not), the scopes whose
  relations it used, the instances it requested (claimed, built or not).

Several threads may record into one unit (a declared type two threads compute at once): each into
its own part, appended to the unit under a small lock when its part ends; the lists are deduplicated
when read, not when recorded (sorting 10 M entries as they arrive cost about a second of resolve).

Only a compile server's builds record (`GlobalScope::recordDependencies`, on when `servingBuild()`;
`KPP_DEPENDENCY_STATS=1` records in any build and reports the counts): a build in its own process
would only pay for them. Cost, master and the branch run at the same time (so that both see the
same load from the rest of the machine), front end at `-j 1`, frozen tree:

| | resolve, branch / master (median of the pairs) |
|---|---:|
| recording off (a build in its own process) | 1.011 (5 pairs: 0.995 to 1.033) |
| recording (a server's build) | 1.108 (4 pairs) |

Recorded on the headline tree: 264 k walks, 348 k declared types, 389 k instances, 2,972 scopes'
relations; 3.8 M names and 6.0 M uses (before deduplication, after the repeats dropped as they
come). Class instances record nothing of their own yet (their member bodies are instances, which
do). The counts vary by a few hundred out of millions from run to run, as the instance requests
do: probes and partial answers on the instance being built reach different reads. A unit may thus
record more than it needs in one run than in another, which costs invalidations, never
correctness; step 4 checks every incremental build against a clean one (`--verify-incremental`)
before relying on the recorded uses.

### Step 3 in three pieces, and what 3c needs from codegen

- **3a** (resolver thread, landed): a request whose inputs are all as the previous build's is
  answered with that build's stdout, stderr and exit code, nothing built. The inputs: every
  source file's path and text (the stdlib's and the configuration's included), the environment
  (TMP, which holds the C import cache, LIB, KPP_CLANG...), the stamps of the C headers the import
  read (headersUnchanged), and the outputs (`--exe`, `--emit-llvm`) at the size and time the build
  left them; the arguments, the directory and the kppc binary are the server's key already.
  `--verify-incremental` (not part of the key) then builds from scratch in the same server, its
  output into files, and compares: stdout and stderr with timings, rates, wait counts, the
  interned types' count, `(cached)` and the object cache's "N unchanged" masked, the exit code, and
  what was written (the IR file's bytes, or an executable's module hashes; the executable carries
  the link's time). A difference is an error naming the first differing line, also in the server's
  log. The server answers a build when it is done and frees it on its own thread, serving the next
  request meanwhile (freeing the full game took about 10 s, and the next request waited for it).
  Full game as run.cmd builds it, frozen tree: a build in its own process 17.6 s; through the
  server, the first 21.5 s, then no-change rebuilds **0.17 s and 0.16 s**; with
  `--verify-incremental` 34 s (the replay and a build from scratch), "the same as from scratch".
  tests/incremental.ps1 repeats each of its six steps through the server with
  `--verify-incremental` and checks in the server's log that all six were replays, verified.
- **3b** (resolver thread, landed): the server keeps the whole build (`Build` in main.cpp: files,
  tokens, trees, the symbol table, the resolved program, the statistics; the report lines it cannot
  recompute) instead of freeing it, and a request whose files changed inside function bodies only
  is built from it. How:
  1. **Which bodies changed.** Each changed file is lexed and parsed anew; its function bodies
     (`{` to the matching `}`, in namespaces, classes and enums) are paired with the kept file's in
     the text's order; outside the bodies the two token streams must be the same token for token,
     and a body that differs must be eligible: not a template's or a class template's member, not
     constexpr or consteval, no `auto` or `decltype` before it (a deduced return type types callers
     elsewhere). The file must have no parse errors and no checker errors in the kept build.
  2. **The graft.** The kept file takes the new version's tokens and text. Every node it keeps is
     pointed at them: a diff of the two token streams (Myers', between the common prefix and
     suffix; given up past 4,000 edits) maps each old token to its new place, and a walk over every
     node of the file (`forEachNode`, ast_walk.cpp, which follows what InstanceCopier copies)
     rewrites each token index a node holds (`remapNodeTokens`). Each changed function then takes
     its body from the new parse, whose arena the kept build keeps (`Build::grafts`); the body's
     declarations and literals are made the kept file's, and their parent the kept function. The
     declaration objects, which the rest of the program points at, stay; the kept instance
     statistics' places in the file move with its tokens. A new body with a `(name) op x` cast to
     rewrite (finishIndex's work) is not grafted. (The first version appended the new bodies'
     tokens after the kept ones and parsed them there: a statistic's "first at" could then fall
     out of the text's order, and the new bodies' declarations had no parent. The walk and the
     token map are what step 4 builds on.)
  3. **The check.** The changed files are walked again (their recorded reads forgotten first). The
     result is used only if the walk made no instance and reused none that only codegen had made
     (`late` records) and every instance the old bodies requested and the new ones do not is still
     requested by some other unit (step 2's recorded uses). Otherwise the request is built from
     scratch, and the kept build (changed by then) is dropped. A crash in the server now logs its
     stack (module offsets, and names when kppc has a PDB): the graft's first version crashed in
     codegen, a literal still naming the new parse's file, found that way.
  4. **The reports** are what a build from scratch prints: the lexer's counts from lexing every
     file again into a symbol table of its own (the kept one holds earlier versions' identifiers
     too, 0.07 s), the parse counts the new parse gave, the resolve's statistics from the kept
     per-file statistics with the changed files' replaced and the instance statistics as they were
     at the end of the resolve (codegen instantiates more afterwards). Then codegen, in full.
  `--verify-incremental` checks these builds as it checks replays. The edit test has two body
  edits of one file in a row (the second through the kept token order) and checks in the server's
  log that both were built this way and verified. Full game, frozen tree, through one server, each
  edit verified "the same as from scratch": a statement added to `BuildEntityAchievement::setup`,
  that file back, two conditions swapped in the same function, and back. Times without
  `--verify-incremental`: **resolve 0.06 to 0.14 s** (1.3 to 2 s in a build from scratch), the
  wall time 17.5 to 26 s, now all codegen (10 to 16.5 s), clang (one module, 1 s) and the link
  (4.6 to 5.4 s): what 3c and the link are for. A body edit of `MapPosition::operator+` is not
  eligible (constexpr) and was built from scratch, as expected.

### Step 4: interface edits (first kind landed: whole functions)

**What edits there are.** The game's own history (C:\Factorio, read only, scratchpad
editkinds.ps1 and declkinds.ps1): of the last 400 commits, 360 change C++ sources; 37% change
function bodies only (step 3b's kind), 56% change declarations, 7% add or remove files. Of the
changed lines outside function bodies, function declarations are 28% and function definition heads
12%, data members and variables 9%, class heads 3%, template lines 1% (braces and other lines the
rest). So the first kind is whole functions: added, removed, or changed in their signature.

**Through the server before step 4** (full game, frozen tree, machine loaded): a body edit
resolved in 0.06 s; a new member function 2.2 to 2.4 s, a signature 2.6 to 4.0 s, a class layout
3.3 to 4.4 s, a template body 3.1 to 3.2 s, a constexpr body 3.9 s, a new file 3.6 s: all built
from scratch.

**How whole functions are taken (landed).** A changed file that is not body-only is compared with
the kept version through the token map: a kept function whose tokens all have places side by side
at a new function's is the same; the others are removed, the new functions not paired are added,
and every token gone or new must be a removed or added function's (anything else changed: from
scratch). Each must be a plain function: no template or template's member, not virtual, no
constructor, destructor, operator or conversion, not constexpr, no deduced return type, no class or
enumeration in its body (what reads such functions without a lookup: layouts, vtables, traits). Then:
- the removed functions are taken out of the index (`GlobalScope::unindexFunction`: their scope's
  entry, or their class's out-of-line definitions) and their walks' records dropped;
- the file's kept nodes are pointed at the new tokens (as in 3b), the added functions grafted into
  the kept declarations they belong to (found by the token map; member lists rebuilt in token
  order), and indexed (`indexFunctions`: out-of-line definitions attached by finishIndex's own code,
  now `Impl::attachOutOfLine`; one matching no declaration, or an object declaration to decide, is
  built from scratch); merged overload sets are made again from the entries as they are now;
- what read the functions' names (step 2's records, by symbol) or a removed function's declared
  type is found (`invalidateReaders`): declared types forgotten and followed, the files whose walks
  read them walked again with the changed file; an instance, a class instance or a scope's relations
  that read one makes it a build from scratch; the 3b checks follow (nothing instantiated anew).
- The index line's entry count is the index's own plus what the functions added and took out.

Full game through one server with `--verify-incremental`: a member function added to
`BuildEntityAchievement` (and taken out again) built this way, "the same as from scratch", resolve
0.9 to 1.0 s (finding the readers scans every recorded read; the walk itself is one file). A
signature change of `BuildEntityAchievement::getPrototype` was built from scratch: instances read the
name `getPrototype` (names are recorded by symbol, step 2, so every `getPrototype` counts). The edit
test adds a signature change, an overload added (that changes which function another file calls)
and taken out, each built this way and checked against a new process's build.

**A template's body (landed 2026-10-30).** A changed body of a function template or of a class
template's member (inline or out of line; not constexpr, no deduced return type) is a body edit
(3b) whose instances are checked again: in a server's build every instance record keeps how it was
checked (`InstanceRecord::Recheck`: a function instance's task, or a class member's walker set-up)
and every instance walk keeps statistics of its own (e9762e60). `recheckInstancesOf` copies each
instance of a changed template from the new body and walks it again, its statistics and what it
reads replacing its old ones; the kept total is made again from every walk's own. It is a build from
scratch when an instance of the template is one only codegen made, when one deduces another return
type, when the checks make an instance anew (endRewalk) or when an instance the old bodies asked for
is asked for by nothing now. The kept file's instance copies are pointed at the new tokens with its
nodes (`forEachInstanceCopy`), those of the changed templates excepted (they are replaced).
Codegen then runs in full: rebuildLLVM emits again the functions it is given and those whose record
holds a changed body, but an instance's record does not hold its own template's body, so an
instance of a changed template kept its old code (the edit test's server-built program printed the
old value until the driver sent such builds to full codegen). Proposed to the codegen thread: an
instance's emission notes its template's definition (`noteInlined(original)`), after which the
driver can let codegen follow. The edit test edits a function template's and a class template
member's two-statement bodies (a trivial one is inlined into main and hid the stale instance).
Full game through one server with `--verify-incremental` (frozen tree, the run.cmd arguments,
scratchpad game-verify.cmd): a statement added to `OptionalFundamental<T>::load` (an out-of-line
member, 6 explicit instances) and taken out again: resolve 0.67 and 0.65 s (2.0 s from scratch),
codegen in full 9.9 and 15.9 s (7.1 s in the first build; the machine was shared), 5 modules
compiled again, each "the same as from scratch".

**A constexpr or consteval function's body (landed 2026-10-30).** What depends on such a body is
what the compile-time evaluator computed by running it, so that is recorded: wherever the evaluator
runs a body (a call, a member call for a place, a constructor's body), the current unit records a use
of the definition (`USE_RanBody`, through `ranBody`, which also calls codegen's `ran` hook: the
place calls and constructor bodies did not tell codegen before). Two caches answer without running
anything (a class instance's static member's value, `memberConstants`, and the specialisation
picked for known arguments): each entry keeps the bodies its computation ran and replays them to
every asker it answers. A run outside any unit while recording is remembered (`ranOutside`). A
changed constexpr body is then a body edit (3b) whose ran-readers go through `invalidateReaders` as a
changed name's readers do: the walks that ran it are walked again (their files) and emitted again;
it is a build from scratch when an instance, a class instance, a scope's relations or a declared type
ran it (an array bound, a constant template argument: what it typed may be a layout), or the body
ran outside a unit; the cache entries that ran it are dropped. A constexpr template's body takes
both paths (its instances checked again too); a deduced return type is still a build from scratch.
Two fixes on the way: a unit's repeated uses were dropped by address alone, so the run of a body
right after its function's declared type (one address, two kinds) went unrecorded (now by address
and kind); and a file walked again keeps the checker errors its old walk found (they are kept by
text, not by unit), so a file with errors is no longer walked again here (a build from scratch:
the edit test's static_assert made to fail and fixed again showed the stale error, and
`--verify-incremental` reported it). The edit test edits `limit()`, run by a constexpr local and a
static_assert in another file, then makes that static_assert fail and pass again.

Full game (the converted tree of C:\factorio-in-kpp d9080751 frozen in scratch, run.cmd's
arguments, scratch\resolver\game-verify.cmd), through one server with `--verify-incremental`, each
"the same as from scratch": `oppositeSide` (CliffOrientation.kpp, a plain constexpr function) edited
and back: 1 file walked again, codegen 4 functions, 1 module; `AreCreatedInOrder` (a constexpr
template two achievements' static_asserts run) edited and back: 3 files walked again, codegen in full
(a template's body); `BoundingBox::emptyValue` edited and back: 2 files walked again, codegen in full
(a global's initializer reads it). Wall time of the `oppositeSide` edit and back twice without
`--verify-incremental`: **3.6, 2.6, 2.6, 2.6 s** to a linked game (resolve 0.60 to 1.15 s, most of
it finding the readers in every recorded read; codegen 0.12 to 0.26 s; clang 0.63 s; link 0.46 s).
The same edits with master d98f13b9's kppc, which builds a constexpr body edit from scratch in the
server: 12.8, 17.0, 19.9, 21.8 s (resolve 2.2 to 4.0 s, codegen 8.1 to 15.7 s), run one after the
other on the shared machine.

**A silent stale layout in the whole-function edits (fixed 2026-10-30).** `struct Holder {
decltype(pick(1)) value; int after; }` and `pick`'s return type changed from int to long long: the
server's build kept `Holder` as `{ i32, i32 }` and `sizeof(Holder)` 8, a build from scratch gives
`{ i64, i32 }` and 16 (only `--verify-incremental` said so). Two holes. A `decltype(expr)` in a
declaration's type takes expr's type from the declaration's own walk, so its declared-type unit
records nothing and the walk's reads never reach the cached type; and a followed declared type could
be a data member's or a global's, which codegen reads as a layout or a global without any recorded
use. Now a reading walk whose declaration's type comes from it (`decltype`, a deduced `auto`, in a
variable's type or a function's signature) is a build from scratch, and a followed declared type
must be a function's, a parameter's, a template parameter's, an alias's or a function's local's
(else from scratch); layouts that change go through codegen's `changedLayouts` once step 4 takes
them. The edit test's step "return type edit (a data member is declared with it)" fails with master's
kppc and passes now.

### Design: step 4 for class layouts (draft, 2026-10-30; codegen's half for the codegen thread's review)

**What is recorded today.** Codegen's per-function record (`Shared::Definition`) keeps `inlined`
(the bodies a function's text holds: inlined callees, constant folds' bodies) and `types` (the
`%struct` names its text mentions). It does not keep the layouts it used: a size, a field's or a
base's offset, a vtable slot are folded into constants without naming the struct. In the resolver,
a class's member list is read by position (aggregate initialisation, traits, the evaluator's
`constLayouts`, reflection) in about 43 loops, none recorded; by name, every member lookup is
recorded (step 2, by symbol).

**Measured first: who depends on a class.** A temporary probe over the headline tree's recorded
reads (frozen game tree, `KPP_DEPENDENCY_STATS=1`), per class: the class instances whose arguments
mention it, the function and member instances that read the class's name or one of its data
members' names, and the walks and declared types that do:

| class | class instances mentioning it | instances reading its names | walks | declared types |
|---|---:|---:|---:|---:|
| BuildEntityAchievement | 10 | 0 | 32 | 27 |
| AchievementStats | 5 | 0 | 367 | 253 |
| AssemblingMachine | 0 | 4 | 779 | 306 |
| Inserter | 10 | 100 | 2,512 | 405 |
| Character | 16 | 55 | 2,553 | 1,034 |
| ElectricNetwork | 1 | 874 | 2,320 | 128 |
| Recipe | 2 | 1,074 | 984 | 45 |
| Chunk | 35 | 964 | 3,294 | 553 |
| MapPosition | 140 | 1,509 | 6,327 | 3,221 |
| TransportLine | 22 | 112,973 | 1,369 | 406 |

Names are recorded by symbol, so a member named `position` or `type` is read by everything reading
any `position`. With today's rule (an instance that read a changed name is a build from scratch),
almost no layout edit, and few signature edits (`getPrototype`, above), could stay incremental. So
the order of work:

1. **Instances checked again instead of a build from scratch.** An instance whose recorded reads meet
   a change (a name, a declared type, a body it ran) is copied and walked again through its
   `InstanceRecord::Recheck` (as a changed template's instances are, `recheckInstancesOf`), with the
   same checks after (no new instance, none dropped, the same deduced return type). A class
   instance's check likewise, once it keeps how it was checked. This serves signature edits now.
   *Landed for function and member instances:* `invalidateReaders` hands them to
   `recheckInstancesOf`; and a reading walk whose declaration's type the walk gives (decltype, a
   deduced `auto`) is compared after the walk (`forgetTypesFromWalks`, then `typeOfDecl`) instead of
   being a build from scratch, which only a changed type now is. The edit test's overload added and
   removed (an instance, `viaScale<double>`, calls `scale`) are checked again so, and its signature
   edit stays incremental beside an `auto` function reading `combine`. On the game a signature edit
   of `BuildEntityAchievement::getPrototype` is still from scratch: 1,826 instances read the symbol
   `getPrototype`, 506 of them made by codegen (`late`) and 328 of kinds with no `Recheck` yet
   (generic lambdas' call operators, functions codegen has typed through `typeFunctionFor`,
   variable templates). Next, either those kinds checked again, or names recorded with the class
   they were found in (most of the 1,826 read another class's `getPrototype`).
   *Then names recorded with their class (landed):* a lookup in a class's scope records the symbol
   with that scope (`memberName`: the scope's id and the symbol in one 64-bit entry of the unit's
   `names`, so a unit's record is the size it was and a build in its own process pays nothing); a
   lookup in a namespace's or an enumeration's scope, or one that finds a using-declaration (whose
   target is looked up elsewhere and cached), records the symbol alone. A changed member of class X
   is read by the symbol-alone records, and by the records of X and of every class derived from X
   (`nameReaders`); a member whose class cannot be told (`X::f` not attached) by every class's.
   Measured on the frozen game tree, the front end at -j 32, master's kppc (07b19720) and this one
   interleaved, 4 rounds (scratch\resolver\memmeasure.ps1): a build in its own process 1.32 to 1.35
   s of resolve and 1,705 to 1,708 MB peak, both alike; a recording build (a server's) 1.85 to 1.89
   s with master, 1.86 to 1.91 s with this, 11.8 M names recorded against 16.9 M, peak 2,012 to
   2,044 MB against 1,969 to 1,988 MB (sampled every 50 ms). Readers on the same recorded reads,
   by symbol and by class:

   | change | instances | walks |
   |---|---:|---:|
   | `BuildEntityAchievement::getPrototype` | 1,320 -> 0 | 3,383 -> 13 |
   | TransportLine's 27 data members | 112,972 -> 2 | 1,271 -> 144 |
   | Inserter's data members | 99 -> 5 | 2,415 -> 141 |
   | MapPosition's data members | 172 -> 24 | 1,791 -> 451 |
   | `Character::update` | 141 -> 0 | 413 -> 1 |

   Through the server the `getPrototype` signature edit now meets 5 instances instead of 1,826,
   all 5 made by codegen with no `Recheck` (the next step, (a) above); still from scratch until then.
   The edit test's two classes with a member `reading` check: an edit of `Gauge::reading` checks
   nothing that reads `Meter::reading` again.
   *Then the instances codegen made (landed):* one that read a change is taken out of the instance
   table, with every instance codegen made whose check used it (transitively), its statistics and
   its copy's failed static_asserts; codegen, which then runs in full, asks for it again and it is
   made anew, as in a build from scratch. The old record is kept (`droppedInstances`), so that no
   address a recorded read names becomes another record's. An instance that can be neither checked
   again nor dropped now fails the incremental build before any file is walked again (the attempt
   that failed this way after walking 3,383 readers' files took 18 s before the build from
   scratch). The edit test's `Panel::scaled<int>` (a member template defined out of line, called with
   explicit arguments: codegen makes it) is dropped and made anew when `Panel::base` changes.
   The `getPrototype` signature edit and back on the game, paired, 3 rounds interleaved
   (scratch\resolver\game-time.ps1; each through its own output directory): this kppc's server
   **10.44 to 10.77 s** to a linked game (resolve 1.05 to 1.11 s, codegen in full 7.8 to 8.1 s, one
   module compiled, the incremental link); master cddf99aa's server, which builds it from scratch
   after the attempt, 31.8 to 34.9 s; a new process building from scratch 10.14 to 10.33 s (resolve
   1.35 s, codegen 6.8 s). Every such build verified the same as from scratch. What is left is
   codegen: it runs in full when an instance was checked again or dropped (its rebuild takes
   program functions, not instances), and in the server 1 s slower than in a new process.
2. **The resolver's half of a layout edit.** A changed file whose differences are data members of
   non-template classes (added, removed, a type or a default initialiser changed; no virtual, base,
   union or typed-union change): the members grafted and indexed as whole functions are; every
   unit that read the class's name, an old or new member's name, or a declared type mentioning the
   class (or a class holding it by value or as a base, transitively) is walked or checked again
   (1); the evaluator's caches keyed by a type mentioning those classes are dropped
   (`constLayouts`, the trait answers, the specialisation cache, `memberConstants`); the positional
   reads (the 43 loops) record `USE_ClassMembers(class)` so that a unit reading a member list
   without a name is found too. The classes go to codegen as `changedLayouts`.
3. **Codegen's half, `rebuildLLVM(..., changedLayouts)`.** Proposed for the codegen thread:
   - `Emitter::structFor` is the one place a layout is used: while a function is emitted it notes
     the canonical class (`layoutsNow`, as `inlinedNow`), kept in its `Definition` as `layouts`.
     Constant folds through the layout hooks pass through it too.
   - The closure of the changed classes: every `StructInfo` whose `fieldTypes` or `bases` hold one
     (by value), transitively; their `StructInfo`s are dropped (`structs`, `structsByType`, the
     emitters' `layoutCache`, the type lines).
   - Emitted again: every definition whose `layouts` meets the closure, and the first-use items keyed
     by those classes (vtables and their thunks, `vtablesIn`; typeinfo and descriptors,
     `descriptorText`; destructor helpers, `emittedShared`; globals of those types). Until those
     first-use items have owners, a changed class that has any of them is a decline (codegen in full).
   - Until it lands, the server runs codegen in full for a layout edit (the resolver's half alone
     already saves the resolve from scratch).

**Member lists read by position, recorded (landed, the first piece of 2).** The 46 loops over a
class's member list go through `membersRead`, which records `USE_ClassMembers(class)`. The caches
computed from member lists or by the evaluator (`constLayouts`, the trait answers,
`memberConstants`, the specialisation cache) keep everything their computation recorded, names and
uses (`CollectUses`), and replay it into each asker they answer (`replayUses`): a cache hit was a
read nobody recorded (the trait answers and `constLayouts` read members' declared types too). A
change forgets the cached answers whose reads it meets (`forgetCachedAnswers`). Every declaration
walked outside any unit is now a unit of its own, not only functions and variables: a namespace's
or a class's static_assert, an alias, an enumeration with its values. Their reads were recorded by
nobody, so an edit they read left them unchecked: the edit test's new step makes a namespace-scope
static_assert in another file fail by a return type edit, and master's server answered it without
the error (`err differs`). A member list read outside any unit while the files are resolved
(`membersReadOutside`; class instances' member tasks read their templates' lists so) makes an edit of
that class's data members a build from scratch.

Measured on the game's recorded reads, the units reading a class's member list by position: none
for BuildEntityAchievement and Inserter, 1 to 4 for Character, AssemblingMachine, Recipe,
ElectricNetwork and AchievementStats, 3 walks for TransportLine and Chunk, 1,534 instances and 2,144
walks for MapPosition (a value type every aggregate and constant builds); no class instance reads
one. Cost, master 07b19720 and this interleaved, the front end at -j 32 on the frozen game tree: a
build in its own process 1.323 to 1.345 s of resolve against 1.328 to 1.347 s, peak the same
(1,707 to 1,710 MB: every recording helper returns on one static flag first); a recording build
1.894 to 1.978 s against 1.854 to 1.904 s, names 11.8 M -> 19.5 M (the caches' replays), uses
7.2 M -> 8.4 M.

**The resolver's half of a layout edit (landed, the second piece).** A changed file whose differences
are whole functions and data members (`functionsOnly`): a kept data member whose tokens, its `;`
included, all have places side by side at a new one's is the same, the others are removed and the
new ones added, each of a plain class (no template, union, typed union or closure; not static, an
alias or a friend; one declarator: `plainDataMember`). The members are taken out of the class's
scope and the kept member list (`unindexMember`, with their walks' reads) and the added ones grafted
in and indexed (`indexMembers`). What read them: their names in the class (and the classes derived
from it), the removed members' declared types, and every unit that read the class's member list by
position (`USE_ClassMembers`; a containing class's layout reads it through the replayed caches); a
class whose list was read outside any unit, a class instance or a declared type that read it, is a
build from scratch. The classes go to `Build::changedLayouts`, for codegen's
`rebuildLLVM(changedLayouts)` once it exists; until then codegen runs in full for them. The edit
test's layout step (a member added to `Shape`) and a member added to `Plain`, which a namespace-scope
`isTriviallyCopyable<Plain>` static_assert in another file reads by position only (it fails then,
as from scratch), are built from the kept program through each server.

Full game: `uint32_t extraCount = 0;` added to `BuildEntityAchievement` and taken out, with
`--verify-incremental`: each "the same as from scratch", resolve 0.75 and 0.77 s, one file walked
again, codegen in full, 5 modules compiled. Edit to linked game, 3 rounds interleaved
(scratch\resolver\game-time-layout.ps1): this kppc's server 10.32 to 10.61 s (resolve 0.76 to 0.81 s,
codegen 7.6 to 7.9 s); master cddf99aa's server, from scratch, 16.3 to 17.7 s; a new process 10.47 to
10.56 s. As for the signature edit, codegen in full is what is left.

**Generic lambdas' call operators checked again; names by position (landed).** A generic lambda's
call operator the resolve made keeps how it was made (its closure type and arguments, in a server's
build), and one that read a change is copied from the lambda and walked again
(`runLambdaInstance`), as a function instance is. Adding one to the edit test exposed an older gap:
the code names a function's local types by their tokens (`f::__lambda_58_0@285`, `TypeTable::toString`),
the parser numbers lambdas in the file's order (`__lambda_<file>_<n>`), and codegen names static
locals, catch filters and static initializer lists by their tokens; an edit earlier in a file moves
them, so a build from scratch names them otherwise, while the server kept the unchanged functions'
code (`--verify-incremental` reported the module, master's server too). Now `remapKeptFile` gives a
kept closure the name the new parse gives the lambda at its new place, emits again a kept
non-template function whose position-named static locals, catch filters or initializer lists moved,
and runs codegen in full when a kept local type moved or was renamed (codegen's session keeps the
types' names) or a template's position-named entity moved (its instances are not emitted again
alone). The edit test's lambdas.kpp: a lambda added before another moves and renumbers it.

**Codegen in full in the server, 1 s slower than in a new process: the old session's freeing.**
`runCodegen` freed the kept codegen session before generating anew, on the build's own time:
1.01 to 1.07 s on the game (timed around `build.codegen.reset()`, 4 builds). The replaced session is
now kept (`Build::retiredCodegen`) and freed once the build has answered its client. The layout edit
above, edit to linked game, interleaved, 4 builds each: this kppc's server **9.03 to 9.15 s**
(codegen in full 6.27 to 6.40 s) against a new process building from scratch 10.46 to 10.65 s
(codegen 6.73 to 6.85 s): an incremental build with codegen in full now beats a build from scratch.

### Step 5b wired: codegen's kept session in the server (on since 661f017)

The kept Build holds codegen's session (`Build::codegen`, declared last so it is destroyed before
the program it points into), filled by a server's build from scratch (`generateLLVM(..., keep)`). An
incremental build hands `rebuildLLVM` what changed: the functions whose bodies 3b replaced, the
functions step 4 added and removed, the functions whose walks read a changed name (step 4's
readers), and every program function whose walk read an edited function's name (its callers:
codegen inlines trivial bodies). It runs codegen in full instead when an instance read an edited
name (an instance's code may hold the body), a global's initializer read one, a module's object or
hash file is missing (an unchanged module is not assembled again, so it has no text to compile), or
rebuildLLVM declines. The IR report counts each module's bytes as last built for the modules not
assembled again; clang skips them.

Full game, a body edit of `AchievementAccessor::getStateInternal` through one server: resolve
0.071 s, **codegen 0.253 s**, clang 0.55 s (one module), link 4.3 s. `--verify-incremental` found
the IR and the modules the same as from scratch but the report's function count lower (by 7 per
rebuild: reported to the codegen thread), so the codegen rebuild was first on only with
`KPP_REBUILD_CODEGEN=1`. Codegen fixed the count and stopped assembling modules that come out the
same (661f017); the rebuild is on by default since. The same edit then, the edit and back twice
through one server: **wall 7.7, 6.5, 5.1, 5.5 s** to a linked game (resolve 0.08 to 0.13 s,
codegen 0.10 to 0.12 s, one module assembled and compiled in 0.6 to 0.8 s, the link 3.7 to 5.7 s);
with `--verify-incremental` each "the same as from scratch", 246,491 functions. The link is now
most of a body edit. A body edit of `BuildEntityAchievement::setup` runs codegen in full: instances
read the name `setup` (names are recorded by symbol). The edit test verifies the executable's
builds too now, checks that codegen rebuilt from its session, and turns a trivial getter called from
another file into two statements with a new value (an inlined body must not stay in its callers).

### Codegen's record of the bodies a function holds (2026-10-30)

Each function's definition in the session records the definitions (as written: an instance's
template's) whose bodies its text holds besides its own, its lambdas' included: the trivial callees
and constructors inlined into it (`inlineCall` calls `Emitter::noteInlined`), and every body the
interpreter ran for one of codegen's constant folds (`ConstHooks::ran`, called by `constRun`); with
it, how the function was emitted (its Instantiation: an instance in the module that asked for it).
rebuildLLVM emits again every function whose record holds a changed or removed body, instances
included, in the build's level order (the functions of depth d after all of depth d - 1, with the
first uses those asked for), and takes the emitters' verdicts on which callees are trivial again (a
stale one kept a getter edited into two statements inlined). `inlinedIdentity` (move, forward)
records nothing: what it emits does not depend on the body. The server keeps passing an edited
function's callers: one that only called it inlines it once an edit makes it trivial, and codegen
knows only the callers that inline. With the record, the server's "an instance read an edited name"
rule is no longer needed for inlining (an instance that holds the body is in the record); whether
to narrow it is the resolver thread's call.

A returned lambda's call operator that its first caller emits (`emitCallOperatorOnDemand`: the
writer never makes the private copy's closure, RenderParameters::getScreenPositionFunction's) went
into that caller's module and belonged to it; the callers of one level are emitted at once, so
which one was first was a race, in the build as in a rebuild (KPP_CHECK_REBUILD=7 moved
getScreenPositionFunction's operator from module 40 to 62). It is now placed in its writer's module
and belongs to the writer, and rebuildLLVM emits it again on its own once its writer is emitted
again.

Measured on master 165b365 with this (full game, 240,101 functions after inlining step 3):
KPP_CHECK_REBUILD every 50th program function (1,424) emits 1,884 again in 0.294 s, every 7th
(10,168) 14,409 in 2.077 s, all 71,176 76,497 in 3.629 s; each time no module assembled again, all
64 assembled from the kept state equal to the build's, the function count the build's. Through one
server (scratchpad srvcheck.sh), the getStateInternal edit, its revert and the edit again: codegen
0.132, 0.257 and 0.102 s, one module, each "the same as from scratch". The headline IR at -j 1 and
-j 32 is the same; the game's --dump-data checksum is C++'s (486241314) and the 19 menu maps run.

### The link in the edit loop (2026-10-30: incremental, 3.8 s -> 0.5 s)

A first try under load (the machine 40 to 100% busy) showed no gain from `/INCREMENTAL` and added
nothing. Measured again on a quiet machine (1 to 36% busy), the game's 64 objects as run.cmd
builds them, every variant in each round (scratchpad link\measure.ps1, measure2.ps1, measure3.ps1:
response files differing only in the flags, run from programs\factorio). Seconds per round:

| link | rounds | exe |
|---|---|---:|
| link.exe `/OPT:REF /DEBUG /MAP` (run.cmd until now) | 3.52 3.87 3.97 4.98 4.26, 3.56 to 3.99 | 67 MB |
| the same with `/OPT:NOICF` | 2.37 2.42 2.30 2.54 2.55, 2.13 to 2.94 | 80 MB |
| link.exe `/OPT:REF /OPT:NOICF /MAP`, no PDB | 1.79 1.89 2.19 2.03 2.42 | 80 MB |
| lld `/OPT:REF /OPT:NOICF /MAP`, no PDB | 0.99 to 1.43 (13 runs) | 66 MB |
| link.exe `/INCREMENTAL /OPT:NOREF /DEBUG /MAP`, full (no .ilk) | 2.16 1.94 2.20 2.43 2.21 | 113 MB |
| the same, relinking after one object changed | **0.58 to 0.73** (8 runs, 1.19 the first) | 113 MB |
| the same without `/MAP` | **0.44 to 0.63** (8 runs) | 113 MB |

"One object changed": module 17 recompiled from its `.ll` with one function added, alternating
with the original each round, so every relink saw a different object (touching the file's time
alone relinks as fast). Identical-function folding was 1.5 s of the 3.8 s; a debug link no longer
folds (`/OPT:NOICF`), which also keeps every function at an address of its own, so a crash report or
a debugger never names a folded twin. The incremental relink beats lld without a PDB (1.0 s) and
keeps the PDB, so the crash report keeps naming functions from it and the debugger has it: naming
them from the map instead (the lld route) loses both and is slower. Copying the 113 MB executable
to bin\x64 takes 0.04 s.

What landed: `kppc --incremental-link` (a debug build's link with `/OPT:NOREF /INCREMENTAL`;
`/INCREMENTAL` rules out `/OPT:REF`, so the executable keeps the functions nobody calls; a release
build with it is an error). run.cmd links with it and without `/MAP` (nothing read the map), and no
longer deletes kpp\run\factorio.exe before a build, which made every link a full one: whether the
build linked is the link's own line in build.txt (`+ link ... (exit 0)`). The first build after a
clean one is a full link (2.2 s); every later one relinks what changed.

In the loop itself (run.cmd, a statement added to `BuildEntityAchievement::setup` and changed each
time, the converted tree of 10-29 23:31, one module recompiled), every other link was a full one at
first: 0.44, 2.43, 0.48, 2.03, 0.60, 1.75, 0.45, 2.31, 0.44 s. `/VERBOSE:INCR` said why: "10 (out
of 1035) modules have changed", then "undefined symbol '_Avx2WmemEnabled' has alternate name;
performing full link". run.cmd rebuilt native\factorio-native.lib on every build (3.0 s), and the
library came out a few bytes different each time. native\build.cmd now builds nothing when its
inputs are as at the last build (native\stamp.ps1: the C:\Factorio commit and a hash of every file
in native\; 0.25 s): the links after that were 0.52, 0.47, 0.46, 0.42, 0.45 s (the first one,
after the library changed, 1.84 s). Wall time of run.cmd for the edit, build and `--version` run:
11.9 to 14.1 s (the front end and codegen in full are most of it; the compile server's
incremental build is the way past that).
- **3c**: codegen keeps each function's result and rebuilds only what an edit changed. The codegen
  thread owns codegen's half (its queued work on destructor helpers, cleanups and module assembly
  reshapes the same structures); the interface the server needs:

```
// What emitting one function produced, kept between builds (one per emitted function: a function
// of the program, an instance, a lambda's call operator, a destructor helper...).
struct FunctionResult
{
  std::string ir;                          // its definition (and nothing that depends on other functions' order)
  std::vector<FunctionKey> requested;      // the functions it asked for (calls, address-of, vtables it needs)
  std::vector<const Ty*> layouts;          // the class layouts it used (fields, sizes, vtables)
  std::vector<const Decl*> globals;        // the globals it read or defined
  std::vector<std::string> errors;         // its errors, as a clean build reports them
  uint32_t module;                         // where it goes
};
// FunctionKey: what names a function across builds. A FunctionDecl* of the program (the fast path
// keeps the declaration objects, moving only the bodies), or the resolver's instance record (kept
// too) for an instance; the server gives codegen these, never pointers it re-creates.

class CodegenSession                       // lives in the server between builds
{
public:
  CodegenResult build(...);                // today's generateLLVM, keeping every FunctionResult
  // An incremental build: the functions whose bodies changed (re-emitted), the classes whose layout
  // may have changed (every function that used one is re-emitted). The functions emitted are the
  // closure of the requests from the program's functions, recomputed from the kept lists: new
  // requests are emitted, functions nobody requests any more are dropped.
  CodegenResult rebuild(const std::vector<FunctionKey>& changedBodies, const std::vector<const Ty*>& changedLayouts);
};
// CodegenResult as today, plus which modules changed: the server writes and compiles only those
// (the object cache already skips a module whose text hash is unchanged; this skips assembling it).
```

  The requirement on all of it: a module's text is a function of the set of FunctionResults in it
  and the types they use, in an order that does not depend on how they were reached, so that a
  rebuilt module is byte-identical to the same module from a clean build (`--verify-incremental`
  checks it on every incremental build in tests/incremental.ps1).

  **Measured by:** the incremental test's edits, and on the full game through the server a leaf body
  edit (`BuildEntityAchievement::setup`) and a widely used inline function's body
  (`MapPosition::operator+`), each against a no-change rebuild: codegen's time (target under 0.1 s
  for the leaf), the modules rebuilt (one), the wall time to a linked game, and
  `--verify-incremental` byte-identical.

### Design: codegen's results, home modules and module assembly (codegen thread, for review, 2026-10-30)

One structure for 3c and for the codegen items above (the destructor helpers copied into every
module, the 4.5 s serial assembly, the 3 s teardown), so that each step lands on its own and the
last one is the server's `rebuild`.

**Where the IR goes today** (the game as run.cmd builds it, master fb0ce0e, over the 64 module
files): 1,760 MB in all. 843 MB are the `kpp.dtor.*` helpers, 14 MB the other linkonce_odr helpers
(thunks, kpp.nodtor), 563 MB the modules' own function definitions, 123 MB type definitions, 72 MB
declarations, 39 MB constants, 21 MB comdat lines, 85 MB the rest (globals, startup lists).

**What emitting a function produces today.** Its definition goes into `bodies[k]`, k a hash of its
IR name modulo the module count; the texts are lists of entries sorted at the end, which is why
the output no longer depends on the threads. Besides its definition a function leaves *first-use
effects*, written by whichever function reaches them first and owned by nobody: a layout (`structs`),
a global and its startup constructor (globalForOnce, into `decls` or `moduleDecls[k]`), a constant
(one per module and content, `moduleConstants`), a vtable (per module, `vtablesIn`), a helper
(`emittedShared`: the destructor thunks, pointer-to-member thunks, the dyncast helper), a lambda's
call operator or an instance (the worklist). Helpers go into `shared`, which every module gets
whole; types and external declarations into `decls`, which every module gets filtered;
`moduleText` then scans each module's whole text for `@names` to add `declare` lines. Two
consequences: the copies (helpers and types in all 64 modules), and that no function's output can
be replaced alone, since what it caused first belongs to no one.

**Proposed: everything emitted is an item with a stable name, and every item records what it uses.**

```
struct Item                     // kept between builds by the CodegenSession
{
  std::string name;             // IR name: a function's, a global's, a helper's, `%struct.<hash>`, a constant's content hash
  Placement placement;          // Own (a function, in module hash(name) % N), Home (one definition in module
                                // hash(name) % N, declared where used: helpers, globals, vtables, typeinfo),
                                // Private (a copy in each module that uses it: constants, type definitions)
  std::string text;             // its definition, nothing order-dependent in it (as today's entries)
  std::string declare;          // what a module that uses it but does not hold it writes (Home and Own)
  std::vector<std::string> uses;     // the items its text names (calls, constants, types, globals, helpers)
  std::vector<FunctionKey> requested; // the functions it asked to be emitted (today's worklist entries)
  std::vector<const Ty*> layouts;    // the class layouts it read
  std::vector<std::string> errors;   // as a clean build reports them
  const Decl* decl;             // for a function: its declaration (and the instance's arguments), for rebuild()
};
```

A function's FunctionResult (the 3c interface) is its Item; a first-use effect becomes an Item of its
own the first time any function names it, and every function that names it lists it in `uses`, so
it stays as long as anyone uses it. An item's text is a function of its name and the program, never
of who reached it first (true today for constants and helpers; globals' startup constructors need
their temporaries numbered from zero, which globalFor already ensures).

**Module k** is then a pure function of the items: the header; the Private items used by module k's
items, sorted; the Home items whose home is k, sorted; a `declare` for every Home or Own item used in
k and held elsewhere, sorted; k's Own items, sorted. The `uses` lists replace the scan of the text
for `@names`, and each module is assembled on a thread of its own once emission is done (64
independent jobs instead of 4.5 s on one). The helpers stop being copied: **843 + 14 MB become about
13 MB**, defined once in their home modules (linkonce_odr stays, harmless). Type definitions become
Private items written only where used (123 MB today, every type in every module). Expected: the IR
from 1.76 GB to about 0.85 GB, clang's work and the assembly roughly halved, and smaller modules
possible (256 instead of 64) once a module no longer carries a fixed 15 MB of helpers.

**rebuild(changedBodies, changedLayouts)** on these items: drop the Items of the functions whose
declaration is in changedBodies (all of its instances, and the lambdas and local classes inside it:
their names carry the enclosing instance's mark and token, so a body edit renames them and the old
ones go unreached) and of every item whose `layouts` meets changedLayouts; emit those again through
the same emitter as a clean build, so an item's text cannot depend on the path; then recompute the
reachable set from the roots (the program's non-template functions, startup functions, vtables'
slots) through the kept `uses` and `requested`, without emitting what is kept; emit new requests,
drop unreached items. A module is assembled again when the set of item names it holds or uses, or
one of their texts, changed; the server compiles the modules whose text hash changed (the object
cache already skips the others).

**Notes on the interface above.** `FunctionKey` as the resolver's records works: codegen keeps
`decl` and the instance's arguments in each Item and maps the server's keys to the Items of that
declaration; inside codegen the IR name stays the key (stable while the declaration and its types
are). Layouts are recorded as canonical class types (canonicalClass), the keys structFor uses.
Errors stay per Item and are reported sorted, as now. Inlining (the templates thread's design
above, step 1 landed in callFunction as 9b36a63) copies a callee's body into its callers, so an Item
also records the bodies it inlined (`inlined`, FunctionKeys): an edit of an inlined body re-emits
its callers, as a layout change re-emits its users. move and forward, all step 1 inlines, are the
library's, whose change is a full rebuild anyway.

**Placement is stable across edits.** A function's or a home item's module is hash(IR name) %
module count; the IR name is printed from the declaration and its types alone (never from the
order anything was reached), so an unchanged function or helper stays in its module whatever else
changes, and a module's object is reused unless one of its own items changed. The module count is
fixed by the configuration (2 x the threads, at most the file count: 64 for the game on this
machine); a different count is a different build (all modules new), as a different `-j` is today.

**Order of work**, each step landed with its measurement and run_tests' byte-for-byte checks:
1. Codegen's teardown skipped (its 3 s of freeing; the process exits, the server frees on its own
   thread). Measured: codegen's wall time on the game. *Landed 8280967: codegen 13.2, 12.5, 13.2,
   18.3 s -> 10.1, 9.8, 11.5, 13.4 s, interleaved on the shared machine; output unchanged.*
2. Chained cleanups: each landing pad continues into the previous one's instead of destroying again
   everything built before it (ControlSettings' constructor, 875 k lines, a quarter of clang's
   critical path). Measured: that module's size and clang time. *Landed e243d6d: the constructor
   875,042 -> 11,956 lines (481,670 -> 1,960 calls of ~String), its module 99 -> 25 MB and clang on
   it 7.6 / 5.4 -> 1.7 / 1.2 s; the game's IR 1,759 -> 1,644 MB, clean clang 10.9, 11.1, 10.3 s ->
   9.5, 5.1, 5.9 s.*
3. Home modules for the helpers, per-module type definitions, declares from recorded `uses`
   instead of the text scan, and parallel assembly. The output changes once (smaller). Measured: IR
   size, codegen's assembly time, clang's total and slowest module, the link. *The destructor
   helpers' home modules landed first: the game's IR 1,631 -> 785 MB; clean builds interleaved (the
   machine shared): clang 5.0, 7.8, 7.9 s -> 6.4, 3.2, 3.5 s, codegen 8.6, 9.9, 9.2 s -> 8.5, 8.6,
   8.9 s; --dump-data's checksums and the 19 menu maps unchanged. A home definition is `weak_odr`:
   as linkonce_odr the release build's optimizer dropped it from its home module, which did not
   call it, and the link missed it. ThinLTO release build of the game: link 130 -> 102 s, wall 154 ->
   125 s, the executable 52,862,976 -> 52,864,000 bytes (ThinLTO imports the helpers it inlines).
   (Both release executables stop at their command line, "Error opening '--benchmark'", before and
   after: the release game has never run; to look at.)* *Then the modules assembled on all threads
   (each module's own texts freed once assembled) and each module given only the type definitions
   it names, and those they name: the IR 786 -> 668 MB; codegen 11.8, 11.1, 8.5 s -> 10.2, 9.3, 6.9 s;
   clang 6.0, 7.0, 6.9 s -> 6.9, 5.0, 3.1 s; wall 28.9, 32.1, 23.7 s -> 28.8, 25.7, 17.6 s (clean
   builds, interleaved, the machine shared). The declares still come from a scan of each module's
   text (now on its own thread); step 4's recorded `uses` replace it.*
4. Every emission recorded as Items (what step 3 placed by name, now with its `uses`, `requested`
   and `layouts`), and the modules assembled from the Items: byte-identical to step 3's output.
   Measured: the cost to a clean build (target: within a few percent). *First part landed
   2096299: each definition carries the `@names` and types its text uses, found on the thread that
   emitted it (Shared::Definition), and a module is assembled from those (no scan of its whole
   text): all 64 of the game's modules byte-identical; codegen 8.3, 7.4, 9.6 s -> 7.1, 6.7, 6.1 s.*
5. CodegenSession::rebuild with the resolver's 3b, measured as 3c says above. *In-process part
   landed 638cf03: a definition records its owner (the function whose emission added it, or itself
   for what is emitted once at a first use: a global's constructor, a lambda's function-pointer
   thunk), and what a function adds on every emission (static locals and their guards, static
   InitializerList arrays) is an owned entry joining its module's declarations when they are
   finished. `KPP_CHECK_REBUILD=n` emits every n-th function again in the kept state and assembles
   the modules again: on the game n=50 is 1,424 functions, n=7 is 10,168, and 0 of 64 modules
   differ; one function is emitted again in 0.093 s and its module assembled in 0.027 s. What the
   server's part needs: from 3b, the FunctionDecls whose bodies changed (their functions are
   emitted again under their owners' names, their old definitions and owned entries dropped
   first); the kept session (Shared and emitters, today freed on a detached thread) held by the
   kept Build; then only the modules whose definitions or entries changed are assembled again, and
   a first use that adds to the shared declarations (a new global or helper) assembles all.
   Not yet: an item that only an edited body used stays (a clean build would not have it), which
   --verify-incremental reports; to be dropped by use, as types already are.*

#### Step 5b: what codegen proposes to the kept Build (codegen thread, for the resolver thread, 2026-10-30)

The interface, in codegen.hpp:

```
struct CodegenSession;  // codegen's state between builds: the shared texts, layouts, names, per-module
                        // definitions with their owners, the emitters; it points into the program
                        // (GlobalScope, types, the parsed files), so it is destroyed before them

// As today; with keep, the session is handed back instead of freed on a detached thread.
CodegenResult generateLLVM(..., std::unique_ptr<CodegenSession>* keep = nullptr);

// After 3b checked the changed bodies again: emits again the functions whose declarations got new
// bodies (and what their emission newly asks for), drops what their old emission added (their
// definitions, their lambdas' operators, their static locals), and assembles again the modules that
// changed (all of them when a first use added to the shared declarations: a new global or helper).
// Returns every module, as generateLLVM does, and which ones changed.
CodegenResult rebuildLLVM(CodegenSession& session, const std::vector<FunctionDecl*>& changedBodies);
```

What the kept Build does with it: it holds `std::unique_ptr<CodegenSession> codegen` (declared after
what the session points into, so it is destroyed first); a build from scratch fills it (`keep`);
incrementalBuild hands rebuildLLVM the FunctionDecls 3b moved new bodies into, under 3b's own rules
(non-template, nothing deduced from the body, the instance set unchanged), and builds from scratch
when there is no session or 3b declines. `--verify-incremental` compares as today; until stale items
are dropped by use (above), a body edit that stops using the only use of a constant or global shows
there as a difference, never silently.

Cost to a clean build in the server: the session is what codegen freed on a detached thread
(about 1 GB for the game), kept instead; a build in its own process does not keep it.

## Parallel front end: contention, not work (2026-10-29, evening)

The front end on the headline tree had grown to about 4 s (read, imports, parse and index 1.1 s;
resolve 2.6 to 4.4 s), with 18 k class and 72 k function instantiations checked. A sampling
profiler over all threads (temporary, not committed: it suspends every thread of the process each
millisecond and unwinds its stack; threads stopped in ntdll are counted as waiting, and where they
wait is reported apart) showed that the resolve phase was not short of CPU but of parallelism:

- The same tree resolved on one thread (`-j 1`) in 9.4 s, while the 32-thread run spent 66 s of
  thread time in resolveFile (the per-file times summed) and 30 to 70 s of process CPU: about
  seven times the work, burnt in contention.
- On average 1.5 of the 32 workers were running; the rest waited, nearly half of the waiting in
  one lock: `SymbolTable::name` took the symbol table's mutex on every call (trait names,
  conversions, lookups ask for names constantly). A fifth of the rest was `recordInstanceSubst`'s
  single map under a single mutex.
- The function-instance maps hashed their keys (pointer bytes) one byte at a time, and chose the
  shard by the same hash modulo 64, so within a shard all keys shared their low six bits and the
  map used one bucket in 64: long chains under the shard lock.
- Type traits (isTriviallyCopyable, isConstructible, isAbstract) walked a class's special members
  and bases on every query: a fifth of the busy samples.

Fixed (each measured paired on a frozen copy of the converted tree, front end only, five runs;
the machine is shared, so minima and medians): word-wise key hash and top-bit shards (2884b16),
a per-(trait, types) answer cache (e46314f), lock-free `SymbolTable::name` (5cca409), sharded
instance substitutions (af28a09). Together, master 7fa0dc4 against 92342eb:

| frozen headline tree, front end | before | after |
|---|---:|---:|
| resolve, 32 threads, median of 5 | 3.34 s | **1.03 s** |
| resolve, 32 threads, min of 5 | 2.04 s | 1.03 s |
| process CPU, 32 threads, median | 52.6 s | 18.4 s |
| resolve, `-j 1` | 10.8 s | 9.1 s |

Errors, function counts and the canonical count are unchanged. Not done: starting the largest
files first (ValueToWidget.kpp, 0.7 MB, alone took 2.2 s and started late) made no difference at
32 threads once contention dominated, and it changed the typed-expression count, so it was left out.

**The front end was not deterministic; it is now (step 2 below).** Two runs of the same tree on
32 threads typed a slightly different number of expressions (6,707,129 against 6,720,488, about
0.2%); one thread was stable. What differed, found by dumping every expression's type per file
(the environment variable `KPP_DUMP_FILESTATS=path` writes each file's statistics) and every
instance walk, and diffing a `-j 1` run against loaded `-j 32` runs:

- **Different types, of one kind of expression: a name of an `auto` function.** A call
  `f(x)` names f; the name's own type was `auto(...)` when f's file had not been walked yet and the
  deduced type when it had, so it depended on which thread walked which file first. Worse, a
  function template's own generic walk cached what its uninstantiated body deduced (a closure type
  of the generic body, no instance's type) as the template's type, and names of the template then
  read that. The *calls'* types were right in every run (a call's result always went through the
  instance), and codegen takes a call's type from the call, so this produced no wrong code that we
  know of; but it was a wrong type on a node, readable by anything that asks. Fixed: a name of a
  non-template `auto` function gets the type its one instance deduces, deduced then if need be, and
  a template's generic walk no longer caches its body's deduction.
- **Instances checked twice or not at all.** The same function instance reached with its
  substitution's pairs in another order had another claim key, so it was checked twice in some runs
  (claimInstance sorted its pairs, claimFunction did not); and a non-template `auto` function was
  checked as an instance by callers that came before its file's walk cached its return type, and
  not by callers after. Fixed: both keys are ordered, and callers always go through the instance.
- **Statistics counted by whoever asked.** An instance's walk added its counts to the walker that
  asked for it first; when that was a probe (a requires-expression's, whose counts are dropped) the
  instance went uncounted, and nested instances were counted again in every enclosing instance's
  total (11.2 M "expressions inside instantiations" were 4.57 M). Fixed: each instance's counts go
  to one total, once. "First seen at" places and the reports' ties are ordered by place and name
  (symbol ids depend on which thread lexed what).
- **Uses of a global `auto` constant whose initializer does not type** (Lua's `const auto
  LUAL_BUFFERSIZE = BUFSIZ;` where BUFSIZ's import failed): a use read the initializer's type when
  the constant's own file had walked it and deduced on the spot when not, so the use was counted
  as "unknown because auto" or not by the order (the types were unknown both ways). Fixed: only a
  known initializer type counts as walked; a declared type is cached only when known and
  auto-free (an unknown one could come from the depth limit of whoever asked first); a failed
  deduction is counted once, by the declaration's own file's walk.
- **Types interned.** The number of distinct types interned still differs by one or two: paths
  that are equivalent but taken in another order intern different intermediate types. It is a
  cache's size, so the determinism check leaves that line out.
- **Requires-expressions** are probed per argument type on the concept's shared nodes, so those
  nodes keep whichever probe wrote last; their types are never read for code, only each probe's
  answer, which does not depend on order.

`tests/determinism.ps1` (run by run_tests.cmd, failing it) builds three test programs at `-j 1`,
`-j 8` and `-j 32` and requires byte-identical IR and reports, and when the converted game exists
(after convert.cmd) requires the headline front end's report to be identical at `-j 1`, `-j 8` and
twice at `-j 32`. Under load (a second compile running beside it) master differed in about one run
in six; after the fixes, 0 of 10.

### Design: instantiation checks as tasks (done: steps 1 to 3)

Today a walker that needs a class instance claims it and checks it inline: the instance's
declarations and then every member body (`instantiateClass` -> `checkMemberBody`, 31% of the busy
samples), before returning to its own file. A thread asking for an instance another thread is
building spins on `yield` until it is done. So one file drags its whole instance tree onto one
thread, and the waiting threads burn CPU.

1. **Waits block instead of spinning.** `state.wait()` / `notify_all()` (C++20 atomic wait) in
   place of the yield loops in claimInstance's, claimClass's and claimFunction's consumers. No
   change in results; CPU only.
2. **Member bodies become tasks.** Instantiating a class does what its users need now (the
   declarations, base classes, layout facts, members' declared types) and queues the check of each
   member body as a task, keyed by (instance, member). Members whose return type is deduced
   (`auto`) are still checked when a caller needs that type, by the caller, as today. All workers
   take tasks after their files and whenever they would otherwise wait; checking a body may queue
   more. The phase ends when the queue is empty.
3. **Results independent of order.** An instance's typing must not depend on what other instances
   have finished: an instance found on the current thread's own stack (a cycle) gets a fixed
   answer (the declared type, never the partial body), and one being built on another thread is
   waited for. Errors and statistics are collected per instance and sorted at the end, as codegen's
   are.
4. **Checked by** comparing the whole of stdout and stderr between `-j 1`, `-j 8` and `-j 32` and
   between repeated runs: they must be byte-identical, as codegen's IR is. A new determinism check
   runs this on the headline tree.

Expected: resolve close to `-j 1`'s 9 s divided by the workers that have work, so well under a
second, and no CPU burnt waiting. Order of work: 1 (small, alone), then 3 with its check (it is
the property the rest relies on), then 2.

Done so far (numbered as landed: step 1 = item 1, step 2 = items 3 and 4):
- Step 1 (cee6aa6): waits block. No measurable change (resolve median 1.05 -> 1.03 s): by then
  waits were rare, so it removes a worst case rather than a cost.
- Step 2 (932b4c4 and the follow-ups to 098b96e): the output does not depend on the thread count,
  as above. Cost: none measurable at 32 threads (median 1.07 -> 1.12 s, within the noise); `-j 1`
  7.9 -> 7.5 s.
- Step 3 (item 2): while the files are resolved, a class instance's member bodies and a function
  instance whose result nobody waits for are queued as tasks; one pool takes files, then tasks
  (GlobalScope::resolveAll), until nothing is queued or running. A walk that needs a queued
  instance's deduced return takes its check over on the spot (an atomic exchange decides who runs
  it), at the depth of the walk that asked, so the depth limit and the results do not depend on
  who ran it; the statistics and errors are those of `-j 1` exactly, and tests/determinism.ps1
  passes. Per member body the tasks cost more than they spread, so a class instance is one task.
  The headline tree's front end (CommonUtil, Agui, cxxopts, StackWalker, Lua, src), five paired
  runs: resolve median **1.57 -> 1.25 s** (min 1.55 -> 1.25 s), CPU about the same. A first version
  that queued only class instances' member bodies, and drained them after the files, was 0.15 s
  slower: the files' own function instances kept a few threads busy while the rest waited.

## Parallel codegen, first step (2026-10-29)

Codegen now emits functions on all threads (`--codegen-threads N`, default `--threads`): level by
level (the functions the program writes, then each round of instantiations they asked for), each
emitter with its own function state and caches, the class layouts, closures, globals and output
shared under one lock, the sets of names asked for and declared in 64 locked shards. What it
emits does not depend on the thread count or on timing: names come from contents (fe55d9e), the
output is sorted, every use of a failed layout or global reports the same error, and the errors
are sorted. Frozen headline tree (the evening's copy), headline command, machine shared with other
builds (48% busy):

| | codegen | wall | stderr lines |
|---|---:|---:|---:|
| `--codegen-threads 1` | 8.69 s | 15.7 s | 26,094 |
| 32 threads (two runs) | 4.07 s, 4.08 s | 11.8 s, 14.3 s | 26,094, 26,094 |

A first 2.1x: the layouts are still built under the shared lock (the largest holder, about 1 s
of it), and every emitter lays out each class it meets once more for its own cache. Run to run,
the error lines differ only in the "requested by" part (which function asked first) and in about
ten lines around function-local statics used by generic lambdas.

## Whole converted src, evening of 2026-10-28 (d549f69)

Input: the headline inputs (path-to-factorio.md: CommonUtil, Agui and `src/` with the stdlib,
4,099 files, 40 MB, 0.98 M lines, 6.3 M tokens), the headline command (`--emit-llvm`), a frozen
copy of that day's convert.cmd output. Three runs: **8.1 s, 8.8 s** and 12.1 s wall (the last
while another thread was building), peak working set **1.0 GB**, 31,735 codegen errors. Phase
split of the first run (when each driver line appears):

| phase | wall | notes |
|---|---:|---|
| read + lex | 0.9 s | read 0.81 s (the file cache partly cold after a fresh conversion), lex 0.06 s |
| C header import | 0.4 s | 50 headers, cached |
| parse + index | 0.1 s | |
| resolve + check + template instantiation | 1.5 s | 14.6 k class and 42.2 k function instantiations checked |
| **front end total** | **2.9 s** | 2.1 s with a warm file cache |
| codegen | 5.4 s | single-threaded; 120,737 functions, 15,043 structs; errors, so no IR is assembled |

What changed in codegen since the morning's measurement below:

- The "out of memory" stop is gone (69ffce0): it was an out-of-bounds read of a function's
  parameter list (a garbage string copy asking for 7 exabytes), not memory. Past it, a wrong
  deduction from a brace list made codegen run forever, which a limit of 64 nested instantiation
  requests now turns into an error (design.md section 3; the whole game needs 12).
- Single-threaded codegen got 31% faster (6388d00, ba670ed), profile-guided with a sampling
  profiler: class sizes and alignments are remembered instead of recomputed from the fields on
  every use, qualified names and function IR names are cached instead of rebuilt as strings per
  call site, member lookup no longer builds the qualified name of every member of the class, a
  layout is found by any spelling of its type, constructor overload choices are cached, and an
  error's line number comes from a table instead of counting newlines. Same tree, same master,
  three runs each: **codegen 8.35 to 8.49 s -> 5.71 to 5.83 s**.
- The occasional run that never ended (400+ s at 1 GB, about one in four) is fixed (4c98ac0): two
  recursions without end overflowed the stack, and the overflow handler then allocated inside a
  heap the overflow had interrupted. Both recursions are errors now, codegen runs on a thread with
  a 1 GB stack, and an overflow that still happens ends the process with the function's name.
- The function count is not comparable with the morning's: functions that depend on a class
  whose layout cannot be built correctly now fail early instead of compiling against a wrong
  layout (96629a6, the headline correction to about 38 k errors).

Parallel codegen is not done yet. Codegen re-types each template's body in place in the shared AST
before emitting an instantiation of it (GlobalScope::retypeFor), and the checker's instantiation
checks running inside those re-types write into other templates' ASTs too, so two threads cannot
emit instantiations safely: they would read each other's types and emit wrong code without an
error. The fix is that each instantiation owns its typed body (design.md, "Instantiations own their typed
bodies": a copy of the template's body per instantiation, typed in place and kept, measured at
0.6 to 0.9 GB), followed by a parallel driver with output that does not depend on thread timing. It
waits for kovarex's go, since it changes resolver.cpp. The same writes make the parallel front end
nondeterministic today: the headline moves by a few lines between identical runs, and `-j 1` gives
about 2,000 fewer spurious overload failures than `-j 32` (resolver thread's measurement).

## The full src past IR for the first time: clang and the link (2026-10-28, night)

`kppc --keep-going --exe` (e7d4669) compiles and links what codegen emitted although it reported
errors: the functions the errors left out are only declared, so the link lists them. Same inputs
as above, the game's own Release builds of its C libraries (C:\Factorio\vs2022\Releasex64: lua,
sdl, sdl-mixer, curl, zlib, zstd, minizip, ogg, vorbis, opus, opusfile, re2, crc32fast, stb,
fontstash, glad, imgui, trio, open-vcdiff, stack-walker) passed with `--lib`.

| step | wall on 32 threads | notes |
|---|---:|---|
| codegen, 64 modules | 12.4 s | 128.5 k functions, 379 MB of IR (single module: 5.4 s; the module assembly is still slow, see below) |
| clang -O0, 64 modules | **6.0 to 6.3 s** | sum of the module times 54 to 56 s; one module takes 5.3 to 5.5 s of it (below) |
| clang -O2, 64 modules | 228 s | sum 517 s; that one module alone 224 s (measured before the modules were balanced) |
| link, lld | **1.6 s** | with /FORCE (10,915 unresolved symbols); link.exe: 3.4 s, stops at a duplicate symbol without /FORCE:MULTIPLE |

What the numbers depend on:

- **One function is most of the backend.** `ControlSettings::ControlSettings(const PropertyTree&,
  KeyboardLayout)`, about 1,400 lines of Kpp (key bindings, one after another), becomes 875 k lines
  of IR, 70 MB, with 481,672 destructor calls. Every one of its 981 cleanup pads destroys all the
  objects alive at that point, so the cleanups grow with the square of the number of objects.
  Cleanups chained scope by scope (each pad destroys what its scope added and unwinds to the
  enclosing one, as clang's do) make it linear. Without the one outlier, the remaining module time
  spread over 32 threads would be about 2 to 3 s at -O0 and about 16 s at -O2 (estimates from
  the measured sums, 54 s and 517 s, not measurements).
- **Balance.** Instantiations used to land in the module of whoever asked for them first, which
  piled the library's into a few modules (the slowest -O0 module took 7.7 s before, 5.3 s after);
  they now go to the module their name hashes to.
- **Module assembly.** Splitting into 64 modules costs 7 s more than one module: every module
  carries the whole shared declaration text (all struct types and globals), copied 64 times, and
  filtered line by line per module. Per-module declarations (only what the module uses) are the
  fix, and they also make a module's text change only when something it uses changes, which the
  object cache needs.
- What the link cannot find is listed in path-to-factorio.md ("From emitted functions to an
  executable").

## Whole converted src, morning of 2026-10-28 (de690f4)

Input: CommonUtil, Agui, all of `src/`, the stubs and the stdlib: 4,412 files, 41 MB, 1.04 M lines,
6.4 M tokens. `kppc --exe` on it, three runs: **8.3 s, 8.5 s, 9.2 s** wall, peak working set
**1.03 to 1.04 GB**. The phase split (the time each driver line appears, warm file cache):

| phase | wall | notes |
|---|---:|---|
| read + lex | 0.1 s | 0.03 s read, 0.06 s lex, 17 M lines/s (cold file cache: read 0.95 s) |
| parse | 0.07 s | 3.7 M nodes, 331 MB AST; 40 parse errors in 17 files |
| (between lex and parse lines) | 0.3 s | stdlib, C header import (cached), setup |
| index | 0.05 s | |
| resolve + check + template instantiation | 1.0 s | 5.7 M expressions (94.5% typed); 13.8 k class and 33.3 k function instantiations checked |
| **front end total** | **1.5 s** | |
| codegen | 6.4 s | single-threaded; 139,309 functions, 16,968 structs |
| IR text, clang, link | not run | codegen reports errors (about 19 k lines), so no IR is assembled |

Notes on the numbers:

- The coordinator's earlier "0.2 s front end" was parse and resolve before the checker and
  instantiation existed; with full type checking it is 1.5 s.
- The `instantiate:` line says "51 s of CPU"; that cannot be CPU time (the phase takes 1.0 s wall
  on 32 threads), it is a sum of nested per-request times. Ignore it as a cost.
- Codegen stops with "out of memory after ~79 k template instantiations (~6.4 k remaining)" while
  the process's peak working set is 1.04 GB on a 128 GB machine. That `bad_alloc` is therefore not
  memory exhaustion but one absurd allocation request (a wrong size somewhere), a bug to find, not
  a limit to raise.
- Codegen costs 46 microseconds per function here against 13 on the synthetic program
  (prototype-results.md, "Backend at scale"): real game functions are bigger and request more
  instantiations.

## The largest converted program that builds: CommonUtil + Agui

The Agui smoke test (converted CommonUtil and Agui, 210 files, 45 k lines; convert.cmd builds and
runs it) is the real converted code that goes all the way to an executable. Three runs each, no
cached objects:

| | wall | codegen | clang (64 modules, 32 threads) | link |
|---|---:|---:|---:|---:|
| debug (-O0) | 0.86 to 0.88 s | 0.25 s, 5,620 functions, 13.7 MB IR | 0.31 s | 0.13 to 0.14 s |
| release (-O2) | 1.0 to 1.9 s | 0.19 to 0.26 s | 0.63 to 1.36 s | 0.09 to 0.26 s |

The spread in the release runs is the machine: another thread was building at the same time.
clang run on each module alone, one at a time, fitted to a cost per KB of IR: **-O2 0.49 to
0.56 ms per KB** (two sessions); -O0 0.066 ms per KB in the quiet session (the second session's
-O0 numbers were dominated by the concurrent load and are not used). On the synthetic program the
same costs were 0.040 ms (-O0) and 0.93 ms (-O2) per KB: real code is 1.6x dearer at -O0 and about
half the price at -O2. The converted code produces 2.4 KB of IR per function.

## Projection for the whole game

Assumptions, each a range:

- **Functions:** 140 k to 200 k. 139 k are emitted before codegen stops, 6.4 k instantiations are
  still queued, and the 32,884 codegen errors (path-to-factorio.md) include unresolved calls whose
  targets, once resolved, request more instantiations.
- **IR:** 330 to 600 MB: 2.4 KB per function, plus the repeated struct types and `linkonce_odr`
  copies that grow with the number of modules.
- **Parallelism of clang over modules:** 12x to 20x on 32 threads (the synthetic program got 12x
  at -O0 on 64 modules; bigger modules amortise process start better).
- **Front end:** 1.5 s, as today; fixing the remaining errors adds checked code, removing
  converter workarounds removes some. Plus 1 s of reading on a cold file cache.
- **Codegen:** 6.5 to 9 s single-threaded (today's 6.4 s plus assembling the IR text, which is
  skipped today); 0.5 to 1 s once it runs per file in parallel, the next step after the errors.
- **Link** (lld, no debug info): 0.5 to 1.5 s. Debug info is not emitted yet; it adds to clang
  -O0 and to the link, guessed at +30% to +100% of those two.

### Debug clean build (C++: 62 s)

| | front end | codegen | clang -O0 | link | total |
|---|---:|---:|---:|---:|---:|
| codegen single-threaded (today's design) | 1.5 s | 6.5 to 9 s | 1.5 to 3.5 s | 0.5 to 1.5 s | **10 to 15 s** |
| codegen parallel | 1.5 s | 0.5 to 1 s | 1.5 to 3.5 s | 0.5 to 1.5 s | **4 to 7.5 s** |
| parallel, with debug info | 1.5 s | 0.5 to 1 s | 2 to 7 s | 1 to 3 s | **5 to 12 s** |

### One-line edit inside a function body (C++: 16 s)

The driver already keeps the object of every module whose IR text did not change, so clang
compiles one module (0.05 to 0.3 s).

| | total | dominated by |
|---|---:|---|
| new process, codegen single-threaded | **9 to 12 s** | codegen |
| new process, codegen parallel | **2.5 to 4.5 s** | front end and link |
| persistent compiler process (re-parse one file, re-check what depends on it, re-emit its module) | **0.5 to 2 s** | the link |

With the persistent process the link is the floor; going below it needs incremental linking or
reloading changed code into the running game, neither planned yet. An edit that changes a widely
used class layout (C++: 55 s through the PCH) changes every module that uses the class and costs
close to a clean debug build, 4 to 12 s.

### Release build (Factorio's C++ release build: not measured yet)

| | total | assumptions |
|---|---:|---|
| per-module -O2, no cross-module inlining | **13 to 28 s** | 165 to 345 s of -O2 CPU over 12x to 20x |
| kppc inlines trivial functions at IR emission | **15 to 35 s** | IR grows 10% to 30% |
| ThinLTO over the modules | **18 to 50 s** | ThinLTO typically costs 1.2x to 2x plain per-module -O2, with a serial thin-link step |

A one-line edit in a release build recompiles one module at -O2 (0.3 to 1 s) on top of the
incremental debug numbers, with or without kppc-side inlining; with ThinLTO the importing
modules are recompiled too, so a few seconds.

**ThinLTO is what release builds do (2026-10-29).** Since instantiations go to the module their
name hashes to, a hot callee is usually in another module than its caller, and per-module -O2
cannot inline it: the library benchmarks (compiler/bench/stdlib/run.cmd, 57 modules) ran up to
20 times slower than one module (sort.int 80 ms against 44, map.lookup 119 against 72,
vector.insert front 897 against 46). `--release` with several modules now compiles each to
ThinLTO bitcode and links with lld (`-flto=thin`, back ends on the compiler's threads, a cache next
to the executable): every benchmark is within noise of the one-module build again (sort.int 43.1,
map.lookup 74.0, vector.insert front 45.9). The benchmark builds in 0.2 s of compiling and 0.3 s of
link instead of 0.27 and 0.08. The full game's release build has not been timed this way yet.

## What would move these numbers most

1. The remaining codegen errors, so that the first real full-game IR replaces the extrapolation
   above (the `bad_alloc` is found and fixed; the errors are counted honestly now, about 32 k,
   most of them behind about 260 classes whose layout cannot be built: path-to-factorio.md).
2. Parallel codegen: 5.4 s (single-threaded, after the 31% of 2026-10-28) down to under 1 s, the
   single largest item in every debug number; it needs typing frames first (above).
3. The persistent compiler process for the edit loop; then the link is what is left.
