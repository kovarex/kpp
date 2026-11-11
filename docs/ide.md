# A kpp IDE (proposal, 2026-10-30; decision 1 taken 2026-10-31)

**Decided (kovarex, 2026-10-31):** "Integrate with the Language Server Protocol
(https://microsoft.github.io/language-server-protocol/) which will allow intellisense to work in
most editors." That is decision 1 below (the query layer and the LSP adapter), built as `kpp --lsp`:
see "The language server as built" at the end. Decisions 2 to 8 (an own editor, its language, the
debugger, hot reload, ownership beyond the language server) stay open.

kovarex asked whether kpp should have its own IDE: compatible with the compiler features still to
come, made for kpp, without bloat. This is the design proposal; no code. It ends with the decisions
it needs (last section).

The short answer: the part that makes a kpp IDE better than anything else is not the editor, it is
the **query layer in the compile server**: the server already holds the whole resolved program
between builds, so navigation, references, hover types and errors for the whole of Factorio are
lookups, not analyses. That layer is worth building now whatever editor sits on it, and an LSP
adapter on it gives VS Code and Visual Studio the same navigation for one more week of work. The
own editor pays off later, and only through what VS cannot do with our output: edit-to-running-game
in one keystroke, a debugger that shows kpp values from the compiler's own types, and hot reload of
changed function bodies into the running game. Until those exist it is a nicer editor with the same
queries as VS + LSP.

## What exists to build on (measured, master 7f3e55cb)

- **The compile server** (`kpp --server`, [compile-speed.md](compile-speed.md), "The persistent
  compiler" and the steps after it): a process per (kpp binary, directory, arguments) on a named
  pipe, keeping `Build` (main.cpp: files, tokens, trees, symbol table, the resolved program, codegen's
  session) between builds. A body edit to the full game: resolve 0.07 to 0.13 s, codegen 0.10 to
  0.26 s, one module through clang 0.55 to 0.8 s; the incremental link alone 0.44 to 0.63 s
  ("The link in the edit loop"). A whole-function interface edit resolves in 0.9 to 1.0 s, anything
  the server cannot take incrementally is a build from scratch in the server (resolve 2 s).
- **What the kept tree already carries** (Ast/): every node has its first token
  (`Node::token`), every declaration its file, parent and end token, every name
  (`NameExpr`, `MemberExpr`, `NamedType`) the declaration it resolved to (`resolved`), every
  expression its type (`Expr::ty`), and `forEachNode` (Ast/AstWalk.cpp) walks a file's tree. That is
  definition, references and hover already computed; nothing has to be analysed again for the IDE.
- **The dependency records** of step 2 (each body's and instance's names looked up and declarations
  used): "what depends on this" and rename's blast radius, for free.

### What queries cost on the kept program (measured for this proposal)

A temporary probe (not committed: it ran after the resolve in `main.cpp`, behind an environment
variable, and was reverted) on a frozen copy of the converted game (factorio-in-kpp d9080751, the
run.cmd inputs: CommonUtil, Agui, cxxopts, StackWalker, Lua, src, the stdlib and the C import; 4,139
files, 1.01 M lines). Three rounds in one process; the machine was 96% busy with other threads'
builds, so these are ranges and an idle machine is faster.

| query | what it does | time |
|---|---|---:|
| walk every node, one thread | 4,550,596 nodes (711,130 of them the imported C headers) | 0.48 to 0.95 s |
| the same on 32 threads | bounded by the one 711 k-node import file | 0.18 to 0.55 s |
| build a reference index | 1,575,709 resolved names to 282,379 declarations, sorted by declaration | 0.10 to 0.14 s, 24 MB |
| references of a declaration, from the index | binary search, 100,000 lookups | 0.16 to 0.25 us each |
| references without an index | parallel walk for the most referenced declaration (17,796 uses) | 0.21 to 0.59 s |
| node at a position, by walking its file | the largest non-imported file (src\Input\InputAction.kpp, 87,134 nodes) | 7.7 to 11.5 ms |
| go to symbol | subsequence match over all 206,654 interned identifiers | 12 to 26 ms (one 65 ms outlier) |
| the resolve the queries read from | from scratch, same runs | 2.0 to 2.8 s |

And the transport: a named-pipe round trip on this machine (a C++ ping-pong, 20,000 requests,
scratchpad pipe.cpp) is **14 to 18 us median, 30 to 32 us p99**, for a 64-byte or a 4 KB answer.

So: a reference index is a tenth of a second and 24 MB, kept and updated per changed file; any
lookup in it is under a microsecond; the pipe adds 20 us; a frame is 16,000 us. Every navigation
query on the whole game can answer within one frame. What does not fit in a frame is anything that
re-resolves: the 2 s resolve from scratch, which is why the "errors as you type" design below never
falls back to it on a keystroke.

## What the IDE must do better than VS + a language server

1. **Edit to running game.** The C++ loop is 16 s for an ordinary one-line edit, 55 s for one in a
   header the PCH includes (factorio-build-baseline.md), then the game's start. The kpp server's
   pieces add up to about **1.2 to 1.8 s** for a body edit to a linked game (resolve 0.1, codegen
   0.1 to 0.26, one module 0.55 to 0.8, incremental link 0.45 to 0.6). That sum is a projection:
   the server and the incremental link have been measured separately, not yet together in one
   loop. The IDE's job is to not add to it: it knows which file changed (no stat of 4,134 files),
   sends the buffer, not a save, and starts the game itself. With hot reload (phase 4) the game
   is not restarted at all, and the loop is the build's 1 to 2 s with the game state kept.
2. **Whole-program navigation and rename, instantly.** Symbols are order-independent and global
   (design.md 2, "One global scope"), and the server holds them all, so go to definition, find all
   references, go to symbol and the package tree are lookups in what the resolve made (table above).
   No per-file guesswork and no background re-indexing: the answer is the compiler's, so it is
   right whenever the build is. Rename is the references plus the clash rule the compiler already
   checks, verified by building.
3. **Errors as you type, from the kept resolve.** A body edit re-checks one body in 0.07 to 0.13 s
   today. The IDE asks for a check, not a build: resolve only, against the unsaved buffer, results
   per file. See "Errors as you type" below for what the server must change for this to hold while
   the code is half typed.
4. **Debugging with kpp's types.** The compiler knows every class's layout (codegen computes it), so
   the debugger can show `String`, `Vector<T>`, `HashMap`, typed unions and enums as values without
   hand-written visualisers, and can evaluate constant expressions with the compiler's own
   interpreter (design.md, "Compile-time evaluation: one interpreter").
5. **Without bloat.** One process for the editor, one for the server that the command-line builds
   share anyway; start in well under a second with the server warm.

## Architecture

```
  editor (one process)                          compile server (kpp --serve KEY, one per configuration)
  - text buffers, rendering, input              - Build: files, trees, symbols, resolved program,
  - project = the build description (thread E)    codegen session, reference index (new)
  - debugger front end (DbgEng)        pipe     - query layer (new): at, definition, references,
  - LSP adapter is a separate process <------->   symbols, diagnostics, check(overlay), rename
                                                - build / run / test (exists: request kind 1)
  VS Code / Visual Studio --LSP--> kpp --lsp --+
```

### Talking to the compile server: a protocol, not in-process

The editor talks to the same server process the command-line build uses, over its named pipe, with
new request kinds next to build (1) and stop (2). In-process (linking the compiler into the editor)
was considered and rejected:

- **One kept program, not two.** The front end peaks at 1.6 GB of working set, with codegen's
  session 2 GB (compile-speed.md). An in-process compiler would be a second copy beside the server
  that `run.cmd` and thread E's VS build use; through the pipe the IDE and the build share one, and
  an edit's incremental build serves both.
- **Crashes stay apart.** A compiler crash (the server logs its stack and the next client restarts
  it) must not take the unsaved text with it.
- **The editor can be written in kpp** (below) while kpp stays C++: the pipe is the boundary.
- **Cost:** 15 to 30 us a round trip, against a 16 ms frame. Large answers (a type's 17,796
  references) are streamed in pages.

What the server needs for this, beyond the build request it serves today:

- **Concurrent queries.** Today the server has one pipe instance and serves one client at a time,
  a build included (server.cpp: `FILE_FLAG_FIRST_PIPE_INSTANCE`, one instance), so a query would wait
  for a build's clang and link. It needs more pipe instances and a reader/writer lock on the kept
  `Build`: an incremental front end (0.1 to 1 s, it grafts in place) takes the writer side; codegen,
  clang and the link run with the front end frozen, so queries proceed during them.
- **Buffers that are not saved.** A request that carries a file's text and version (an overlay):
  checks and queries see the buffer, the next build from the command line sees the disk.
- **The same key.** A server is per (binary, directory, arguments), so the IDE must start or reach it
  with exactly the build's arguments. It takes them from the build description thread E is defining
  (docs/build.md, not on master yet), the same file the VS integration reads, never its own copy.
  Debug and release (thread H) are two servers, two kept programs: the IDE queries the configuration
  being edited and builds the one asked for.

### What the server must expose (the query layer)

A new file of the server's (`compiler/src/query.cpp`), reading the kept `Build` under the reader
lock. Positions in requests are (file, byte offset), answers carry (file, line, column, length) for
display, computed from tokens on the server.

| request | answer | built on |
|---|---|---|
| `overlay(file, version, text)` / `drop(file)` | none | a text kept beside the file's, used by `check` |
| `check(files)` | diagnostics of those files: structured (file, range, severity, message, related locations), not today's printed strings | the kept resolve, speculative (below) |
| `at(file, offset)` | the innermost node, the declaration it names, its type as written, its package | `Node::token`, `resolved`, `Expr::ty`; a per-file token-to-node table (the walk is 8 to 12 ms for the largest file; a table built once per file version makes it a binary search) |
| `definition(decl)`, `declarations(decl)` | where it is defined; every redeclaration and out-of-line definition | the index |
| `references(decl)` | every use, paged; in instances mapped back to the template's tokens | the reference index (new: 0.1 s and 24 MB on the game, updated per changed file) |
| `symbols(file)`, `search(text)` | a file's outline; go to symbol with package paths (design.md, "Tools and the IDE") | the global index, the symbol table |
| `packages()` | the package tree | the files' directories and their README.md's first paragraphs (design.md, "Code packages", decided 2026-11-04) |
| `dependents(decl)` | the bodies and instances that read it | step 2's records |
| `overrides`, `callers`, `hierarchy` | virtual overrides, the call graph, bases and derived classes | the resolver's scopes; codegen's per-function request lists |
| `rename(decl, name)` | the edits, or the clash it would make | `references` and the one-global-scope check |
| `semanticTokens(file)` | each identifier's kind (type, function, member, local, constant, inactive) | `resolved` and the parser's `if constexpr` at declaration scope |
| `evaluate(file, offset, expr)` | a constant expression's value | the interpreter |
| `layout(type)` | fields, offsets, sizes, vtable, union tags | codegen's layouts (for the debugger) |
| `build`, `run`, `test` | as today, plus the list of functions whose code changed | request kind 1; `rebuildLLVM`'s changed set (for hot reload) |

Two gaps found while reading the resolver:

- **Template bodies.** References inside a template are resolved per instance in the instance's
  copy (InstanceCopier); the template's own tree has only what is not dependent. The index must
  walk the instance copies too (`forEachInstanceCopy` exists) and map each copy's token back to the
  template's, and say when a name in a template body has no instance yet (no answer is not the
  same as no references).
- **Structured diagnostics.** The checker's errors reach the driver as strings
  (`checkerErrors()`), files' parse errors as `Diagnostic`. The IDE needs all of them as data with a
  range; the printed form becomes one rendering of it, byte-identical to today's (determinism rule).

### Errors as you type

The server's incremental paths take only code that parses: step 3b requires "no parse errors and no
checker errors in the kept build", and anything it cannot take is a build from scratch that replaces
the kept build. On a keystroke that is the wrong answer twice: a half-typed line has parse errors,
and a 2 s resolve per keystroke is not as-you-type. So `check` is speculative:

- It parses the buffer and, when only bodies changed, checks those bodies against the kept program
  in a scratch copy (the graft's machinery, without committing it), returns the diagnostics and
  throws the copy away. The kept build changes only on a real build (or after the buffer has been
  quiet and clean for a moment, as an optimisation).
- A body with parse errors is checked with the error-tolerant parse's recovery (the parser already
  produces `ErrorStmt`/`ErrorExpr`); its errors are reported, and the rest of the file keeps the
  kept build's diagnostics.
- An interface edit in the buffer (a signature typed half way) is checked only as far as the step 4
  path takes it; beyond that the IDE shows the parse errors at once and the semantic ones after the
  next build, and says so in the status line. No silent stale answers: every diagnostic set carries
  the buffer version it was computed for.

Target: diagnostics for a body edit within 100 ms of the keystroke on the full game (the body check
is 0.07 to 0.13 s today, which includes per-build overhead a check does not have).

### Also speaking LSP

Yes, as an adapter: `kpp --lsp` speaks JSON-RPC on stdio to VS Code or Visual Studio and translates
to the server's requests (definition, references, hover, document and workspace symbols,
diagnostics, rename, semantic tokens all have LSP forms). It keeps VS and VS Code working for anyone
who does not use the kpp IDE, it is how thread E's VS integration gets navigation (VS 2022 hosts
language servers through an extension), and it costs about a week once the query layer exists.

The kpp IDE itself does not go through LSP: LSP is file-and-position oriented and has no form for
the package tree, build-and-run, hot reload, `layout` for the debugger or `dependents`; the IDE uses
the server's own requests. One query layer, two front doors.

### The editor: what it is built with

| option | for | against |
|---|---|---|
| **Written in kpp, Win32 and GDI text through the C import** (recommended) | dogfooding: the second real kpp program after the game, every gap it hits becomes a spec snippet; the C import already takes `windows.h` and imgui's C interface (convert.cmd's smoke tests); no runtime but ours | a compiler gap can block editor work for a day; GDI text (ClearType through `ExtTextOutW`) is enough for monospaced code, but DirectWrite and Direct2D, if ever needed, have C++-only headers: a thin C shim or their vtables declared in kpp |
| The same in C++ | no compiler risk | no dogfooding; another C++ codebase beside kpp |
| Scintilla as the text core | a mature editing control, driven by window messages (`SCI_*`), so usable from kpp through the C import | a 1 MB third-party component whose model (styled bytes, its own undo) we would work around for semantic colouring and overlays |
| An existing IDE framework (Theia, a VS Code fork, Lapce, Zed) | features at once | the bloat kovarex wants to avoid; a second language runtime; their extension model, not ours |

The core is small: a piece table per buffer (undo is its history), line starts, GDI text drawn into
a back buffer (Direct3D 11, whose headers have C declarations, only if GDI is measured too slow), a
fixed-width layout (no bidirectional text: code), file tree,
panels for references, diagnostics, output and the debugger. Windows only, as the compiler.

### The debugger

`--debug-info` (thread B, landing: CodeView in the objects, a PDB from the link) makes the game
debuggable by any Windows debugger, Visual Studio's included; thread E gets that for free.

For the kpp IDE: **DbgEng first** (dbgeng.dll, the engine WinDbg runs on, part of Windows): process
start and attach, breakpoints, stepping, threads, call stacks and source lines from the PDB through
dbghelp, all through a COM interface whose header declares a C form (the C import must take its
`DECLARE_INTERFACE` vtables: a spec snippet first). What the kpp IDE adds is **values from
the compiler**: the server's `layout` gives every class's fields, a typed union's active member from
its tag, a `Vector`'s elements from its buffer and size, without natvis files; `evaluate` runs
constant expressions in the interpreter. A debugger of our own (the Win32 debug API plus our own
PDB reading) is not proposed: months of work for what DbgEng does, until DbgEng is measured to be
the bottleneck (stepping speed, conditional breakpoints in a hot loop).

A side product for Visual Studio: kpp writes a `.natvis` from the library's layouts into every
`--debug-info` build's PDB, so the VS debugger shows kpp values too (docs/debugging.md, "Values in
Visual Studio"; it needs the PDB's types, thread B's).

**Hot reload** (phase 4, research): rebuilding a changed body is already exactly the server's fast
path, and `rebuildLLVM` knows exactly which functions' code changed (inlined callers included,
through `noteInlined`). Those functions go into a small DLL, the debugger loads it into the
paused game and patches each old function's entry with a jump to the new one (functions emitted with
a patchable entry; LLVM has `patchable-function-entry`). The hard parts: the new code must reach
the running process's globals and functions (resolved from the PDB, or the executable's own
export table), and only body edits qualify (a layout change cannot be patched into live objects).
For Factorio this is the payoff that no other tool gives us for free: edit a body, see it in the
running map without restarting and reloading the save.

## Compared with thread E's Visual Studio integration

Thread E (build model, docs/build.md, tools/vs/; nothing pushed yet at the time of writing) gives VS
a kpp project: build through kpp (through the server), errors in the error list, and with
`--debug-info` the VS debugger. What it does not give is any understanding of `.kpp` in the
editor: VS's C++ IntelliSense parses headers and translation units, and kpp has neither, so it would
report every cross-file name as undefined.

| | VS + E's integration | + LSP adapter (phase 1) | kpp IDE |
|---|---|---|---|
| build, errors after a build | yes | yes | yes |
| Rebuild, Clean | yes, kpp's `--rebuild` and `--clean` (build.md; until 2026-11-08 Rebuild only built and Clean deleted the executable) | same | yes |
| debugger | VS's, raw structs (natvis from the server: kpp values) | same | DbgEng + the server's types and interpreter |
| go to definition, references, symbols, rename | no | yes | yes, plus package tree and `dependents` |
| errors as you type | no | yes | yes |
| edit to running game | VS build + start | same | one keystroke, buffer not saved, game started or hot-reloaded |
| hot reload | no (VS edit-and-continue needs MSVC's own compiler output) | no | phase 4 |
| footprint | VS (GBs, tens of seconds to start) | same | one small process |

**Where the IDE starts paying off:** phases 0 and 1 serve everyone, VS included, and should be done
first whatever is decided about the editor. The own editor starts paying off at **phase 3** (the
debugger with kpp values, which VS gets only partly through natvis) and clearly at **phase 4** (hot
reload, which nothing else gives). Phase 2 alone is a leaner editor with the same answers as VS +
LSP; its value is the dogfooding and the foundation for 3 and 4.

One more honest limit: today the game's source of truth is C++ in C:\Factorio, and the kpp tree is
generated by the converter, so editing converted code is an experiment that the next conversion
overwrites. On the game itself the IDE pays off from the day development moves to the kpp sources;
until then its users are the stdlib, the spec snippets, `programs/` and the compiler threads.

## Compiler features to anticipate

From design.md and compile-speed.md; each needs a place in the query layer, not a special case in
the editor.

- **Code packages** (decided 2026-11-04): the package tree, the directories with their README.md's
  first paragraph on hover, is the IDE's project view; search shows package paths.
- **Tests in the compiler** (design.md 8: `test name { ... }`, `kpp --tests`, `--filter`):
  run the test under the cursor, a package's tests, failures with their operand values as
  diagnostics. Thread G's work on testing kpp code defines the runner's output; the IDE consumes it.
- **Configurations** (thread H, debug and release) and `config` constants with `if constexpr` at
  every scope: inactive branches shown as inactive per configuration, a configuration switch that
  picks which server is queried.
- **Reflection, `for constexpr`, typed unions, real enums, compile-time evaluation**: hover shows a
  constant's value and a `for constexpr` body per element, from the interpreter; the debugger shows
  typed unions by tag and enums by name.
- **The persistent compiler's next steps** (interface edits beyond whole functions, finer
  dependency records by scope instead of by symbol): every step that makes builds incremental makes
  `check` incremental too, since it is the same machinery.
- **Inlining** (`noteInlined`): the debugger maps an inlined body's code back to its callee
  (CodeView inline sites, if B emits them), and hot reload re-emits the callers that inlined it.
- **Release builds (ThinLTO)**: debugging stays on debug builds; the IDE only builds and runs release.
- **Draft types, struct/class, the syntax-without-lookup question**: they change what the parser and
  resolver accept, not the queries; semantic highlighting comes from the resolve, never from a
  separate grammar that could disagree.
- **kpp.net** (future): libraries fetched as top-level packages appear in the tree like the stdlib.

## Phased plan

Estimates are thread-weeks of one thread working on it full time, from this project's pace so far;
the ranges are honest, the upper ends assume compiler gaps found on the way.

| phase | what | estimate | done when |
|---|---|---:|---|
| 0 | **Query layer** in the server: pipe instances and the reader/writer lock, overlays, structured diagnostics, `at`, `definition`, `references` (index with instance copies), `symbols`, `search`, speculative `check` for body edits; `kpp --query` on the command line for tests and measurement; tests in run_tests (answers on tests/ programs) | 2 to 3 | the metrics below measured on the full game, the determinism rule holding |
| 1 | **LSP adapter** `kpp --lsp` and a minimal VS Code extension (grammar for colouring before semantic tokens, the language client); a note to thread E on hosting it in VS | 1 | navigation and diagnostics in VS Code on the full game |
| 2 | **Minimal kpp IDE** in kpp: window, buffers, rendering, file/package tree, go to definition and symbol, references panel, diagnostics as you type, build and run through the server, output panel | 4 to 6 | kovarex can open the game, navigate, edit and run it from the IDE |
| 3 | **Debugger**: DbgEng, breakpoints, stepping, call stack, locals and watches with the server's `layout`, `evaluate`; `.natvis` export for VS | 3 to 5 (after `--debug-info` lands) | a crash in the game stops in the IDE with kpp values shown |
| 4 | **Rename and refactors, test runner, hot reload** (research spike first: one body patched into the running game) | 2 for rename and tests; 4 to 8 for hot reload, with a real chance the spike says no | an edited body takes effect in the running game without a restart |

Minimal first version: phases 0 to 2, **7 to 10 thread-weeks**, of which phases 0 and 1 (3 to 4)
are useful on their own.

## What to measure to prove it is worth it

Each with the command and numbers, paired where there is something to compare (CLAUDE.md, "Measure,
don't guess"), on the full converted game.

1. **Edit to running game**: from the keystroke to the game's first frame with the change, for a
   body edit, a signature edit and a layout edit. Against: the same edits through VS + E's
   integration, and the C++ build (16 s and 55 s plus the game's start). Target for a body edit:
   under 2 s to a linked game (projected 1.2 to 1.8 s today), under 2 s to the change visible in a
   running game with hot reload.
2. **Query latency**, p50 and p99 over a fixed seeded set of 1,000 positions and declarations:
   `at`, `definition`, `references`, `search`, `check` after a body edit. Targets: navigation under
   16 ms at p99 including the pipe, `check` under 100 ms for a body edit.
3. **Correctness of answers**: for 500 random declarations, `references` against an independent
   count (a walk of all nodes and instance copies, which is what the probe did); every rename in
   the test series followed by a build that must succeed and match `--verify-incremental`.
4. **Footprint**: the IDE's own memory beyond the server (target under 100 MB with the game open),
   start to usable with a warm server (target under 0.5 s), the server's extra memory for the
   reference index (24 MB measured) and per-file tables.
5. **Use**: whether kovarex and the threads actually switch to it. If after phase 2 VS + LSP is what
   people keep open, phases 3 and 4 go into the LSP side and VS (natvis, a hot-reload command
   through the server) instead of the editor.

## Decisions for kovarex

1. **Build the query layer and the LSP adapter now** (phases 0 and 1, 3 to 4 thread-weeks)?
   **Decided 2026-10-31: yes, LSP** (kovarex: "Integrate with the Language Server Protocol ... which
   will allow intellisense to work in most editors"). Recommended: yes. It serves VS, VS Code and any later IDE, and the server is where the value is.
2. **Build an own editor at all**, and when? **Decided 2026-11-06 (kovarex): later, only when kpp is
   fully mature with all features implemented.** No editor work until then. Decisions 3 to 8, as far
   as they depend on the editor, are parked with it.
3. **Written in kpp** (dogfooding, compiler gaps become spec snippets) **or C++**? Recommended: kpp.
4. **Editor core**: own (piece table, GDI text) or Scintilla through its messages? Recommended:
   own; it is small, and Scintilla's model fights semantic colouring and overlays.
5. **Debugger**: DbgEng with the compiler's types, or our own debugger? Recommended: DbgEng; own only
   if DbgEng is measured to be the limit.
6. **Hot reload** in scope, as a research spike after the debugger? Recommended: yes; it is the one
   feature nobody else can give the game.
7. **Protocol**: the server's own requests for the IDE plus an LSP adapter, or LSP only?
   Recommended: both, one query layer.
8. **Ownership.** The query layer lives in the server, which is thread A's. Proposed: thread F owns
   the new `query.cpp`, the new request kinds and `--lsp`; changes to the server's locking and pipe
   instances are made with thread A's review. Thread E's build description is the IDE's project
   file (no second format).

## The language server as built (2026-10-31)

Decision 1 (LSP) is `kpp --lsp`. An editor starts it and speaks the Language Server Protocol 3.17
on its stdin and stdout; its log goes to stderr.

**Launch**, as thread E's clients start it ([editors.md](editors.md), "The language server", whose
interface this follows): `kpp --lsp` with no other arguments checks the project of the editor's
workspace folder: build.kpp, found from the folder up (or `buildFile`), gives the sources and options
of one cell, a configuration (`configuration`, debug by default) of one target (`target`), or of all
targets together (their sources and exclusions with the first one's options), with build.kpp's
`settings` (`projectCheckArguments`, compiler/src/Project/Project.cpp; those are the keys of initializationOptions and of
`workspace/didChangeConfiguration`'s `kpp`; saving build.kpp describes the project again). Without
a build.kpp the folder itself is the program. `kpp --lsp OPTIONS SOURCES` checks exactly those. A
check always adds
`--quiet --diagnostic-columns --check` and the standard library; `--exe`, `--emit-llvm` and
`--server` given are left out (logged). `--lsp-log FILE` writes the log to a file too.

**Test code is checked** (2026-11-08): when the program's own files (not the standard library's or a
library's) have a test or a suite, the cell is checked with `--tests`
(a library's cell has it already), so a test body's names have their errors, colors, hover and
navigation (`g.check(3)` in a test is a method). The first check of such a cell runs again with it
(`testsChanged`, compiler/src/lsp.cpp; ParsedFile::hasTestCode, set whether the parser parsed the test
or skipped it); once no test is left, it is checked without. A
test declaration alone (`test void helper()`) does not count: the game's `factorio` cell has one in
src/Util/ReleaseAssert.kpp and no test, and stays a build without tests (factorio-test is its tests' cell). The editor then sees the code as a
`--tests` build does: `config::tests` is true, `when (config::tests)` code is active, and an error in
main still names main, which such a build renames for the test runner's. A name the parser made in
the test machinery (a suite's registration) sits at the place of the token it was made from, and is
no occurrence: its text there is not its name.

**Cells** (kovarex, 2026-11-01: the configuration selected in the IDE "is the context of editing
the code"). A cell is a target and a configuration of build.kpp's (`projectCells`, compiler/src/Project/Project.cpp);
the server checks one at a time, the one the client selected: initializationOptions,
`workspace/didChangeConfiguration` or the request
`kpp/selectCell`, which answers with the cell or an error; `kpp/cells` lists them for a picker, and
the notification `kpp/activeCell` names the cell after each switch. Visual Studio sends its
solution configuration (`Debug|x64`, matched by the generated files' configuration names, any case)
and its startup project (the target). A cell build.kpp does not have is an error and the cell stays
as it was. Switching does not restart anything: the checked cell's kept build is parked
(`takeKeptState`, main.cpp) and the new cell's put back (`putKeptState`), so a switch back to a cell
checked before is incremental; at most `keptCells` (4) are kept, the least recently used dropped. A
cell's kept build is used only for the arguments it was made with: a change of `settings` or of
what build.kpp describes checks it from scratch. The open documents' unsaved texts stand in every
cell. Interface in [editors.md](editors.md), "Cells"; tested by tests/lsp/cells.jsonl. Hover on a `config::`
name says which cell's value the code sees; on a library's option (build.md, "Library options") it
gives the value, who set it, the default (or "required"), the type, the allowed values and the
description, from build.kpp's description of the cell (`ProjectCell::options`); with every target
checked, the options of all of them. Not done: completing option names in build.kpp (build.kpp
is not part of the checked program).

Measured on the game (factorio-in-kpp 73c8aeb1, its build.kpp, a scratch copy; scratchpad
cellsmeasure.py), target factorio: the first check of a cell 10.5 to 10.7 s (debug, then release);
switching back to a kept cell 0.12 to 0.15 s (debug) and 1.17 s (release), the switch request itself
2 ms. Each kept cell of the game holds about 6 GB (the process: 6.2 GB with one cell, 12.3 GB with
two), so `keptCells` is the memory's limit.

**How it holds the program.** In its own process, through the compile server's machinery
(`buildInThisProcess`, server.cpp): each check is a build of the kind a compile server runs, kept
for the next and incremental from it where the server's paths allow (step 3b body edits, step 4
whole functions, codegen's kept session), its output captured instead of printed. This differs from
the proposal above (the IDE talking to the build's own server over its pipe): the build's server is
keyed by arguments that include `--exe` and serves one client at a time, so a check would wait
behind a build's clang and link, and every query would need the server's new locking first. In
process, nothing in the server changed but a client-less session, and queries read the kept program
under the lock a build already takes (`withKeptProgram`, main.cpp). The cost is a second kept program
when a command-line build server for the same sources also runs (the full game: about 2 GB).

**Unsaved buffers.** An open document's text stands for its file (`setFileOverlay`, source.cpp:
`readFile` gives it instead of the disk's); closing it brings back the file on disk.

**Errors as you type.** After every change, once no other message is waiting, the program is checked
with `--check` (new): every error a build reports, codegen's included, nothing written. Codegen is
needed: the checker's type errors (`int n = "s";`, a member that does not exist) are reported by
codegen, not by the front end. A check whose codegen has no kept session to follow the edit (the
first, or one after codegen could not keep its session across errors) stops at the front end's
errors when there are any, and codegen's show once they are fixed (main.cpp, reportResolve;
tests\checkerrors.ps1). Codegen keeps no session across some errors, so while many persisted every
check generated the whole program again: the game took 17-22 s a check with 264 errors and 6-9 s with
1,602, for a front end of 0.4 s. Diagnostics are published per file, only when they changed, and
cleared when gone. `--diagnostic-columns` (new) adds the column where the error's maker knows the
token: parse errors and the resolver's checker errors (`path:line:column`); their range is the
token. Codegen's errors carry the line only (its range is the line's text): a gap.

**Queries** read the kept program: every name's `resolved` declaration and every declaration's own
name, found by token (`occurrencesIn`, one walk of the file, cached until the next check).

| request | answer |
|---|---|
| `textDocument/hover` | the declaration as written (to its body or initializer), its kind and qualified name, an expression's type, the `//` comment above the declaration |
| `textDocument/definition` | a class's definition, a member function's or a static data member's out-of-line definition, else the declaration; on `This` (alone or as `This::`), the class or enum it names, as hover does |
| `textDocument/declaration` | a definition's in-class declaration (one declaration for both: `canonical`; the resolver links a static data member to its definition only, so the server keeps the way back) |
| `textDocument/references` | every name in the program resolved to the declaration or its other declarations; an index of all files made on the first request after a check, on all threads. A virtual function's are also those of what it overrides and of what overrides it (kovarex, 2026-11-09: a call through the base is listed on an override), not a sibling's or an overload's. A constructor's are also the constructions overload resolution gives it (`chooseConstructor`, the rule codegen uses), written with the class's name or none: `T x(a)`, `T x{a}`, `T x = a`, `T(a)`, `T{a}`, `new T(a)`, member and base initializers, an argument's, a return's and an assignment's conversion; a construction in an instance's body (a library's `new T(forward<Ts>(values)...)`) is shown at the name the program's own code called to ask for the instance in the end (`emplace` in `this->suspendedContext.emplace(this)`, `makeUnique`, `emplace_back`: the resolver's root request, InstanceRecord::rootFile). An operator function's are the operators that call it (`a + b`), a conversion operator's the conversions it makes (`i32 n = object`, `if (object)`); a destructor's, its name's (no implicit destruction). A copy or move counts only for the copy or move constructor itself. 0.6 s on the game for the first constructor asked after a check (`kpp --lsp` logs each request's count and time; `KPP_LSP_TRACE` set logs every method an editor sends) |
| `textDocument/documentSymbol` | the file's outline: namespaces, classes and their members, enums, functions, variables |
| `workspace/symbol` | every declaration outside function bodies, exact names first, then prefixes, substrings and the query's letters in order; 200 at most |
| `textDocument/completion` (`.`, `>`, `:`) | after `.` and `->`: the members of the class the expression before is (names, calls, indexing and `this` chained; its bases' members after its own), each with its declaration as the detail; right after `this`, only what an object names (data members and member functions, not statics, nested types or aliases); after `X::`: the names declared in X; after `This::`, the enclosing class's or enum's statics, nested types and enumerators (an unscoped nested enum's too), its bases' after its own; else the function's parameters and locals declared before, its class's members, and the top-level names starting with what was typed (300 at most, `isIncomplete` past that) |
| `textDocument/signatureHelp` (`(`, `,`) | the overloads of the function called (a member after `.` and `->`, a scope's after `X::`), each parameter's range in the label, the argument the cursor is at |
| `textDocument/prepareRename`, `textDocument/rename` | below |
| `textDocument/codeAction` | below, "Quick fixes" |
| `textDocument/semanticTokens/full` | every resolved name and declaration's own name by kind (namespace, type, class, enum, struct, typeParameter, parameter, variable, property, enumMember, function, method), `declaration` and `defaultLibrary` (the standard library's) as modifiers; kppKeyword for kpp's own words where the parser takes them (`check(`, `require(`, `fail(`, `requireThrows<`, `checkThrows<`, `assert(`, `fallthrough;`, `nameof(`, `implicit`, `This`, `super`, `when`, `test`, `suite`, `depends`, `tags`, `generate`, `caller`, `import`...; in test code skipped without `--tests`, as a check of a program without tests saw it before 2026-11-08, by their neighbours), drawn bold in a keyword's color (Visual Studio's "kpp Keyword", VS Code's `kppKeyword`), while a method `obj.check(x)` stays a method (tests/lsp/colors.jsonl). `super` (built in, design.md 23f) is a kpp word too; its hover and go to definition are its first base's, the class (tests/lsp/navigation.jsonl) |

**Rename never misses a use silently.** Every identifier spelled like the old name in the program's
own files must be accounted for: a name the compiler resolved to the declaration (or to one of its
other declarations; a class's constructors and destructor count as the class), a qualifier naming it
(`Shape` in `Shape::area`, `Shape::count = 2` too, `this.Shape::area()`, `Shape::Alias::value` through an
alias), a constructor's member initializer naming it (or naming a base of a base),
the same names in template instances' copies; or a name resolved to something else, which is left
alone. `This` is a keyword, not the class's name: renaming a class leaves each `This` as written,
and a rename started on it is refused ("rename Shape at its name"); through `This::count` the member
renames as through `Shape::count`. A name the compiler did not resolve (a template's dependent name
with no instance), a token that means the declaration in one instance and something else in another,
or a use in the standard library makes the rename an error that names the place. It is refused too
for an imported C or a standard library declaration, a constructor, destructor or operator, a
virtual function (its overrides would keep the old name), C linkage, a new name that is no
identifier or is a keyword, and a new name that would change what another name means: a member the
class or a base already has, a name used anywhere in a local's function, and, for everything else,
any declaration outside function bodies so named (one global scope). The edits are returned, not
applied: the editor applies them, and the next check is the build that confirms them.

**Quick fixes** come from the diagnostic's message, so the compiler states each fix once, for the
command line and the editor alike. A mechanical error ends, before any parenthesized note such as
`(design.md 8)`, with one of two clauses, OLD copied from the source where the error points:
``write `NEW`, not `OLD` `` replaces OLD with NEW (``write `this.width`, not `width` ``);
``remove `OLD` `` removes it (``remove `explicit` ``, ``remove `break;` ``) with the spaces after it,
before it at the end of a line, or its whole line when nothing else is on it. The server offers the
fix only when OLD starts at the diagnostic's position or occurs once on its line; a message without
either clause has none. The fixes are `quickfix` code actions with the edit in them (`isPreferred`).

Gaps: an operator has no name token, so it is not found by position; completion and signature help
look a name up from its nearest resolved spelling before the cursor, the enclosing class and the
top-level names, not with the resolver's own lookup (a using-directive's names are missed).

**Tested** by tests/lsp.ps1 (in run_tests): a scripted session (tests/lsp/session.jsonl) on
tests/lsp/program, every message the server sends compared with tests/lsp/session.expected: the
first check, hover on a function, a data member and a local, definition, references with and without
the declaration, the outline, a workspace symbol, rename of a local, a function and a class (its uses
as a template argument too) and four refusals, signature help, semantic tokens, completion after
`.`, `->` and `::` and of a prefix, diagnostics of a misspelt member (codegen's), a misspelt name (the
resolver's, with its column), a type error, a parse error (the token's range), each cleared when
fixed, and closing a document. `--check-each-change` makes the check run after every change, so the
transcript does not depend on timing. Thread E's tools/vscode/test/lsp-smoke.js (what the editors'
clients rely on) runs against `kpp --lsp` in run_tests when Node.js is found.

**Measured on the full game** (scratchpad lspmeasure.py: a client that times each request from its
send to its answer; the converted tree of the resolver thread's worktree, 2026-10-31 19:19, frozen
copy, 0 diagnostics; run.cmd's sources and C include directories; kpp of this commit; the machine
85% busy with other threads). The first check is the whole program from scratch; a request waits
for the check of the documents as they are, so "after a body edit" is the check plus the answer.

| what | time |
|---|---:|
| first check (front end and codegen of the whole program, from scratch) | 11.4 to 11.8 s |
| check after opening a file, no change (the previous build's output again) | 0.18 to 0.41 s |
| hover, first in a file (its names found) | 6.9 ms |
| hover, definition, again | 0.1 ms |
| references, first after a check (the index of every file) | 0.72 s |
| references, again (7 results) | 0.1 to 0.2 ms |
| documentSymbol (15) | 0.2 ms |
| workspace/symbol, three queries (157 to 200 results) | 13 to 16 ms |
| diagnostics after a body edit (a statement added to `BuildEntityAchievement::gained` and taken out, three times each) | 0.92 to 2.07 s |
| of it: reading every source again / parse / walk / lexed again for the report | 0.09 to 0.23 / 0.03 to 0.07 / 0.10 to 0.27 / 0.06 to 0.19 s |
| diagnostics after typing an error (an undeclared name) | 8.0 s |
| diagnostics after fixing it | 17.4 s |

The requests of steps 3 and 4, the same tree and machine (scratchpad lspmeasure2.py), in
BuildEntityAchievement.kpp: semantic tokens of the file 7.4 ms (first) and 0.3 ms; signature help in
`TimeUtil::timeFormat(` 2.0 to 2.5 ms; prepareRename 0.1 ms; rename of `builtCount` (every
identifier so spelled in the program accounted for, the instances' copies walked) 1.1 to 2.5 s, 8
edits in one file, which are all of its 8 uses (three other classes' members of that name are left
alone); completion after typing `this->` 1.96 to 3.39 s (the check: the line does not parse, so
the front end runs from scratch and codegen not at all), 35 members, 0.2 ms when asked again;
taking the line out again 12.7 to 17.1 s (from scratch, codegen in full).

What makes the slow ones slow is where the compile server stops being incremental, not the
language server: codegen keeps its session only when the program has no codegen error
(codegen.cpp, `keepTexts`), so a check with an error is codegen in full; and a kept build with
checker errors in the edited file is not taken incrementally ("the file has checker errors"), so the
fix is a build from scratch; a file that does not parse (a line half typed) is a front end from
scratch too. On the older conversion in factorio-in-kpp (d9080751), which has 109 format errors under
today's compiler, every check after an edit was codegen in full (8 to 10 s) for the same reason.
Errors as you type need these paths to take a program with errors (codegen's session kept with the
failed functions marked, a file's checker errors kept per unit so that its walk can take them back,
a body that does not parse checked as an error body); reported to the codegen and resolver threads. Smaller items: every check reads all 4,126 sources again although the
language server knows which document changed, and lexes them again for a report nobody reads.

Since then (resolver thread, [compile-speed.md](compile-speed.md), "Edits with errors stay
incremental"): a file with checker errors and a body that does not parse stay on the incremental
path, the sources are read only when their stamp changed, and nothing is lexed again under
`--quiet`. The same measurement: completion after typing `this->` 0.31 to 0.40 s, taking the line out
0.49 to 0.72 s, a body edit 0.39 to 0.44 s; an error typed and fixed were still 7.7 to 10.8 s, codegen
in full both times, until codegen kept its session across errors (006315e6): with both, master
d336a43e, every check of the session is 0.17 to 0.44 s (an error typed 0.35 to 0.37 s, fixed 0.37 s,
`this->` 0.17 s, the line taken out 0.35 to 0.42 s).
