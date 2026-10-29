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

## End to end on the full game (2026-10-29, night)

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
  map file alone.
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

## Design: the persistent compiler (for review)

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

**Open questions for review.** Whether the server should be on by default from step 3 (with
`--no-server` as the way out), or opt-in (`--server`) until the incremental test has run for a
while; I would make it opt-in until step 4. And whether the client may start a server on its own
or the user starts it (a long-lived process left running by a build command surprises people).

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
