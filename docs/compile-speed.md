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
error. The fix designed with the resolver and template threads is typing frames (the types of one
instantiation in a per-thread table instead of in the AST, read through accessors by the resolver
and codegen), followed by a parallel driver with output that does not depend on thread timing. It
waits for kovarex's go, since it changes resolver.cpp. The same writes make the parallel front end
nondeterministic today: the headline moves by a few lines between identical runs, and `-j 1` gives
about 2,000 fewer spurious overload failures than `-j 32` (resolver thread's measurement).

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

## What would move these numbers most

1. The remaining codegen errors, so that the first real full-game IR replaces the extrapolation
   above (the `bad_alloc` is found and fixed; the errors are counted honestly now, about 32 k,
   most of them behind about 260 classes whose layout cannot be built: path-to-factorio.md).
2. Parallel codegen: 5.4 s (single-threaded, after the 31% of 2026-10-28) down to under 1 s, the
   single largest item in every debug number; it needs typing frames first (above).
3. The persistent compiler process for the edit loop; then the link is what is left.
