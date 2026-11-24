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
  - On SysV x86-64 a struct of 16 bytes or less goes on the stack when the registers it would take ran out (two
    doubles after seven). The C++ compiler decides that again, by what the struct's eightbytes hold, so under
    `--via-cpp` the code generator writes that copy's type by its eightbytes (`byval(<{ double, double }>)`,
    bytes for an INTEGER one) and the writer passes a struct of those members. Bytes alone would be passed in
    the integer registers still free (spec interop/c_structs_by_value gave 28 for 8928).
  - A small struct the target's C passes in memory for another reason (an unaligned member, an x87 long
    double) is a storage type with an unaligned member of its own, which the C++ compiler passes in memory too.
- **Integers of other widths.** An i24 (the C calling convention's struct of three bytes) or an i40 (a bit
  field's storage) is held in a `u32` or a `u64` with its upper bits zero: each operation cuts its result to the
  width, a signed one sign-extends its operands first, a load and a store move the integer's own bytes, and what
  a C caller or callee leaves above the width is masked away. An operation not written for them (an intrinsic,
  an atomic one, a conversion to floating point) refuses them.

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
never a guess: an integer wider than 64 bits that is no i128, module-level and inline assembly other than the
code generator's own `nop`, `pause`, `yield` and `cpuid`, an alias that is no global of its module at an
offset, a volatile atomic access, a calling convention that is not C's, `indirectbr`, an intrinsic it has no
builtin for.

## Debug information

With `--debug-info` the writer puts a `#line` directive of the kpp source before each statement whose instruction
has a line, and the modules are compiled with `-g` (CodeView on Windows): the C++ compiler's line tables, a
debugger's stepping and a stack trace name kpp's files and lines. Locals are not described: a debugger shows the
translation unit's `a3` and `t17`, not kpp's variables, and no kpp types.

## Testing

- `compiler\tests\spec.cmd --via-cpp` builds every specification snippet through C++ (into `build\spec-via-cpp`)
  and compares its output as the plain run does. `compiler/tests/spec/via-cpp-known-failures.txt` lists what fails
  that way; a line that starts passing fails the run until it goes. CI runs it on every push (ci.yml's spec
  job, config via-cpp, in 2 shards).
- `run_tests.cmd`'s "via C++" (tests\viacpp.ps1, when kpp has LLVM in it) builds one program both ways and
  compares what the two print.
- The game: `kpp factorio-test debug --via-cpp` in a kpp-factorio checkout, its tests run with
  `--crc-mode write --crc-file FILE`, and the file compared with the plain build's.
- Other targets: `spec.cmd --target linux --via-cpp` and `--target linux-arm64 --via-cpp` (built on Windows,
  run under WSL, arm64 under `qemu-aarch64-static`; the sysroot has to have libstdc++, as
  tools\sysroot\linux.ps1's has). The link takes `-lstdc++` for the C++ exceptions' runtime. A kpp hosted on
  Linux has no LLVM in it, so `--via-cpp` is run from Windows.

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

Other targets, 2026-11-24, the specification suite through C++ (kpp at this commit):

| target | pass | not passing |
|---|---:|---|
| Linux x86-64 (run under WSL) | 2,353 | 6, each for the sanitizer's Linux runtime, which a build of kpp does not have |
| Linux arm64 (run under qemu) | 2,335 | 1, the same |

The final build of the game (`kpp factorio final`, full LTO), kpp-factorio 349878f, Windows:

| | plain kpp | through C++ |
|---|---:|---:|
| factorio.exe | 72.6 MB | 93.2 MB |
| its code (.text) | 40.7 MB | 54.5 MB |
| the build | 4 min 36 s | 9 min 5 s (the link 8 min 30 s) |
| the five benchmark saves' checksums | | the same on each |

The Windows program is larger because each call that may throw is a `try` block of its own, and the MSVC
exception model compiles each `catch` into a function of its own (a funclet) with its tables; one `try` a
function would take most of that back, and is not written. The Itanium model, which Linux and the Switch use,
has no such cost:

The release build of the game for Linux (`kpp factorio release --target linux`, `-O2`, thin LTO, built on
Windows), kpp-factorio 349878f, 2026-11-24:

| | plain kpp | through C++ |
|---|---:|---:|
| factorio | 87.4 MB | 90.7 MB |
| What the back end gets (280,655 functions, 64 modules) | 1,216 MB of IR | 559 MB of C++ |
| Writing the C++ | | 55.4 s of thread time on 32 threads, about 2 s of the build |
| The modules compiled (the first half of thin LTO) | 10.8 s | 29.8 s |
| The link (the second half) | 61.2 s | 109.7 s |

Speed, measured by what does not depend on the machine's load: that Linux build run under WSL in valgrind's
callgrind (scratch script `lin\run.sh VARIANT TICKS callgrind`), on the belts save
(flame_sla-10k-belt-module-1.0), at two tick counts, whose difference is the ticks alone, without the map's
loading. Two runs of the same build and tick count differ by 0.01%.

| executed instructions | plain kpp | through C++ | |
|---|---:|---:|---:|
| loading and 20 ticks | 43,805 M | 44,590 M | +1.8% |
| loading and 120 ticks | 45,732 M | 46,382 M | |
| 100 ticks | 1,927 M | 1,792 M | -7.0% |

The same with valgrind's cache and branch simulation (`--cache-sim=yes --branch-sim=yes`), 500 ticks (the
difference of 520 and 20):

| 500 ticks of belts | plain kpp | through C++ | |
|---|---:|---:|---:|
| instructions | 8,684 M | 8,061 M | -7.2% |
| first-level instruction cache misses | 4.2 M | 6.1 M | +43% |
| data reads and writes | 3,971 M | 3,929 M | -1.1% |
| first-level data cache misses | 368.3 M | 365.8 M | -0.7% |
| last-level data cache misses | 90.4 M | 95.1 M | +5.2% |
| branches | 1,130 M | 1,004 M | -11.1% |
| mispredicted | 32.8 M | 31.1 M | -5.3% |
| cycles by valgrind's usual estimate (1 an instruction, 10 a first-level miss, 100 a last-level one, 15 a mispredict) | 21.11 G | 20.89 G | -1.0% |

So the program through C++ is as fast as the plain one where the exceptions are Itanium's: it executes fewer
instructions, and its larger code costs 2 million more instruction cache misses in 8 billion instructions. The
tick's time is its data's cache misses in both. (Why the C++ compiler's IR optimizes to fewer instructions than
kpp's own was not looked into: what it tells is that kpp's IR leaves something to the optimizer unsaid.)

Wall-clock time on PRACOVNA could not tell the builds apart: the first timing of the Windows final builds
(2026-11-24, 6 runs of 1,200 ticks a save) gave the C++ build 1.05 to 1.36 times the plain build's time per
tick, with spreads up to 36%; a second one of four builds (3 runs of 600 ticks) was equal on circuits and 1.25 on
belts, the same executable swinging between 5.7 and 9.5 ms a tick from run to run (PRACOVNA's light saves jump
between two speed levels). A build without the noexcept filters' scopes of the search and one without any `try`
were not faster by a margin the noise would show, so both stay as they are.
