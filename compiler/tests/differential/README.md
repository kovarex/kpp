# Differential tests: kpp against C++

Each test is one program built twice, by kpp and as C++ by clang++, and both are run. Their standard
output and exit code must be the same: a difference is a silent wrong answer of kpp's, or a program kpp
no longer compiles. The programs check far more than the spec's one construct per snippet. They are
the batteries and random program generators the threads used to find most of kpp's silent wrong
answers from 2026-11-05 on (scratch\codegen, templates, build, registry, refactor, quality), kept here
so they run on every push (kovarex, 2026-11-16).

    python -I compiler\tests\differential\run.py              (debug: kpp, and clang -O0)
    python -I compiler\tests\differential\run.py --release    (kpp --release, clang -O2)

`--filter TEXT` runs the tests whose name holds TEXT, `--programs-only` and `--generators-only` one
half, `--jobs N` (8), `--shard I/N` every Nth test from the Ith (CI's 2 shards a configuration),
`--keep` keeps the sources and executables. The last line is `ALL PASSED` or `N FAILED`, and the exit
code 0 only on the first. The work is in compiler\build\differential, where a difference leaves
NAME.expected.out and NAME.kpp.out for a diff; the C++ builds' outputs are cached there by source, flags
and compiler, so a second run builds only with kpp. About 5 minutes a configuration on 12 jobs of the
Ryzen 9950X, and in CI (`differential` in .github/workflows/ci.yml) 4 jobs; kpp's gate leaves them out.

## The two C++ builds

The answer is clang++ with MSVC's target and STL (Visual Studio 2022's Clang component; KPP_DIFF_CXX
names another). Where its output differs from kpp's, the program is built again by llvm-mingw's clang++
(the windows-gnu target: Itanium's ABI, libc++; tools\sysroot\llvm-mingw.ps1 puts it in build\llvm-mingw,
KPP_DIFF_CXX2 names another), and kpp's output passes when each line is the line of one of the two.
That is what C++ leaves to the implementation or unspecified: MSVC's ABI evaluates call arguments right
to left where kpp and the other targets go left to right, the layout of bases, the last digits of libm,
the library's growth. Without llvm-mingw only the first counts.

## Programs

`programs/<group>/` holds hand-written programs, each either `NAME.body` (one text for both
languages, after the group's `prelude.h` for C++ and `prelude.kpp` for kpp) or `NAME.cxx` and
`NAME.kpps` (when the two need other names, such as std's types against kpp's library, or a header that
maps kpp's trait names onto <type_traits>). Every program prints what it checks with printf.

| Group | Areas |
| --- | --- |
| codegen | virtual calls, lifetime extension, conversions, init order, overloads, multiple inheritance, C++20 comparisons, exceptions, temporaries, elision, unions, bit-fields, pointer handlers |
| templates | concepts, SFINAE, deduction, instantiation, inlining, compile time against run time, packs, traits |
| registry | containers, Optional, Tuple, String, error codes, streams, visit, enums, integer and float edges, statics |
| stdlib-e, stdlib-g, stdlib-g2 | the standard library against MSVC's STL: filesystem, chrono, charconv, algorithms, containers, smart pointers, random, concurrency, text |
| lifetimes, language | switch and goto, special members, member pointers, operators, lookup, statics, type traits, casts, initialization, lambdas |

kpp reads the program as C++ text: it is compiled as a `.cpp` file, so its switch keeps C++'s meaning
(design.md "No implicit fallthrough": a case runs on only by `[[fallthrough]];`, which the programs
write), with `--this-pointer` (`this` is C++'s pointer). What kpp's rules refuse is written for both:
- the library is in std (design.md 16): std-names.kpp, before every program's kpp side, names what the
  programs use bare (an alias for a class or a class template; `using std::f;` for the rest);
- explicit by default (design.md "Explicit by default"): run.py drops C++'s `explicit`, and a constructor
  or conversion operator a program converts by implicitly is marked `IMPLICIT` (kpp's `implicit`,
  nothing in C++; the generators' cxx-head.h defines it away);
- the generators' KEXPLICIT, KBREAK and KFALLTHROUGH marks are C++'s explicit, break and [[fallthrough]].

`NAME.expected` (or NAME.release.expected) replaces the C++ builds' output where neither is the answer:
its `#` lines say why (clang's own mistake, a C++ build that crashes, kpp's deliberate layout), the
rest is the output, its last line `exit N`.

## Generators

`generators/gen*.py SEED ...` write a random program for a seed; run.py's GENERATORS table gives each
one's arguments and how many seeds run by default (the same seeds every run, so a failure is the
commit's). `--seeds N --first-seed M` runs others, which is how to hunt for new bugs. What a generator
writes that clang rejects (undefined behaviour, ambiguity) it drops by asking clang first, so the
programs are valid C++. They cover arithmetic over every integer and float type (each test at compile
time and at run time), structs by value across C (self-checking, linked with clang's object), SSE
intrinsics, exception flow, partial construction, temporaries in expressions, lifetimes with a throw
at each construction, aggregates, lambdas, conversions, overloads, templates, packs, inheritance with
and without virtual bases.

## A difference

A new difference fails the run. Reduce it to a spec snippet (`compiler/tests/spec/`, pending until
fixed: a silent wrong answer outranks other work) and list the program or seed in pending.txt with the
snippet's name; run.py fails when a pending one starts matching (FIXED), so the line goes with the fix.
A deliberate difference of kpp's (docs/differences-from-cpp.md) is written out of the program instead.
pending.txt says, for each program that does not match yet, which kind it is.
