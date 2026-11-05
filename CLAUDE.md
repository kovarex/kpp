# kpp

A C++-like language and its compiler, designed for order-of-magnitude faster builds:
no includes, order-independent symbols, one multithreaded persistent compiler,
simple templates, own standard library, C-only interop.

## Layout
- `readme.md`, `compilation/`: the original language proposal.
- `docs/design.md`: the record of decided rules. Read it before making design choices and update
  it when a decision is made or changed. `docs/prototype-results.md`: measurements and method.
  `docs/stdlib/`: the standard library specification (self-contained; changing a rule there is a
  design decision), `docs/stdlib-conformance.md`: where stdlib/ does not meet it yet.
- `compiler/`: the compiler. `compiler\build.cmd` builds `compiler\build\kppc.exe` (CMake + Ninja +
  clang-cl from VS 2022). Benchmarks live in `compiler/bench/`, test inputs in `compiler/tests/`.
- `stdlib/`: the standard library, written in kpp and compiled from source with every program
  (`kppc` finds it next to the compiler; `--std DIR` or `--no-std` override that).
- `programs/`: real kpp programs, one directory each (`kpplex`: the lexer in kpp, with its C++
  transliteration and `run.cmd` timing both on the Factorio corpus).
- `kppnet/`: the kpp.net hosting kit (site, registry, git hosting; nothing deployed by threads);
  `kppnet/site/`: kppsite, the static site generator in kpp.
- `libraries/`: the official kpp libraries (json, sha256, base64, html, markdown, url, bcrypt, net, mariadb, http, httpclient), each a
  library project with its tests; run_tests.cmd tests them all.
- `tools/`: editor integration (VS, VS Code), release packaging. `projectlib/`: build.kpp's package.

## Machine
- Work happens on PC-PRACOVNA (Windows 10, Ryzen 9 9950X 16C/32T, 128 GB RAM).
- Toolchain: Visual Studio 2022 Community (MSVC 14.44, bundled clang 19.1.5, CMake, Ninja).
- Factorio source (benchmark corpus) is at C:\Factorio. Never modify its sources or commit there; building
  it with its own FASTBuild command line (C++ Debug/Release/FinalRelease, its libraries) is allowed (kovarex, 2026-10-31; FinalRelease 2026-11-03).
- Repositories: this one is C:\kpp (github.com/kovarex/kpp; it replaced C:\kov and
  github.com/wube/kov, which are no longer worked in). The Factorio source converted to kpp is
  C:\factorio-in-kpp (github.com/kovarex/factorio-in-kpp, private: it is game source), written by
  `programs\factorio\convert.cmd`; commit and push it there when a converter change alters it.
  Every thread reads it, so gates and timing runs write their own copy: `convert.cmd`, `tests.cmd`,
  `harness.cmd` and `run.cmd` take the checkout from their argument or FACTORIO_IN_KPP and stop
  without one. `programs\factorio\setup-checkout.cmd scratch\<thread>\fik` makes a copy once (a worktree of
  C:\factorio-in-kpp without data\ and shaders\, its game reading C:\factorio-in-kpp's data through its config:
  0.2 GB, and about 3.8 GB per configuration built, build\ and bin\). Never a junction into C:\factorio-in-kpp: git
  deletes through it (2026-11-03); `setup-checkout.cmd --unlink DIR` fixes a copy made with junctions.
- Several threads may work in this same clone (C:\kpp): commit small, push promptly, pull before
  committing.

## Rules
Decided by kovarex; every thread follows them.
- Factorio source: always the latest pull. Convert from C:\Factorio's current HEAD, never a pinned
  commit; when kovarex pulls, the next conversion moves to the new HEAD. Name the converted commit
  in every factorio-in-kpp commit message and measurement. Push a new-source conversion only once
  the headline is 0 and the game links.
- No silent fallbacks, no silent wrong answers: an unsupported construct is a compile error. A silent
  miscompile outranks every other task, the headline included.
- Spec first: every feature and fix starts with a snippet in `compiler/tests/spec/`; a gap found in
  converted code gets a pending snippet before it is fixed. A pending snippet that starts passing
  fails the run.
- One thread per worktree, under C:\kpp-sandbox (see its README.md). C:\kpp stays clean master.
  Never create top-level directories in C:\. Disk is shared: keep scratch\<thread> under ~15 GB
  (reuse fixed directory names, delete a measurement's build trees and IR dumps once its numbers
  are written down; kovarex found 900+ GB in C:\kpp-sandbox on 2026-11-01). Builds, clones, game
  copies, IR dumps and harness outputs go in C:\kpp-sandbox\scratch\<thread>, never in the session
  scratchpad (%LOCALAPPDATA%\Temp\claude\...): that is for small notes only, and kovarex cleans it. Delete with
  `cmd /c rd /s /q`, never PowerShell `Remove-Item -Recurse`, which can follow a junction into
  C:\Factorio. Rebase onto origin/master; push fast-forward when `compiler\tests\gate.cmd` passes
  (run_tests' quick run beside the spec, about 3 minutes; kovarex 2026-11-04: speed over zero risk)
  and push again without re-gating when the rebase is clean. CI runs everything (run_tests --full,
  every spec configuration, FreeBSD) on every push, and thread H runs `convert.cmd` and the game's
  tests on master in batches and bisects a red one; whoever broke master fixes it first. A risky
  codegen or converter change also needs `convert.cmd` before its push: headline 0, tests errors 0
  (kpp\tests-errors.txt: every one is a stubbed test function) and every smoke test. A gate passed
  only when each script's last line says `ALL PASSED` and its exit code is 0 (gate, run_tests, spec
  and convert end with one summary line; grepping one part's count missed a red targets check). Commit small, push
  promptly. Never `git stash`: the stash is shared across worktrees. No DCO sign-off (kovarex removed
  it on 2026-11-02); keep the Co-Authored-By line. Stage explicit paths (`git add PATH`, never `-a` or
  `.`) and check `git show --stat` against the message before pushing.
- kpp is written in lower case everywhere: docs, prose, comments, user-visible messages, the
  installer (`C:\Program Files\kpp`), release titles, the VS Code extension (kovarex, 2026-11-02). Only
  code style keeps a capital, in identifiers (`KppDescription`); file names and `.kpp` never change.
  run_tests' lower case check (tests\lowercase.ps1) fails on a capitalised one.
- Licence: kpp and libraries/ are Apache-2.0 WITH LLVM-exception (LICENSE). Not under it, and never
  relicensed by a thread: programs/factorio/ (game-derived) and factorio-in-kpp (Wube's game source).
  Third-party code keeps its own licence and notice.
- Ownership: A resolver, parser, compile speed, persistent compiler; B codegen, driver, layout,
  cimport, linking, running the game; C convert.cpp, programs/factorio, the stdlib (sole owner),
  interop, factorio-in-kpp pushes; D deduction, instantiation, overloads, checker, interpreter,
  inlining. A small hunk in another thread's file is fine after telling the coordinator.
- Threads cannot message each other: cross-thread notes go through the coordinator, who also gives
  each pending spec exactly one owner.
- Measure, don't guess: paired binaries on the same converted tree, interleaved rounds; every speed
  claim names the command and the numbers.
- CI stays green (kovarex, 2026-11-04): a push is done only when its CI run on master is green; the
  pusher checks it (`gh run list`), and a red run is the pusher's first task, before new work. A
  cancelled run (superseded by a newer push) is not green: check the newer one.
- Game speed: a kpp release (or final) build is compared against the build players get: C++
  FinalRelease of the factorio-all-in-one target (all of src in one unity file, AllInOne.cpp;
  `FBuild.exe vs2022-FinalReleasex64-factorio-all-in-one`), from the same Factorio commit; C++
  Release and factorio-run FinalRelease only as extra columns (kovarex, 2026-11-04).
- Determinism: the front end and the whole IR are byte-identical at 1, 8 and 32 threads.
  `--no-inline` output is byte-identical apart from inlining. Any inlining path outside `inlineCall`
  calls `noteInlined(written)`.
- The stdlib follows the C++ standard's guarantees; every deviation is listed in
  docs/stdlib-conformance.md.
- C:\Factorio's sources are read-only; only its FASTBuild build may write there (its build outputs),
  one FASTBuild run at a time, coordinated through the coordinator. The game runs from C:\factorio-in-kpp or a thread's scratch game dir with
  an isolated write dir; never write to %APPDATA%\Factorio.
- Line endings: resolver.cpp LF, codegen.cpp CRLF.
- Decisions go into docs/design.md; open questions stay listed there until kovarex answers.
- Reports to kovarex give exact counts and commits and link docs on GitHub
  (https://github.com/kovarex/kpp/blob/master/docs/...). kovarex never edits the repos; "solve X"
  from him is an instruction.

## Current phase
Front end (lexer, parser, resolver, type checker, template instantiation) is done and measured
(docs/prototype-results.md). Code generation for the kpp v0 subset works through textual LLVM IR
and the bundled clang: `compiler\build\kppc.exe --exe out.exe program.kpp`. Run
`compiler\tests\gate.cmd` after every compiler change: the end-to-end tests and checks
(`compiler\tests\run_tests.cmd`, its quick run; `--full` for all of it) beside the specification
suite `compiler\tests\spec.cmd` (one snippet per construct, pass/pending, writes
docs/spec-status.md). `programs\factorio\convert.cmd` (for a risky codegen or converter change, and
in thread H's batches on master) converts the whole game into a factorio-in-kpp checkout (FACTORIO_IN_KPP, a thread's own copy; src\, tests\, libraries\CommonUtil\ ..., at their paths in Factorio's repository),
then CommonUtil, Agui and Util on their own into programs\factorio\kpp and runs their three smoke tests (the one check that runs converted game code) and the imgui, glm, Lua internals and GDI+ ones (imgui frames through the generated C interface, the glm port against glm, the Lua internals bridge against the C++ build's layout, the GDI+ classes against the SDK's gdiplus.h); the Factorio benchmark command in docs/prototype-results.md is the regression check for the
front end. A new gap found in converted code gets a pending snippet in `compiler/tests/spec/`
before it is fixed. Measure, don't guess: every speed claim comes with the benchmark command and
numbers.
