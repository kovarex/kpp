# kpp's C++ back end

`kpp --via-cpp` builds a program through C++: each module the code generator emits is written as one C++
translation unit and compiled as that. It exists for a platform whose own C++ compiler has to build the
program: Nintendo's releases are built with Nintendo's clang, which will not run kpp (kovarex, 2026-11-23).
The C++ is an intermediate build step, as an object file is. Nobody is meant to read or edit it.

Decided by kovarex on 2026-11-23, from the study of three ways to get the kpp game through Nintendo's compiler:
C++ source is required (their compiler compiling kpp's IR was refused), and the C++ is written after the code
generator, not from the typed tree ("Yes, do B. The generated code doesn't have to be nice at all, it is just an
intermediate build step basically.").

## Where it sits

1. The front end and the code generator run as in any build: the program becomes LLVM IR, one module per group
   of files.
2. With `--via-cpp`, each module's IR is read back (LLVM's parser, in kpp) and written as C++ by
   `compiler/src/cppwriter.cpp`: `EXE.k.cpp` beside `EXE.k.ll`.
3. clang compiles each `EXE.k.cpp` (`-x c++ -std=c++17`), and the link takes the objects and a runtime of three
   functions.

Because the writer starts after the code generator, every kpp feature is already lowered when it runs: templates,
union enums, explicit types, arguments matched by type, `for constexpr`, compile-time evaluation, zero
initialization, the checks. A feature added to kpp later needs nothing here. What the writer translates is LLVM's
operations, about fifty kinds.

It needs a kpp built with LLVM in it (CMake's `KPP_LLVM_DIR`, as the releases are); without, `--via-cpp` is an
error.

## What the C++ looks like

| In the IR | In the C++ |
|---|---|
| A function | A function of the same symbol (`__asm__("name")`): `g7`, its parameters `p0`, `p1` |
| An `alloca` | A local array of bytes of its size and alignment |
| A value (`%t5`) | A local `t5`; one used once, by its own block, is written where it is used |
| A basic block | A label; a branch is a `goto`, a `phi` an assignment on each branch into its block |
| `load`, `store` | `*(u32_a*)p`: through a type that may alias anything, as LLVM's memory does |
| `getelementptr` | Byte arithmetic on a `char*`, the offsets folded by the module's data layout |
| Integers | Unsigned types of their size; a signed operation casts its operands |
| A struct or array as a value | A struct `T<n>` of the same layout, asserted by `static_assert` |
| A vector | The compiler's vector type (`vector_size`); an x86 intrinsic is the compiler's builtin |
| A global | A packed struct `G<n>` of its parts that are not zero at their offsets, initialized statically |
| `linkonce_odr`, `weak_odr` | `inline`, kept in the object (`used`) |
| `llvm.global_ctors` | `__attribute__((constructor))` functions, in its order |
| `invoke`, `landingpad`, `resume` | `try`/`catch`, see below |

The generated C++ is for one target, as the IR is: `config::platform`, `sizeof` and the C calling convention are
folded in before the writer runs.

## What keeps the program the same

- **Signed arithmetic wraps.** Integers are computed in unsigned types; types narrower than `int` are computed as
  `u32` and cast back, so C++'s promotion to `int` never overflows.
- **Evaluation order.** One operation per statement in the order the code generator chose. A value written in
  place is one without a side effect, whose operands do not change.
- **Floating point.** One operation per expression node, compiled with `-ffp-contract=off` as every kpp module
  is. A constant is written as a hexadecimal literal, exact; one that is not finite by its bits.
- **Memory.** No type-based aliasing: reads and writes go through `may_alias` types, and the modules are compiled
  with `-fno-strict-aliasing` too.
- **Layout.** Nothing depends on the C++ compiler's layout but the structs used as values, each asserted. Globals
  are byte-exact.
- **Globals are constants.** `-Werror=global-constructors` makes a global the C++ compiler would initialize at
  startup a compile error.
- **The C calling convention.** A C function is declared with the types the code generator lowered it to for the
  target; `sret` becomes a struct returned by value and `byval` a struct passed by value, so that the C++ compiler
  places them as the target's C does. A parameter the caller sign-extends (`signext`) is a signed type.

## Exceptions

The code generator has two exception models (design.md, "Targets and hosts" 2): Windows' structured exceptions
and the Itanium model's landing pads. C++ can express neither's tables, so through the C++ back end every target
gets landing pads (`Target::seh()` is false with `--via-cpp`), and the writer turns them into C++ exceptions:

- A kpp exception is still the 64-byte record `kpp_throw` fills. Where the IR calls `_Unwind_RaiseException`, the
  C++ calls the runtime's `kpp_cpp_raise`, which throws one C++ type, `kpp_cpp_exception`, holding the record.
- An `invoke` is `try { call } catch (kpp_cpp_exception& e) { kpp_lp = e.h; goto pad; }`.
- A `landingpad` computes its selector as kpp's personality does in the search: the first clause, in order, whose
  descriptor `kpp_eh_offset` finds among the thrown type's bases; a null clause takes everything.
- `resume` throws the record on (`kpp_cpp_resume`).

**The search stays two-phase** (kovarex, 2026-11-03: "we need to be able to report crashes like that"): the handler
is found before anything unwinds, so an exception nobody catches, and one that reaches a noexcept function, end the
program through `kpp_eh_terminate` with the throw site on the stack. C++'s own search cannot do it, since every
frame with cleanups catches. So the runtime searches itself: a call whose landing pad has catch clauses, or a
noexcept function's filter, is made inside a `kpp_cpp_scope` naming them, linked into a list of the thread's
(`kpp_cpp_scopes`), and `kpp_cpp_raise` walks that list, innermost first, as kpp's personality walks the frames.
It throws only when a clause takes the exception.

What it costs: a call in a `try` or in a noexcept function, whose callee may throw, writes three words and
this thread's list head twice. A call that only has cleanups pays nothing. A throw costs a C++ throw per frame
that has cleanups.

## What the C++ compiler has to accept

clang and GCC extensions, which Nintendo's clang has and Visual C++ has not: `__asm__` labels, `__attribute__`
(`may_alias`, `aligned`, `used`, `constructor`, `vector_size`, `weak`), `__int128`, the `__atomic_*` and
`__builtin_*` functions, `__thread`.

The flags, besides the level and the target: `-x c++ -std=c++17 -fexceptions -fcxx-exceptions -fno-rtti
-fno-strict-aliasing -fno-threadsafe-statics -ffp-contract=off -Wno-everything -Werror=global-constructors`.

## What it does not write

A construct the writer does not translate fails the module by name ("kpp's C++ back end does not write ..."),
never a guess: an integer of a width that is no power of two, module-level and inline assembly other than the
code generator's own `nop`, `pause` and `yield`, an alias, a volatile access, a calling convention that is not
C's, `indirectbr`, an intrinsic it has no builtin for.

## Debug information

With `--debug-info` the writer puts a `#line` directive of the kpp source before each statement whose instruction
has a line, and the modules are compiled with `-g` (CodeView on Windows): the C++ compiler's line tables, a
debugger's stepping and a stack trace name kpp's files and lines. Locals are not described: a debugger shows the
translation unit's `a3` and `t17`, not kpp's variables, and no kpp types.

## Testing

- `compiler\tests\spec.cmd --via-cpp` builds every specification snippet through C++ (into `build\spec-via-cpp`)
  and compares its output as the plain run does. `compiler/tests/spec/via-cpp-known-failures.txt` lists what fails
  that way; a line that starts passing fails the run until it goes.
- `run_tests.cmd`'s "via C++" (tests\viacpp.ps1, when kpp has LLVM in it) builds one program both ways and
  compares what the two print.
- The game: `kpp factorio-test debug --via-cpp` in a kpp-factorio checkout, its tests run with
  `--crc-mode write --crc-file FILE`, and the file compared with the plain build's.

## Measured

2026-11-23, PC-PRACOVNA, Windows (the MSVC target, Visual Studio's clang 19.1.5), kpp at this commit:

| | plain kpp | through C++ |
|---|---:|---:|
| Specification suite (`spec.cmd`, 16 jobs) | 2,489 pass | 2,488 pass, 1 known (142.7 s) |
| Game tests, kpp-factorio `kpp` 349878f, `factorio-test debug`: tests | 9,572, 0 failed | 9,572, 0 failed |
| Their CRC file (1,879,595 bytes) | sha256 84954fd0... | the same bytes |
| What the back end gets (338,756 functions, 256 modules) | 2,243 MB of IR | 918 MB of C++, 28.0 million lines |
| The modules compiled (`-O0`, 32 threads) | 3.8 s (LLVM in kpp, most of it during codegen) | 26.9 s (clang) |
| The whole build | 19.1 s | 39.3 s |
| The tests' run | 2 min 29 s | 4 min 38 s |

The debug program is slower through C++ because every value is a local the C++ compiler keeps in memory at `-O0`.
A release build's speed is not measured yet.
