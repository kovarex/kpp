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

### Design: instantiation checks as tasks (steps 1 and 2 done, step 3 next)

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
- Step 2 (932b4c4 and the follow-up): the output does not depend on the thread count, as above.
  Cost: none measurable at 32 threads (median 1.07 -> 1.12 s, within the noise); `-j 1` 7.9 ->
  7.5 s.

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
