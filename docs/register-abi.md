# kpp's register convention (a prototype)

Study and prototype of thread "Try vectorcall-style calling convention in kpp" (kovarex, 2026-11-27, from rseding's
hint: C++ compilers keep passing small objects through memory because they cannot break their ABI, and MS x64's
default convention passes far less in registers than `__vectorcall`). kpp owns every caller of its own functions, so it
can pass them in registers. Branch `claude/project-thread-ickng2`, draft pull request #39. Not on master.

## What kpp does on master

- Every class by value, of any size, is a pointer to a copy the caller makes; the callee owns and destroys it. An
  8-byte `MapPosition`, a 1-byte `ForceID`, a 16-byte `Vector` or `StringView`, a `float32x4` all go through memory.
- Every class result goes through a result slot the caller passes (`ptr`), also an 8-byte one.
- A `const T&` is a pointer, as in C++.
- C functions (`extern "C"`, imported headers) follow the target's C convention (cPassing, design.md "Stage 2").
- Game census (kpp-changed b8009b9be9, the 2026-11-27 nightly's final build, pre-LTO IR): 27,978 of 286,839 defined
  functions take a class by value; the most common are `std::StringView` (2,915), `std::String` (1,177), the
  prototype IDs (`ID<...>`, `IDWithQuality<...>`), `MapPosition` (558), `ForceID` (489), `MapTick` (412),
  `TilePosition` (286), `Color` (284), `Vector` (179), `BoundingBox` (64), `float32x4` (68), `float64x2` (57).
- The C++ game marks its SSE helpers `__vectorcall` (`VCALL`, 127 uses); kpp accepts `[[vectorcall]]` and does
  nothing with it, and the converter drops `VCALL`, so those functions take their `float32x4` through memory in kpp.
- The final configuration is full LTO, whose argument promotion turns some pointer parameters of internal functions
  back into values (seen in the game: `Vehicle::updatePosition(MapPosition, VectorOrientation)` gets its
  `MapPosition` in RDX, but still reads the `VectorOrientation` through R8, as that load comes after a call). It cannot
  for virtual functions, functions called through a pointer (`std::Function`), loads after a call that may write
  memory, more than two values per parameter, and never for results: LLVM has no pass that returns an sret slot's
  contents in registers.

## The prototype

`KPP_REGISTER_ABI=off|vectorcall|sysv|c` (unset is `vectorcall` on the branch, so that CI runs everything with it).

- **Which classes:** no destructor, no copy or move constructor of its own (relocatesBitwise), not polymorphic, not
  packed, not a closure; up to 32 bytes as a parameter (at most 4 parts), 16 as a result (at most 2 parts).
- **How:** by SysV's eightbyte classes: an integer part (`i64`, or `iN` for the last bytes), a float, `<2 x float>`,
  a double, a 16-byte vector (`<4 x float>`); an empty class is no part at all. The callee stores the parts into a
  slot of its own, which the body has as kpp's pointer to a copy; a result is built in a slot of its own and returned
  as an LLVM struct of its parts (registers).
- **Which functions:** a kpp function (not a C one, not variadic, not a closure's call operator, not through
  `--via-cpp`) that takes or returns such a class. A function that passes none keeps the target's C convention: a
  function type that passes no class by value is one type for kpp and C, and C may call such a function.
- **The LLVM calling convention on Windows x64** (elsewhere the target's C one, already SysV or AAPCS64):
  - `vectorcall` (x86_vectorcallcc): MS x64's callee-saved registers; floats and vectors in XMM0-5, each also using
    its position's integer register; integers in RCX, RDX, R8, R9 by position; results in RAX:RDX or XMM0-3. LLVM
    names such a function `NAME@@BYTES`.
  - `sysv` (x86_64_sysvcc): 6 integer and 8 vector registers, not by position. **Unsafe on Windows:** LLVM gives a
    function's catch and cleanup funclets the callee-saved registers of the function's convention, SysV's, while
    Windows' exception runtime calls funclets as MS x64 functions. clang 19 for x86_64-pc-windows-msvc compiles a
    `sysv_abi` function's catch block that keeps values in ESI, EDI and XMM6-10 without saving them, corrupting the
    runtime's registers.
  - `c`: MS x64's own convention, four positional registers, a 16-byte vector through a pointer.
- **Where:** one mechanism, CCallingConvention.cpp: `registerPassing` classifies, `withRegisters` lowers a
  `Signature` (`registers`, `cc`, `lowered`, `result`), `emitKppCall` emits a call (loads the parts from the
  argument's copy, stores a result's parts into the caller's slot); definitions (emitFunction), declarations, direct,
  virtual and pointer calls, member pointers, vtable adjusting thunks, a captureless lambda's thunk and the calls
  written by hand (postfix `++`, `<=>`, a range for's comparison, `operator=`, `operator==`) use it.
- **Checked:** CI of the draft pull request with the convention on (every suite: run_tests, the spec in debug,
  release, debug-info and windows-gnu, the differential suite, Linux, arm64, macOS, FreeBSD): green on 690355a2f.
  A checker of every direct call against its callee's definition (cc, result, parameter types), which LLVM's
  verifier does not do under opaque pointers.

## Study result (2026-11-27, before any game measurement)

Published for kovarex: https://claude.ai/artifact/M1aWSnPn5MRbWfmyjPG7tB. Expected gain for the game 0.1-1.2%, most
likely about 0.3%, under the benchmark's 2% noise: full LTO inlines nearly all small functions; the hot entity
classes' surviving procedures rarely take a position by value (inserters 1 of 104, robots 4 of 114, belts 11 of 220,
crafting machines 0 of 217); a memory round trip costs about 3 ns only on a dependency chain (C++ micro: a chained Vec2
call 4.3 ns through memory, 1.1 ns in registers; a Pos in one register 1.3 ns against 0.9 through memory); kpp-changed
is already 1.6% faster than C++ FinalRelease (nightly 2026-11-26). Recommendation: drop the convention as a speed
project, keep this branch as a reference; the cheap step is `noalias` on kpp's result slots and argument copies (no
ABI change: LLVM's own argument promotion then works in full LTO, and the dead zeroing of result slots goes).
