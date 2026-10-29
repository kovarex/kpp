# Kpp

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
- `stdlib/`: the standard library, written in Kpp and compiled from source with every program
  (`kppc` finds it next to the compiler; `--std DIR` or `--no-std` override that).
- `programs/`: real Kpp programs, one directory each (`kpplex`: the lexer in Kpp, with its C++
  transliteration and `run.cmd` timing both on the Factorio corpus).

## Machine
- Work happens on PC-PRACOVNA (Windows 10, Ryzen 9 9950X 16C/32T, 128 GB RAM).
- Toolchain: Visual Studio 2022 Community (MSVC 14.44, bundled clang 19.1.5, CMake, Ninja).
- Factorio source (benchmark corpus) is at C:\Factorio. Read-only: never modify or commit there.
- Repositories: this one is C:\kpp (github.com/kovarex/kpp; it replaced C:\kov and
  github.com/wube/kov, which are no longer worked in). The Factorio source converted to Kpp is
  C:\factorio-in-kpp (github.com/kovarex/factorio-in-kpp, private: it is game source), written by
  `programs\factorio\convert.cmd`; commit and push it there when a converter change alters it.
- Several threads may work in this same clone (C:\kpp): commit small, push promptly, pull before
  committing.

## Current phase
Front end (lexer, parser, resolver, type checker, template instantiation) is done and measured
(docs/prototype-results.md). Code generation for the Kpp v0 subset works through textual LLVM IR
and the bundled clang: `compiler\build\kppc.exe --exe out.exe program.kpp`. Run the end-to-end
tests with `compiler\tests\run_tests.cmd` and the specification suite `compiler\tests\spec.cmd`
(one snippet per construct, pass/pending, writes docs/spec-status.md) after every compiler
change, and `programs\factorio\convert.cmd`, which converts the whole game into C:\factorio-in-kpp\kpp,
then CommonUtil, Agui and Util on their own into programs\factorio\kpp and runs their three smoke tests (the one check that runs converted game code) and the imgui and glm ones (imgui frames through the generated C interface, the glm port against glm); the Factorio benchmark command in docs/prototype-results.md is the regression check for the
front end. A new gap found in converted code gets a pending snippet in `compiler/tests/spec/`
before it is fixed. Measure, don't guess: every speed claim comes with the benchmark command and
numbers.
