# kpp design

kpp is a C++-like language with a new compiler built around one idea: **no includes at the
deepest level of how the compiler works**, so that builds are orders of magnitude faster.
The original proposal's compilation model is [compilation/index.md](../compilation/index.md); its
language changes are in [readme.md](../readme.md), "The language", each with its state.

This file records decisions that have been made. When a decision changes, update it here. The rules
that differ from C++, for a reader who knows it, are in
[differences-from-cpp.md](differences-from-cpp.md).

## Decided

### 1. A native compiler, not a layer over Clang
- Our own front end (lexer, parser, name/type resolution, template instantiation, constexpr
  interpreter). LLVM is used only as the code generation backend.
- Rejected: Clang fork, or a build driver that generates C++20 modules / includes behind
  the scenes. Clang's declare-before-use, one-file-one-thread model cannot deliver the goal.

### 2. Compilation model
- **No `#include`, no forward declarations.** Every project symbol is visible everywhere.
  The project is a set of files (a "scope expansion path"), not an include graph.
  A class that is declared but never defined in the program (a C interface's handle type, a
  converted forward declaration) is an opaque class: a pointer to it is passed, stored, compared
  and cast like any pointer (a cast between it and another class pointer reinterprets, never
  adjusts), and only a use that needs its layout (an object of it, its size, arithmetic on such a
  pointer) is an error (2026-10-31; spec classes/pointer_to_declared_only_class_is_opaque).
- **Order-independent.** Declaration order does not matter (except runtime static
  initialization order within a file). Circular dependencies that cannot be resolved
  (e.g. type A contains B by value and B contains A) are reported as errors.
  Globals built at startup are built in definition order within a file (a global's
  initializer may read, directly or through functions, the globals defined before it in its
  file); files go in the driver's order, the library first, and a program must not depend on
  that order across files (as C++ across translation units). A global the interpreter can evaluate
  is constant initialization, as in C++ ([basic.start.static] 2): whatever reads it, in any order, has
  its value (constant data; a reference to an object of static storage is that address; an object
  with vtable pointers, which no constant of the shared text can name, is built before every other
  startup function, its constructor being one the interpreter ran, spec
  language/constant_initialization_of_polymorphic_global). Every one is built, whether
  anything reads it or not (an initializer that registers something, as C++ runs it): globals
  of namespaces and static data members of classes that are no templates; a static member of
  a class template is built when an instance of it is used, and every one of an instance an
  explicit instantiation names (`template class LuaStruct<MapSettings>;`, as C++ instantiates
  all its members), built where the explicit instantiation is written, in the order of the
  members' definitions. A `thread_local` (a global, a
  static member or a local) is one object per thread, built by each thread at its first use
  (a local at its first pass), not at startup, and destroyed when the thread ends, the last built
  first, with the temporaries its initializer extended after it (2026-11-06: kpp_thread_atexit, on
  Windows a fiber-local slot's callback, elsewhere the C library's __cxa_thread_atexit; one built
  from constant data is built at first use too, so that its destruction is registered; spec
  language/thread_local_destroyed_at_thread_exit). The main thread's are destroyed as it ends,
  before the statics (on MSVC targets through the Visual C++ runtime's __tlregdtor, on ELF the C
  library's; windows-gnu keeps the fiber-local slot, whose callback runs after them).
  Objects of static storage are destroyed at the program's end as in C++ (kovarex, 2026-11-06;
  [basic.start.term]): when main returns or exit() is called, in the reverse order of the completion
  of their construction, atexit's functions in that one order. Each is registered where its
  construction completes (kpp_atexit, in the same function as the initialization: __cxa_atexit on
  ELF, atexit and a list on Windows), a function-local static when its declaration is first passed,
  an object of constant data that needs destroying at its place in the startup order, and it is
  never in read-only memory; the temporaries a static's initializer extended go after it. A
  destructor that lets an exception out ends the program through terminate. quick_exit, _Exit and
  abort destroy nothing. Specs language/globals_destroyed_at_exit,
  globals_destroyed_by_a_call_of_exit, destructor_of_a_global_that_throws_terminates.
- **One multithreaded compiler process** sharing one symbol table, instead of independent
  per-file processes:
  1. Index: parse all files in parallel, build the global symbol tree, record unresolved
     references, keep function bodies.
  2. Resolve: once everything is indexed, resolve references and types.
  3. Codegen: with sizes and the full tree known, generate code. No separate link step.
- **Each template instantiation is generated exactly once** (reuse / wait for the thread
  producing it / produce it).
- **Persistent compiler.** It stays running with the program model in memory, recompiles
  precisely what changed, and serves IDE queries.
- No macros and no preprocessor (decided 2026-10-23 after the Factorio survey in
  [macro-survey.md](macro-survey.md)): build configuration is a set of typed `config` constants,
  and every use of macros is covered by the features in section 7.

- **Inlining across chunks** (decided 2026-10-28, to be done later): kpp hands clang the program
  in chunks (IR modules) compiled in parallel, and a non-template function's body is in one chunk
  only, so calls from other chunks cannot be inlined (up to 35% on small String methods,
  docs/stdlib-performance.md). The options were (1) ThinLTO at link time, (2) available_externally
  copies of small functions in every chunk that calls them, (3) grouping callers with callees into
  chunks, (4) kpp inlining trivial functions itself when it emits IR. kovarex: (1) or (4); (2)
  rejected, since the copies grow with the number of chunks. One chunk (`--modules 1`) remains the
  manual extreme for a release build.
- **Floating point is strict IEEE** (the game builds with /fp:strict for its determinism): kpp
  emits no fast-math flags and no `contract`, and the back end (clang, or LLVM's in kpp) compiles
  the modules with `-ffp-contract=off` for the baseline x86-64 target (no FMA), so no multiply-add is fused and
  every operation rounds as written.

Expected limit: once headers and duplicate instantiations are gone, LLVM optimization
dominates release builds. The big wins are debug builds and incremental rebuilds; release
builds win less unless machine code is cached per function.

### 3. Simple templates
- Templates are for generic types and functions (`Vector<T>`, `max<T>`), not a
  metaprogramming language.
- Kept: type and value parameters, variadic parameters, full specialization,
  `if constexpr`, constraints written as plain conditions (`requires`-like).
- SFINAE stays, for C++ compatibility (kovarex, 2026-11-07, F's question: imported C++ libraries rely on
  it): a candidate whose substitution fails is removed, as C++ removes it. kpp's own code writes
  `requires`; `EnableIf` and `VoidT` stay in the library for imported or unconverted code.
- Dropped: template metaprogramming as computation, `typename` / `template` disambiguators.
- Compile-time computation is done with ordinary constexpr code run by an interpreter.
- Factorio survey: almost no SFINAE (2 `enable_if`, 1 `void_t`), but 593 full
  specializations, plus `if constexpr`, `requires` and variadics, so those stay.
- Constraints (2026-10-29): a requires clause is evaluated for a call's deduced arguments, its
  `!`, `&&`, `||` and parentheses over parts that are traits (`isIntegral<T>`), concepts, boolean
  variable templates (`hasBegin<C>`), requires-expressions (each requirement must type-check for
  the arguments) and any other constant expression (`N1 * D2 <= N2 * D1` over deduced constants).
  A part whose arguments do not form for the known arguments (`decltype(declval<T>().str())` for a
  T without str()) is false, as C++'s substitution failure is. A clause naming something not
  declared is a compile error, and so is a clause that cannot be evaluated once every argument is
  known: never a silent pass. While an argument is still dependent the clause waits.
  A constrained template parameter (`template<SmallInt T>`, `template<SameSize<int> T>`, a pack
  `template<Small... Ts>`) is its concept checked as a requires clause would be, `C<T, X...>`, each
  element of a pack (2026-11-05: it was parsed and never checked, so `template<SmallInt T> g(T)`
  took g(2.5)). Of two functions alike but for their constraints (the same parameters, template
  parameters taken by position) the more constrained is chosen, by C++'s subsumption
  ([temp.constr.order]): the constraints normalized through the concepts they name, an atom being an
  expression with the arguments it is evaluated for, so that the atoms a concept defines are the
  same wherever it is named while `requires integral<T>` written in two templates are two atoms;
  constraints neither of which subsumes the other leave the call ambiguous, an error (specs
  templates/more_constrained_overload_is_chosen, templates/constraints_that_neither_subsume_are_ambiguous;
  the first declared was taken until then). A class template's constrained parameter is checked
  where the specialization is named, one its argument does not satisfy a compile error
  (templates/class_template_constraint_is_checked; the game's InfiniteRectangle<ConvertibleToBool T>
  is given pointers, which satisfy it).
  A partial specialization's constrained template parameters are checked for the arguments it
  deduces: one whose constraints do not hold does not match, and of two with the same patterns the
  one whose constraints subsume the other's is taken (2026-11-06: they were never checked, and the
  first declared took every argument; templates/constrained_partial_specialization). A
  requires-expression's compound requirement checks its result's type-constraint (`{ t.size() } ->
  Integral` is Integral<decltype((t.size()))>) and a nested requirement (`requires sizeof(T) <= 2;`)
  must evaluate to true (2026-11-06: the first was skipped, the second only type-checked;
  templates/requires_compound_and_nested_requirements). A constrained placeholder parameter
  (`Integral auto x`) is its invented template parameter's type-constraint; one anywhere else (a
  variable's, a result's) is an error until it is checked (templates/constrained_auto_parameter,
  templates/constrained_auto_variable_is_not_supported_yet).
- Deduction from several arguments (2026-11-05): a template parameter that two arguments deduce
  gets the same type from each, else the template is no candidate ([temp.deduct.type] 2): `f(1,
  2.5)` of `template<class T> f(T, T)` takes another overload or has none. The first deduction stood
  until then and the other arguments converted to it (`larger(1, 2.5)` gave 2; spec
  templates/parameter_deduced_twice_must_agree).
- Partial specializations (2026-11-05): an array type is a template argument (`Kind<int[3]>`,
  `Kind<T[N]>`, `Kind<T[]>`, `Kind<T[N][M]>`; `Take<table[2]>` stays an index for a constant
  parameter, as `Frac<gcd(N, M)>` stays a call), so a partial specialization on arrays matches
  them, the more specialized of `T[N]` and `T[N][M]` taken (templates/array_types_as_template_arguments;
  the parser read `T[N]` there as an index and `Kind<int[3]>` silently took the primary). Two
  partial specializations that both match and neither more specialized than the other are
  ambiguous, a compile error where the type is named (templates/ambiguous_partial_specializations_are_error;
  the first declared was taken).
- Class-type template parameters (2026-11-06, C++20 [temp.param] 6): `template<Fixed F>` takes a
  constant of the class, a named constexpr object or a brace list (`scaled<Fixed{3, 14}>()`, which
  the parser reads as an expression there). The argument is the object's value, its parts (bases,
  then data members) each an integer, bool, char or enumeration value or an object of such parts
  (TypeTable::objectValue, printed `Fixed{3, 14}`): arguments of equal parts are one instance. In
  the template the parameter is a constant lvalue of that value, one object per value (codegen's
  linkonce_odr `template.object.Fixed{3, 14}`, so `&F` is the same in every instance given it); the
  evaluator reads it as the object. A part of a floating, pointer or reference type gives no
  argument yet (an error at the use). Spec templates/class_type_template_argument, clang's output.
- Variable templates (2026-11-02): a use is the instance's value, the initializer evaluated for
  the use's arguments. Its object (`&hasBegin<C>`, a reference or a Text piece bound to it) is a
  constant global per argument list holding that value; a builtin trait (`isSame<A, B>`) has none,
  and a reference binds a temporary. One that is not a const value has its own object per argument
  list (2026-11-06; one global named by the template alone read `hasBegin<Vector<int>>` as false);
  one whose initializer is no constant, built at startup, is a compile error so far.
- Instantiation depth (implementation limit, 2026-10-28, open to change by kovarex): a function
  instantiation requested through a chain of more than 64 instantiations, each asked for by the
  previous one's body, is an error naming the last two. Without a limit, a template that
  instantiates itself with ever larger arguments never stops, and every step costs more because
  the names grow (the first case, from a wrong deduction, ran for over ten minutes). C++ allows
  1024 nested instantiations, but counts fewer of them (a function body's calls are instantiated
  later, not nested); kpp counts every link of the chain, and the whole Factorio source needs 12.
  Templates as computation are dropped (above), so deep recursion is not expected.

### 4. Our own standard library
- No C++ std. A smaller, saner library written in kpp: one UTF-8 `String` with small-string
  optimization (no traits/char-type/allocator template parameters; allocators passed at
  runtime), Vector, SmallVector, HashMap, Optional, Span, UniquePtr, Function, formatting,
  sorting, file/OS wrappers. No iostreams.
- Factorio's in-house containers (FlatMap, SmallVector, TinyVector, IntrusiveList...) are
  the starting point.
- `strong_ordering` is what `a <=> b` yields (std::strong_ordering, weak_ordering and
  partial_ordering all convert to it), compared with 0 as in C++. The comparison rewrites of
  C++20 hold: a class with `operator<=>` has `<`, `>`, `<=` and `>=`; a defaulted one orders by
  its bases and members in turn and gives `==`; `a != b` is `!(a == b)` for every class.
  (Done 2026-10-25.)
- `a != b` is only ever `!(a == b)`: declaring an `operator!=` that returns bool is an error, a member or
  a free function, written, defaulted or a template (kovarex, 2026-11-09: it stops people and machines
  from writing bloat by habit; a written one can only repeat `==` or disagree with it). The converter
  drops the game's, the stdlib and libraries lose theirs, then the checker refuses them (pending spec
  kpp/operator_not_equal_returning_bool_is_error). An `operator!=` returning another type stays allowed
  (kovarex, 2026-11-09): a SIMD mask or an expression object, where C++'s rewrite does not apply; the
  converted game has six, in src/Util/SSE.hpp (float32x4, float64x2 and int32x4 give their mask).
  The converter's step (C, 2026-11-09; ProgramPasses.inl's withNotEqualsDropped, on each output file after the
  out-of-line bodies join their declarations): a bool `operator!=` goes where it is provably the negation of its
  scope's `==` of the same parameters: `= default`; `!(*this == p)`, `!(p1 == p2)`, `!operator==(...)`; a body
  `x != y || ...` (or `|` between parenthesized terms) whose `==` is `x == y && ...` over the same pairs; or each
  data member's term beside a defaulted `==` in a class without bases and arrays. With no `==` of its parameters it
  becomes that `==`, its body negated (`x != y || ...` as `x == y && ...`, `!x` as `x`, anything else `!(...)`).
  Kept, each a TODO line in CONVERSION.md: virtual ones (ModSetting and its five settings, whose chain spans
  files), a body of more than one return (asserts, early returns), one with no body in its output file, an `==`
  written through `!=`. In the game (from C:\Factorio 4c178a5b915): 170 dropped (21 defaulted, 54 negations, 95
  by their terms), 42 made the `==`, 18 kept for review besides SSE's six. Its IR before and after: every function
  only before is an `operator!=` (and one `==` instance now inlined everywhere), every one only after an
  `operator==`, and no call reaches another function than the `==` of a dropped `!=` (24 differ by that `==`
  inlined). tests\convert_notequal checks each form against C++'s output. The 18 are hand edits of the C++ it
  reads (programs/factorio/overrides/source-edits.txt, C, 2026-11-09): ModSetting's `==`, written
  `!(*this != other)`, is virtual and the chain's five overrides are `==` (`super::operator==(other) && ...`);
  ModSettingsTemplate's and LogisticSections' `==`, written through their `!=`, take its body with each result
  negated; GapFiller's iterator equals its end (its three results negated, integers); TechnologyReference's,
  EditorTool's, CompactDeque's iterator's, IDWithCount's and ImmutableString's `!=` were their `==` negated
  (the same asserts) and go; InputAction's and EditorAction's two empty-action checks become `==`. The game
  then has no `operator!=` returning bool.
- Structured bindings are tuple-like through the library's `TupleSize` and `get` (std::tuple_size,
  std::get), as in C++: when `TupleSize<E>` has a member named `value` (a full or partial
  specialization for E; the primary `TupleSize` is empty), `auto [a, b] = e` names exactly
  `value` names, and binding i names a hidden `auto&& a = get<i>(e)`. That is `e.get<i>()` when E
  has a member function template `get` whose first template parameter is a constant, else a `get`
  of the global scope or of E's namespace (argument-dependent lookup), never a member or local
  named `get` where the binding is written. Any other object (an array, a class without
  `TupleSize<E>::value`) binds its elements or data members in order, as before. Two
  simplifications against C++: e is passed to get as an lvalue (C++ passes an xvalue when the
  hidden object is no reference, to pick a `get(E&&)`; the library's get takes `E&` and
  `const E&`, which name the same element), and a binding's type is what get gives without its
  reference, which is also what decltype(a) gives (C++ takes it from std::tuple_element<i,
  E>::type, which the library need not declare; the two agree whenever get returns the element
  by reference). A get that returns a value gives the binding that value, which lives as long as
  the binding. A count other than `value`, or a `value` that is no constant, is an error.
  (2026-10-29: specs language/structured_binding_tuple_protocol, _tuple_forms and
  _tuple_size_mismatch.) The hidden object of `auto [a, b, c] = arr;` (no ref-qualifier) given an
  array is an array of its type, cv from the declaration, each element copied from arr's (by its
  copy constructor; moved from an xvalue), as C++ has it ([dcl.struct.bind] 1), where an `auto`
  variable's type decays to a pointer; a range-for's `auto [u, w]` over arrays copies each element
  alike (2026-11-05, from B: it was a decayed pointer, which codegen refused; spec
  language/structured_binding_copies_array).
- Each library type that a std type converts to keeps the guarantees the C++ standard gives that
  type (iterator, pointer and reference invalidation, complexity, the meaning of return values),
  since converted code relies on them. A faster type that breaks one is a separate, explicitly
  named type (FastMap next to Map). The rules are written out, self-contained, in docs/stdlib/
  (the specification); docs/stdlib-conformance.md lists where the code does not meet it yet.
  (kovarex 2026-10-28.)
- FastMap's and FastSet's entries move without throwing: an entry whose move constructor may throw
  is a compile error (a static_assert in BTree), since a move that threw while entries shift would
  leave the tree with a hole and its keys out of order, past repair (K, 2026-11-09, from the
  coordinator's go-ahead; spec library/fastmap_entry_that_may_throw_when_moved_is_error,
  docs/stdlib/fastmap.md). Map and Set take any entry.
- Containers move elements as the C++ ones do: when Vector grows or shifts, HashMap rehashes or
  stableSort merges, each element is move-constructed at its new place and the old one destroyed
  (`relocate` in Utility.kpp), so a type that keeps its own address can fix it in its move
  constructor. For types whose moves are plain copies clang turns this back into memcpy.
  (kovarex 2026-10-28; Vector used to copy bytes without calling constructors, which cost
  10-17% on growing a Vector<String>, docs/stdlib-performance.md.)
- The game's 2D `Vector` class is `Vector2D` in the conversion, since `Vector` is the library's
  std::vector (decided 2026-10-25; the converter renames the identifier).
- Sizes are unsigned (kovarex 2026-11-01: "all the sizes should be unsigned not signed"): every
  size, count, capacity, length, index and position is a `ContainerSize` (stdlib/core/Sizes.kpp).
  Differences, offsets and comparison results stay signed. npos is the largest size; past the
  limit a container throws LengthError (docs/stdlib-conformance.md, "Sizes are unsigned").
- **Container sizes are u64** (decided 2026-11-01; section 13, "The size width", has the
  measurement): `ContainerSize` is `u64`, with no switch, so there is one String, one Vector and so
  on. kovarex's rule: "If it doesn't make a difference, it would be more practical to just have one
  String/Vector etc". H measured u64 against u32 on the game: release 0.983x on the 45 menu maps
  and 0.994x on the unit tests, debug 1.001x and 1.005x, the exe +1.3%, peak commit +6 MB, CRCs
  9,501/9,501 with 0 mismatches against C++ Release. The u32 width and its
  `kpp --config sizeBits` switch were removed (the option is now an error). String is 32 bytes,
  Vector 24.

### 5. Interop: C only
- Import C headers / C ABI (OS APIs, SDL, Lua, zlib, openssl, curl...).
- C++ libraries (glm, imgui, tracy, re2, Steam SDK...) get a thin C wrapper or a port.
- Mechanism (decided 2026-10-23): `extern "C" import "header.h";` anywhere in the program. The
  compiler collects all imports, runs clang's preprocessor over them once (cached by content hash
  under `%TEMP%\kpp-imports`), parses the result in C mode as one more file of the program and
  turns object-like macros that look like constants (`EOF`, `MAX_PATH`, `HEAP_ZERO_MEMORY`) into
  `const auto` declarations. Function-like macros are not imported. Inline functions in headers are
  declared, not compiled: the library provides them (the link line adds
  `legacy_stdio_definitions` for the UCRT's inline printf/scanf family). `--cinclude DIR` adds
  include directories for the imports.
- **Converted code imports once per library (kovarex, 2026-11-02).** An import is the whole program's
  wherever it is written, so the converter writes no import into a converted file: each library's
  (each converted root's) C headers are imported once, in its `c-imports.kpp` (src/c-imports.kpp for
  the game), in the include order of its files, not sorted (a Windows header may need another
  before it: dbghelp.h and psapi.h need windows.h). stdlib.h, string.h and stdio.h, which C++'s
  standard headers bring in, are imported only where a file uses a name of theirs without including
  them, the names in a comment. The tree imported string.h 827 times, stdlib.h 800, stdio.h 778 and
  stdint.h 400, 3,378 imports in all; it now has 225 in 17 files. Hand-written files keep their own
  (a binding's import is part of it).
- C constructs accepted for this: unions (one block of storage, every member at offset 0),
  anonymous struct/union members, `typedef struct { ... } Name;`, `(void)` parameter lists,
  MSVC's `__intN`, calling conventions and `__declspec`. Imported declarations never appear in
  parse statistics or diagnostics unless `--errors` is given.
- MS C's anonymous member of a named struct type (`MONITORINFO;` inside winuser.h's
  MONITORINFOEXW, whose C++ version derives from it): as a struct's first data member it becomes a
  base at offset 0, as C++ has it, so the layout is C's and its members are the struct's; anywhere
  else it is a member with a name of its own (the layout stays right; its members by their names
  are an error). (2026-10-29, spec interop/c_anonymous_member_of_named_type.)
- `#pragma pack` in an imported header lays its structs and unions out as the C compiler does, every
  form of MSVC's and clang's (`pack(n)`, `pack()`, `push`, `pop`, labels, `show`, `_Pragma`): the import
  gives each definition under a packing `__kpp_pack(n)` after `struct` (applyPragmaPack, in the cached
  import), and a member's alignment is at most n. A required alignment (`__declspec(align)`, an SSE
  vector, or a class holding one) stays on MSVC's layout and gives way to n on Itanium's (clang's two
  layout builders: CONTEXT stays 16-aligned under the SDK's pack(8)). A form not understood, and a
  pragma that lays structs out another way (`ms_struct`, `align`, `options`), is an error; so are bit
  fields under a packing that matters off Windows (not done). Until 2026-11-06 the pragma lines were
  skipped without a word: MINIDUMP_EXCEPTION_INFORMATION was 24 bytes where C has 16 (thread F's PDB
  comparison; specs interop/c_pragma_*).
- A C function a header declares or defines and kpp declares `extern "C"` with its parameters is one
  function, its `static` included: a later extern declaration takes a visible static one's linkage
  (C11 6.2.2p4). A call reaches the header's definition when it has one. MSVC 14.44's time.h defines
  time, localtime, gmtime and mktime `static __inline` (14.51's plain `__inline`) and the stdlib's
  CTime.kpp declares them; before, every call of them was ambiguous. (2026-11-02, thread A, spec
  interop/c_static_inline_function_declared_extern_c.)
- For the C libraries' conversion (2026-10-31, thread D):
  - **Flexible array members.** `T items[];` as a struct's last data member, with another data
    member before it, has C's meaning: `sizeof` ends where the array starts (rounded up to the
    alignment), offsetof gives its start, and the elements are past the struct, indexed through a
    pointer or a reference. Errors (C's and clang's): one before another data member, alone, in a
    union, in a base, or held by value in another class; `sizeof` of the member, a range-for over it
    and a value for it in an aggregate's braces (it has no length). Specs
    language/flexible_array_member, flexible_array_member_placement_is_error and
    flexible_array_member_without_length_is_error.
  - **alloca.** `alloca` (imported with malloc.h) and `_alloca` allocate stack memory, aligned to 16,
    that lasts until the calling function returns, in a loop or an ended block too. A compile-time
    evaluation that reaches either is no constant. Spec language/alloca.
  - **MSVC's intrinsics that are instructions.** `_BitScanForward/Reverse(64)`, `__cpuid(ex)`,
    `_byteswap_*`, the rotations (`_rotl` ... `_rotr64`, `_rotl8/16`), `__popcnt(16/64)` and the
    SSE operations are emitted as their instructions, never as CRT calls; clang's implicit ones
    (`__popcnt`, `_rotl8`) are declared by the import of intrin.h. Specs interop/msvc_intrinsics,
    interop/sse_scalar_conversions.

### 6. Language changes from the proposal (not yet scheduled)
Real enum class with methods/str()/iteration, `this` as a reference, required `this.`,
required `override`, explicit by default (`implicit` keyword), no implicit switch
fallthrough, exhaustive enum switches (abort on invalid value), typed union, `?->`, `?:`,
`both_const` / `class_const`, named parameters, test tools in the compiler, `accumulate`
methods (the Factorio survey suggests generating save/load from the class may beat it).

`[*this]`, a lambda's capture of the object by copy (C++17), is an error until a program needs
it (2026-11-05, A and B): the parser read it as `[this]`, so a later change to the object showed
through the lambda (`7` where g++ prints `3`). No code of the game, the standard library or the
libraries uses it. Spec language/lambda_capture_of_this_by_copy_is_error. It stays an error once `this`
is a reference (kovarex, 2026-11-07, 23d): a copy is `[copy = this]`, the object by reference `[&this]`.

### 7. No preprocessor; what replaces macros (decided 2026-10-23)
Based on [macro-survey.md](macro-survey.md): Factorio's 611 macros and 22,647 expansions fall
into eleven cases and none of them needs textual substitution, so kpp has no preprocessor. A `#` or
`%:` line in kpp source is a compile error, each one reported (until 2026-11-06 it was skipped, so that
`#ifdef NOPE` compiled its body); the C and C++ a conversion or an import reads keep theirs. The
replacements, in the order of how much code they remove:
1. Tests and `check` in the compiler: `test name { ... }` declarations the compiler collects
   and the standard runner selects; `check(expr)` reports the text, the location and the operand
   values and records the failure instead of aborting.
2. `assert`/`release_assert` as statements of the language (text, location, operand values;
   `assert` compiles to nothing when `config::debug` is false); an implicit caller location
   parameter (`CallerLocation here = caller`, filled at the call site); `conditional(config::x)`
   functions whose calls compile to nothing, arguments unevaluated, when the condition is false.
   Done 2026-10-24: `assert(expr);` and `release_assert(expr);` are statements; a failure prints
   `file:line:column: assertion failed: <expression text>` through `kpp_assert_failed`, which a
   program may define itself as `extern "C"` (debugger break, test exception); `CallerLocation`
   lives in `stdlib/core/Location.kpp`; `conditional(expr)` is written where `static` would be.
   Operand values in the message: later.
3. Compile-time reflection: `for constexpr (m : members(T))` with `m.name`, `m.type` and
   `object.[m]`; `enumerators(E)` and `count(E)`; `nameof(expr)`.
4. `if constexpr` and `for constexpr` at every scope (function, class, namespace), over typed
   `config` constants and type lists (`types<A, B, C>`, or `const` arrays of structs for lists
   with data). A file may start with `requires config::platform == Platform::X;`.
   Done 2026-10-24: `--config name=value` (and `--release`) generate a `config` namespace of
   `const` values plus `enum class Platform { Windows, Linux, Mac }`; `if constexpr` folds at
   statement scope on any constant, and at namespace and class scope on conditions of config
   constants and literals (the parser evaluates those, so the other branch is never parsed);
   `requires <condition>;` at the top of a file. `for constexpr` and type lists: not yet.
   **Discarded branches (2026-11-02, kovarex's decision for the converted game's DEBUG and
   release code, `if constexpr (config::debug) { ... } else { ... }`).** In a function body the
   branch a constant condition discards is not checked, in a function that is no template as in a
   template's instance: it may name what only the other configuration declares (a member, a
   function, a type), and nothing in it (a local class, a lambda, a static_assert, a call that
   would instantiate) is checked or emitted. It must still parse. (C++ checks a discarded
   statement outside templates; kpp does not, so that one source holds both configurations.) A
   conversion's resolve of the C++ walks both branches, as C++ does (the converter rewrites
   both). Spec language/discarded_branch_in_plain_function.
5. Typed unions (`union enum`, section 6).
6. Real enums with `str()`, `fromStr()`, `count` and iteration (section 6). Done 2026-10-24
   except `fromStr()`, see 8.4.
7. `_` as a name that may be declared any number of times in a scope, each an unnamed local that
   lives to the end of the scope. Done 2026-10-24.
8. Attributes for the real compiler modifiers: `[[noinline]]`, `[[forceinline]]`,
   `[[vectorcall]]`, `[[optimize]]`, `[[no_sanitize]]`. Done 2026-10-24 for `noinline` and
   `forceinline`; the others are accepted and ignored for now. C++'s `[[nodiscard]]` and
   `[[deprecated("why")]]` warn as C++ compilers do (2026-11-06, thread E's battery): an expression
   statement that discards a call's result where the function, or the class or enumeration it
   returns, is `[[nodiscard]]` (a cast to void says it is meant), and a call of a `[[deprecated]]`
   function, with its reason. A warning is no error. On by default, unlike the other warnings'
   flags (`--access-warnings`, `--return-copy-warnings`), since the program's author asked for
   them by writing the attribute (decided 2026-11-06, kovarex: "yes"). In a template instance's
   body a warning counts once codegen emits the instance, as C++ warns in the instances it makes.
   Spec language/nodiscard_and_deprecated_warn.
The spellings above are the working proposal; each is settled and recorded here when the feature
is implemented.

### 8. Compile-time iteration, reflection, enums, typed unions, tests (decided 2026-10-24)
The spellings below were accepted by kovarex on 2026-10-24; they implement items 1 and 3 to 6 of
section 7. Everything here is instantiated at compile time by the same machinery as templates:
a body written once is checked and generated once per element.

**8.1 Compile-time lists.** `types<A, B, C>` is a list of types. A `const` array or a brace list
of constants is a list of values. Data that belongs to a type is a static member of the type
(`static const char* name = "custom-input";` in `CustomInputID`), not a second column of the
list, so the X-macro lists with strings become `types<...>` plus `T::name`.

**8.2 `for constexpr`.** The body is instantiated once per element; the loop variable is a type
or a constant:
```
for constexpr (typename T : types<CustomInputID, EntityID, ItemID>)
  registerLoader<T>(T::name);
for constexpr (int i : {1, 2, 4})
  table[i] = make(i);
switch (type)
{
  for constexpr (typename T : messageTypes)
    case T::kind: return parse<T>();
}
```
At namespace and class scope the body may hold function definitions and explicit
specialisations, which share their spelled name and differ by the element's type (overloads);
it may not declare data members or variables, because there is no way to name them per element
and no token pasting. Per-element data is an array indexed by the element, or a typed union.
Done 2026-10-25: `for constexpr` inside functions over `types<...>` and over a brace list of
constants; the loop variable is a template parameter of the body (a type, or a constant usable as
a template argument and an array bound), and the body is re-typed and generated once per element
the way a pack expansion is. Since the same evening, `case` labels inside the body: `switch (kind)
{ for constexpr (typename T : kinds) case T::kind: return parse<T>(); }` makes one case per
element (the label and the statement it labels are the body). Not yet: a list named by an alias or
a `const` array, bodies at namespace or class scope.

**8.3 Reflection.** `members(T)` is the list of T's non-static data members, bases first, in
declaration order; each member `m` has `m.name` (a string literal), `m.type` (usable where a
type is written), `m.offset`, and `object.[m]` is that member of `object`. `enumerators(E)`
lists an enum's values with `e.name` and `e.value`. `nameof(x)` is the last name of `x` as a
string literal (`nameof(this->sprite)` is `"sprite"`, `nameof(Direction)` is `"Direction"`).
In `if constexpr`, types compare with `==` (`if constexpr (m.type == bool)`).
```
template<typename T> void addValues(const T& object, Values& result)
{
  for constexpr (auto m : members(T))
    result.add(object.[m], m.name);
}
```
Reflection reads declarations; it never creates names. Save and load written this way replace
the `accumulate` idea of section 6 for the serialisation case.
Done 2026-10-25: `for constexpr (auto m : members(T))` with `m.name`, `m.offset` and
`object.[m]` (bases' members first, then the class's own, in declaration order; a member
descriptor is a compile-time value the loop binds, like a template argument). Done 2026-10-29:
`m.type`, written where a type is (`m.type copy = object.[m];`), and `alternatives(U)` (8.5). Not
yet: `enumerators(E)`, comparing types with `==` in `if constexpr`.

**8.4 Real enums**, the proposal's syntax with the inner brace block for the values:
```
enum class Direction : u8
{
  { North, East, South, West, None }
  bool isVertical() const { return this == North || this == South; }
};
Direction::count                       // 5
for (Direction d : Direction::values)  // iteration, in declaration order
d.str()                                // "North"; a user-written str() replaces the generated one
Direction::fromStr("North")            // Optional<Direction>
switch (d) { case Direction::North: ... } // exhaustive, no default; an invalid value aborts
```
Inside the enum's methods `this` is the value and the enumerators are in scope unqualified.
Comparison, copying and `switch` work on the value; the underlying type is explicit or `i32`.
Done 2026-10-24: the brace block, methods with `this` as the value, `count`, `values`, the
generated or hand-written `str()`, `switch` without `default` that must name every value (a
compile error otherwise) and aborts on a value outside them; `nameof(x)` from 8.3. A plain C++
`enum class` without the inner block keeps the C++ rules. Done 2026-10-25: `fromStr(const char*
text)` returns `Optional<E>`; the compiler declares it on every kpp enum and lowers it to the
library's `enumFromStr<E>(text)` (stdlib/core/Optional.kpp), which walks `E::values` comparing
`str()`; a `String` passes its `c_str()`, and a hand-written `fromStr` replaces the generated one.

An enumerator of a scoped enum is named through its enum outside the enum, in a case label too
(kovarex, 2026-11-07: "for readability and simplicity ... for now, this is a simple rule"):
`case Direction::North:`, no bare label on a switch over the enum and no `.North` shorthand. Inside the
enum's own methods `North`, `Direction::North` and `This::North` (23c) all name it. A bare one outside is
the error "'North' is not declared (...): write `Direction::North`, not `North`" (spec
language/scoped_enumerator_needs_its_enum;
the game has none, as kpp never accepted one).

**No plain enums** (kovarex, 2026-11-07: "plain enums are not nice"; decided, not built): kpp code has only
`enum class`; imported C and C++ headers keep theirs. As with the other rules, the error comes behind a flag
first, counted, then the converter's rewrite, then the error by default. `--plain-enum-warnings` (built) lists
each plain enum in kpp code and each use of its enumerators by kind ("Solid of Flags used in a bit operation
(the enum at PATH:LINE)"). The game (tests.cmd's build, Factorio 4c178a5b915): 208, 115 in src, 79 in libraries
(converted C: Lua, stb, trio, StackWalker), 13 in tests, 1 in the stdlib (Win32.kpp); by their heaviest use,
144 only as their own type (14 nameless), 20 flags in bit operations, 20 an array's index or size, 24
integers otherwise (Lua's opcode tables). Uses: 498 in bit operations, 466 in arithmetic, 375 as an index, 177
compared with an integer, 40 as an array's size, 16 converted. The form each kind converts to is a proposal
with C and the coordinator (enum class with its C++ underlying type, flags with bit operators, `count` for
array sizes, named constants for nameless ones and opcode tables). Spec language/plain_enum_warnings_mode.

An enumeration of a class template belongs to its instance (2026-11-06): the type carries the
instance's arguments, so `enum : T`, `sizeof`, promotion, overload ranking, a bit field, `count`,
`values`, `kppEnumeratorName` and `UnderlyingType` read the argument's type and the instance's
enumerators (GlobalScope::underlyingOf, one answer for the checker and the code generator). One in a
function's body is a copy of that instance's, whose walk keeps what its arguments make the underlying
type. Off Windows one without a fixed type is as wide as that instance's enumerators; on Windows it
stays int, as MSVC's is (clang's MSVC target widens it: not followed). Its methods, `str()` and
`fromStr()` are the instance's: a call passes the enumeration's type with its arguments as the class
type, its body is typed and emitted per instance (2026-11-07; a compile error before). Specs
language/enum_underlying_type_from_template_parameter, enum_local_to_a_template_instance,
enum_wider_than_int_in_a_template, kpp/enum_of_a_class_template_instance and enum_method_of_class_template.

**8.5 Typed unions**, the proposal's `union enum`: an enum whose every value carries a typed
payload, stored as the tag plus storage for the largest alternative.
```
union enum Property : u8
{
  { {int, Integer}, {double, Double}, {String, Text}, {String, Comment} }
  String str() const;
};
Property p;
p[Integer] = 5;                        // sets the tag, constructs the value (destroying the old one)
if (p == Property::Integer) use(p[Integer]);   // p[Integer] on another tag: release_assert
switch (p) { case Integer: return format("{}", p[Integer]); case Text: return p[Text]; ... }
```
Alternatives are listed by `alternatives(Property)` for reflection. `InputAction`,
`EditorAction`, `NetworkMessage`, `UpgradableEntity` in Factorio are this shape.
Done 2026-10-25: `union enum` with methods; `p[Integer] = v` (the value first, which may read what the
union holds; the alternative held takes it by its operator=, as std::variant, and another is
destroyed and the value constructed: kovarex 2026-11-06; the rule below), `p[Integer]` (a release_assert on another tag), `p == Integer`,
`switch (p)` (exhaustive, as for a kpp enum), copying, assignment and destruction following the
tag. Decided on the way: a default-constructed union holds its first alternative,
default-constructed; the tag type is the nested kpp enum `Property::Tag` (so `tag.str()` and
`Property::Tag::count` exist), and the alternatives' names are in the union's scope, as unscoped
enumerators. The parser desugars the union to a class with the tag member and an anonymous union
of the alternatives; the compiler adds the tag-aware operations. Not yet: a generated `str()`.

Decided 2026-10-28 (proposed for the converter's mapping of `std::variant` to named typed unions:
the game constructs them from values and saves and loads them generically; approved as written),
done 2026-10-29:
- *From a value.* `U u = x;`, `u = x;`, `return x;` in a function returning U, and x passed for
  a `U` (or `const U&`) parameter set the alternative whose type is x's type without references
  and `const`, when exactly one alternative has that type: `AIState next() { return
  Patrolling(...); }`. No other conversion takes part (an `int` does not set a `double`
  alternative), and a type two alternatives share (`{String, Text}, {String, Comment}`) is a
  compile error naming both (write `u[Text] = x`).
- *`alternatives(U)`*, as `members(T)` (8.3): U's alternatives in declaration order, for `for
  constexpr`. An alternative `a` has `a.name` (a string literal), `a.type` (usable where a type is
  written) and `a.tag` (its `U::Tag` value; `int(a.tag)` is its index); `u[a]` and `u == a.tag` are
  `u[Name]` and `u == Name` for it.
- *`u.tag()`*, generated like `str()`: the `U::Tag` value held (`U::Tag::count` already exists).
- Setting by index needs nothing more: `for constexpr (auto a : alternatives(U)) if (index ==
  int(a.tag)) { a.type value; load(value); u[a] = move(value); }`. The variants' save and load in
  SerialiserHelper/DeserialiserHelper become one override each, written this way.
Decided on the way: the tag data member is internal (`__tag`), so that `tag()` is the method. In
a `for constexpr` body over a value variable `a`, `a.type` is read as a type wherever one may be
written (`a.type value;`, `a.type()` constructs one, `Vector<a.type>`); a member descriptor of
`members(T)` has `m.type` too. The two errors are compile errors: "the typed union D has no
alternative of type int32", "a String is ambiguous for the typed union D: its alternatives Text
and Comment both have that type". Overload resolution ranks the conversion from a value like a
converting constructor's.
Spec snippets: kpp/typed_union_from_value, kpp/typed_union_alternatives.

Decided by kovarex 2026-10-31: *from a tag and a value*. Several tags may carry the same type, as
before (many InputActions with the same data, told apart by the tag); construction from a value
alone works only when exactly one alternative has that type, and is an ambiguity error
otherwise. An explicit construction from (tag, value) constructs that alternative directly:
`U(U::Text, x)`, `U u(U::Int2, 7);`, `return U(U::Comment, s);`, with no default construction of
the first alternative first (so it works for a union whose first alternative is not
default-constructible). Constructor arguments are taken in place: `U(U::Pos, 1, 2)` constructs
the alternative from them. The tag is a constant (a tag read at run time is an error). Specs:
kpp/typed_union_from_tag_and_value, kpp/typed_union_from_tag_and_value_errors.

Decided 2026-10-29 (the converter's std::variant ordering, asked by the coordinator): *comparisons*
are std::variant's. `a == b` holds when both hold the same alternative and its values are equal
(`!=` is its negation); `a < b`, `>`, `<=`, `>=` and `<=>` order by the alternative held, in
declaration order, then by the values of it (their `<=>`, or their `==` and `<`). A union's own
operator takes precedence; a class whose defaulted `==` or `<=>` has a typed union member compares
it so. `VariantAlternative<I, U>` is the type of the I-th alternative (a built-in of the
compiler's, as std::variant_alternative_t). Spec snippets: kpp/typed_union_comparisons,
kpp/typed_union_variant_alternative.

`u[Name] = x` and `u = x` are std::variant's assignment ([variant.assign]):
- x is evaluated first, a temporary `T(...)` included: it may read what the union holds, directly or
  through a reference, and if that throws the union keeps its alternative.
- The alternative held takes x by its operator= (for x of another type the one the checker chose
  for it, as `get<T>(v) = x`).
- Another alternative goes, and the new one is built in its place from x. Where building it from x
  can throw and the class's move cannot, a temporary of the class is built before anything goes, and
  moved: such a throw leaves the union as it was.

As for std::variant, an x that refers into the alternative held is gone when x goes to the place
itself: `u[Name] = move(u[Other].member)`, a copy that cannot throw, a class whose move can. A class
with no copy or move constructor cannot be assigned a value of its own class (a compile error;
std::variant refuses it too).

*Built in place.* No assignment builds an alternative where it stays, as the value comes first. A
class that must not move (the game's Targeter, registered with the deserialiser while it loads) is
built by the union's destructor and placement new from a tag, which is std::variant's emplace:
`u.~U(); new (&u) U(U::Name, T(input));` or `new (&u) U(U::Name, a, b);`, with no move, whichever
alternative was held, which goes first (the arguments must not refer to it). The converter's
generated `emplace<T>(args...)` and the game's loading are written so. Until 2026-11-06 `u[Name] =
T(...)` was built in place when the union held another alternative, the held one destroyed before
the value was evaluated: reading it directly stopped on the alternative check, and reading it
through a reference silently read the destroyed object, printing empty strings (thread B; specs
kpp/typed_union_new_alternative_reads_the_held_one, kpp/typed_union_value_reads_the_held_one_through_a_reference,
kpp/typed_union_value_of_another_type, kpp/typed_union_alternative_built_in_place,
kpp/typed_union_assigned_class_that_cannot_move_is_error).

`a = b` of two unions is std::variant's too: the same alternative by its operator=, another by the
rule above, for b's alternative as a value to copy or move.

A union whose new alternative's construction throws once the held one has gone (`u[C] = x` or `a =
b` by the rules above) holds none, as std::variant's
valueless_by_exception: its tag is `Tag::count` until an alternative is built, it is destroyed as
holding nothing, a copy of it is valueless too, it equals only a valueless union and orders before
every other, and `switch` on it aborts as on any invalid enum value. `u = u` keeps u. (2026-11-06,
E's battery: the tag was set first, and the never-built alternative was destroyed later; specs
kpp/typed_union_alternative_that_throws_is_not_destroyed, kpp/typed_union_valueless_and_self_assignment.)

**8.6 Tests in the compiler.**
```
test directionIsVertical
{
  check(Direction::North.isVertical());
  check(Direction::East.str() == "East");
}
```
A `test name { ... }` declaration (8.6 had a quoted name; 8.7 made it an identifier) may appear at namespace scope or inside a class (where it
sees private members). `check(expr)` records a failure with the expression text, the location
and the operand values and continues; `require(expr)` ends the test. Tests are compiled only
with `kpp --tests`, which generates the runner (`--run-test <name or file>` selects); without
the switch they cost nothing. Fixtures are ordinary classes constructed in the test.
Done 2026-10-30 (thread G; the runner's details in "Running and testing kpp code"): `kpp --test
FILES [-- runner arguments]` builds and runs them, `--tests` builds them (with `--exe`). The
selection is the runner's `--filter TEXT` (repeatable, `*` matches anything) instead of
`--run-test`, plus `--list` and `--report FILE`. `check` and `require` are statements inside a
test's body only (elsewhere `check` is an ordinary name); a comparison's operand values are
printed when `toString` or a `str()` method can say them. A failing `require`, or an `assert`
inside a test, ends the test by throwing, so destructors run. The build has the constant
`config::tests`, and the program's own `main` is renamed `__kpp_program_main`. `--test` and
`--run` run nothing when the build printed an error, whatever its exit code: a test the parser
could not read would otherwise silently not run (kpp/test_build_error). Not yet: a
compile error for two tests of one name, `--jobs N`.
Spec snippets: kpp/tests_in_compiler, kpp/test_runner, kpp/test_check_values, kpp/test_require,
kpp/test_filter, kpp/test_list, kpp/test_report, kpp/test_in_class, kpp/test_build_error.
8.7 replaces the quoted name: a test's name is an identifier.

**8.7 Suites, fixtures, dependencies and the rest of the checks (decided 2026-11-06).**
kovarex decided this on 2026-11-06 so that the game's tests (Factorio's UnitTest++ fork: 562
`SUITE`s, 7,648 `TEST`s, 1,186 `TEST_FIXTURE`s, 131 `TEST_PARAMETRISED_FIXTURE`s, 69 `DEPENDENCY`s,
13 `TEST_GENERATOR`s, 52,089 check macros) convert to native tests instead of their expanded macros.
The survey and the converter's mapping: [test-suites.md](test-suites.md). Done 2026-11-06:
the names, suites with depends and tags, fixtures, `generate`, the order, the errors across files
(driver::checkTestSuites), the char-pointer error, after a failure, `--exit-on-failure`, `--suite`,
`--skip-suite`, `--tag`, the hooks, the message arguments, `fail` and `requireThrows`/`checkThrows`.
The mixed-sign rule, a call's values and `--jobs` are done too. The parser
writes each test, fixture class and suite registration as tokens it then reads (parseTest,
parseSuite); the runner (compiler/src/tests.cpp) orders the suites when it starts.
```
suite DragBuildingBelt depends DragBuilding, TransportBelt
{
  test GhostForwardsOverObstacle : GameViewFixtureForBelts
  {
    buildEntities(1, "<<<  v  ");                 // the fixture's member
    require(entityAt(3, 1) != nullptr, "row ", 1);
  }
}
```
- **Names.** A suite's and a test's name is an identifier, case-sensitive as every kpp name; there
  is no quoted form (the 8.6 strings are renamed when this is implemented). Suite names are a name
  context of their own: they are found only after `suite` and `depends` (and by the runner), never
  by ordinary lookup, so `suite Beacon` beside `class Beacon` hides nothing. A test's name is
  scoped to its suite: two suites may each have a test `Empty`. A test's full name is
  `Suite.Test`; a test outside a suite keeps `file > test`. Compile errors: two suites of one name
  (a suite is one block in one file), two tests of one name in one suite, a `depends` naming no
  suite, a dependency cycle (the error lists it).
- **Suites.** `suite Name [depends A, B] [tags t1, t2] { ... }` at namespace scope. Its other
  declarations (helper functions and classes) are in a scope of the suite's own, seen by its tests
  only. Dependencies are between suites only. `tags` replace Factorio's `SUITE_MODPACKS`: without
  `--tag`, the runner runs the untagged suites; `--tag t` runs those tagged t.
- **Order.** Suites run in dependency order (Kahn's algorithm; of the suites that are ready, the
  first by name); the **tests of a suite run in the order they are declared in it** (kovarex),
  then the tests outside suites in today's order. The runner computes the order when it
  starts, the same every run, and `--list` shows it. `--shuffle [SEED]` alone changes it: it shuffles within the dependency order
  and prints the seed.
- **Fixtures.** `test Name : Fixture` or `test Name : Fixture(arguments)`: the body is a member
  function of a class derived from `Fixture`, so it names the fixture's members unqualified. The
  fixture is constructed before the body and destroyed after it, also when the body fails; a
  failure or exception while constructing it fails the test with `test failed: creating fixture
  Fixture: <what>` (the body does not run), one while destroying it with `tearing down fixture`.
- **Generated tests.** `generate Name { ... }` in a suite runs when the runner starts and adds
  tests with `addTest(name, function)` (a `String` and a `Function<void()>`): Factorio's
  `TEST_GENERATOR` (one test per save file, per entity pair). They take the generator's place in
  the suite's order, in the order added; their full name is `Suite.Generator.name`.
- **Checks.** `check(expr)` continues after a failure and `require(expr)` ends the test, as in
  8.6; the converter writes `require` for every Factorio `CHECK*` (Factorio's fork ends the test at
  each failed check). New forms: `check(expr, arguments...)` and `require(expr, arguments...)`
  print the arguments after the failure as `println` prints them, C text and a `str()` method too, and
  `?` for a value with no text (Factorio's `_SIGNED` ids, which the converter cannot type);
  `fail(arguments...)` fails and ends the test (`CHECK_FAILED`); `requireThrows<T>(expr)` evaluates
  `expr` inside a try, fails and ends the test unless a `T` (or a class derived from it) is thrown,
  and is the caught exception; `checkThrows<T>(expr)` is an `Optional<T>` and continues. These are
  statements of the language, as `check` is: kpp has no lazily evaluated arguments.
- **Values in a failure.** Besides a top-level comparison's two operands (8.6), a top-level call
  shows its arguments' values, `!call` too: `check failed: isClose(x, 1.5, 0.01) (1.52, 1.5,
  0.01)`, so `CHECK_CLOSE`, `CHECK_BETWEEN` and `CHECK_CONTAINS` become ordinary functions. The
  values are the ones the function took, after their conversions, so the check means what the call
  alone means and each argument is evaluated once; `--report` lists them as `values`. The compiler
  keeps them where the call has them, and a check that passes runs nothing for them, except that a
  class by value is copied bytewise first (the callee may change it). So what a reference parameter
  shows is the object as the call left it, a class by value with a destructor shows `?` (the callee
  destroyed it), as do an `InitializerList` and a value `__kppShow` cannot say, a null pointer shows `null`, an argument left
  to its default is not listed, and a `char*` is read as C text. A call of a function or a member
  function has them; a call through a pointer or of a closure has none. The checked call is not
  inlined by kpp. Its cost, measured on the game's tests (52,089 checks, 2026-11-07): kpp of
  1dd08930 against the same with call values, one converted tree and stdlib, tests.cmd's build
  command from scratch, interleaved, each run after 20 s of load under 10%: the build 19.5 s (19.8,
  19.5, 19.5) against 19.2 s (19.3, 19.2, 19.2), factorio-test.exe 522,752 bytes larger (0.37%),
  and the 9,501 tests in 16 shards (suites.py --shards 16) 67.4 s (67.4, 67.4) against 67.7 s
  (68.4, 66.9): no difference beyond the noise.
- **Integers in a check compare by value.** Inside `check` and `require`, a comparison of a signed
  and an unsigned integer compares their values (`std::cmp_equal`'s rule): `check(-1 ==
  4294967295u)` fails, which outside a check (design.md:1029, the signed side converted) is true.
  It keeps `CHECK_EQUAL`'s meaning. The rule covers the comparison that is the check's whole
  expression: nested in one (`check(!(a == b))`, `check(a == b && c)`) it is C++'s, as a game's
  `CHECK(a == b && c)` always was.
- **Char pointers in a check.** Inside `check` and `require`, `==` or `!=` between two char pointers
  (`const char*`, `char*`, or one of them and a string literal) is a compile error, "compares
  addresses: write StringView(a) == b, or cast to (const void*) to compare the addresses":
  Factorio's `CHECK_EQUAL` compared their text (`strcmp`), and the converter, which has no types
  in test bodies, writes `require(a == e)` for every `CHECK_EQUAL`; the error points out the sites.
- **Test functions.** A function or class declared `test` (`test void requireBuildable(...)`,
  `test class GameViewFixture { ... }`) is compiled only in a test build and may use `check`,
  `require` and `fail`. A failure there names the check's location, then one `called from` line per
  test function up to the test (`file:line:column: called from here`): Factorio's
  `_SOURCELOCATION` variants and its own check macros. A lambda inside a test is part of the test.
  The line is the call's, the place `CallerLocation` gives it, for constructors, operators,
  conversions and virtual calls too. A destructor has no call site, and a call through a function
  pointer or a `Function` has none either: they add no line, never a wrong one. Nor does a virtual
  call whose named function has no body (a pure one), a constructor while its member initializers
  run, or a function-try-block's function. How: frames replace the hidden `CallerLocation` parameter
  first planned, so no signature changes. Before a call of a test function in test code, the call
  site goes to a per-thread variable (`kpp_test_call`), and the body's first statement, a
  `__KppTestFrame` the parser writes, takes it onto a per-thread list that its destructor leaves. A
  noted site reaches its own call's frame or none. Two things can run in between: a constructor holds
  it in a slot of its own while its member initializers run, which may throw (`kpp_test_hold`,
  `kpp_test_release`), and a virtual function without a frame that overrides a test function drops
  it on entry (`kpp_test_forget`). A test function is therefore not inlined, and costs about 10 ns a
  call, a constructor about 2 ns more; code that is no test function and overrides none pays nothing.
  Which functions are test functions (coordinator, 2026-11-06): every function and class declared
  in a suite is one implicitly; `test class X` makes all its member functions test functions,
  and an out-of-class body writes `test` itself, in another file too: a file's parser cannot know
  the class, and a build without tests must skip the body. Omitting it is a compile error. A
  function's first declaration decides, so later declarations
  may repeat or omit `test`, and adding it later is a compile error. Both rules hold for function
  templates, members of class templates and member templates, where `test` comes first (`test
  template<typename T> void f(T);`).
- **After a failure** (Factorio's behaviour, `tests/TestRunner.cpp:33-60`): the failed test ends,
  the run goes on, dependents included. `--exit-on-failure` (off by default) stops after the first
  failed test has finished; the rest count as not run (`3 tests, 1 failed, 1 not run`), and since
  dependencies run first, a failure in a dependency stops the run before its dependents. With
  `--jobs N` it starts no further suite. Reporting only: a failed test of a suite whose
  dependency, directly or further back, had a failure, prints `failed: B.x (after failures in: A)`,
  and the summary then names the root failures, those without a failed dependency:
  `3 tests, 2 failed (root failures: A.y)`.
- **The runner's options** add `--suite NAME` and `--skip-suite NAME` (repeatable), `--with-dependencies`
  (adds the suites the selection depends on, whole, whatever selected the others; `--skip-suite`
  still wins), `--from NAME` and `--after NAME` (start the selection in the order at a suite, a test's
  full name or a test's name when one selected test has it, or just after it; an unknown or ambiguous
  name is an error, exit code 2, and so are both options together), `--tag TAG`, `--exit-on-failure`
  and `--shuffle [SEED]` (an unsigned number; without one the runner picks it; it prints `shuffle seed:
  N`, and one seed gives one order on every target). Names on the command line
  match case-insensitively (a convenience, as Factorio's command line has it); `--filter` matches
  the full name. A `--filter` no test matches, a `--suite` or `--skip-suite` no suite has, and a
  `--tag` no suite has are errors, exit code 2: each would run nothing, or skip nothing, and pass. `--report` lines gain `"suite"`, `"seconds"` and `"status": "not run"`. A test that
  ran longer than 5 minutes gets a line when it ends, `slow test: Suite.Test ran longer than 300 s`
  (`--slow SECONDS` sets the limit).
- **Hooks.** If the program defines them, the runner calls `void testsSetup(const Vector<String>&
  arguments)` before the first test, with the arguments it does not know itself (without it an
  unknown argument is an error, exit code 2), `void testAfterEach()` after each test (a failure in it
  fails that test), and `void testsTeardown()` after the last. The converted game keeps its options
  (`--config`, CRC mode, heavy mode, graphics) and its per-test checks (leaked entities) there.
  `void testBeforeEach()` comes before each test; a failure that ends it leaves the test's body out
  (the game's `crcDumper.open`). `void testRun(Function<void()> run)` wraps a test: `run` builds the
  fixture, runs the body and destroys the fixture, and the hook may call it again (the game's
  rerun on `LatencyTestDesyncError`, for the desync report). Without the hook the runner calls `run`
  once. A failure's `TestEnded` passes through the hook; a hook that catches everything must let it go.
  `void testAssertFailed(const char* file, int line, int column, const char* text)` replaces what a
  failing assert does in a test build (it fails the test): the game's throws, and its tests check that.
- **What a test build adds for the program.** `StringView currentTestName()` is the running test's
  full name, empty outside a test. `bool runTest(StringView name)` runs a registered test in place,
  through `testRun` and without `testBeforeEach` and `testAfterEach` (as the game's `Test::Run`),
  and `bool runCaptured(Function<void()> body)` runs a function the same way (the game's
  MetaTestRunner builds its inner test on the spot). Both return true when it passed; its failures
  are not printed and do not fail the caller, which writes `require(runTest(name))` if they should.
  `name` is `Suite.Test`, or `Test` when one test has that name (exact, case-sensitive); an unknown or
  ambiguous name, or the running test itself, fails and ends the caller. `TestEnded` is what a
  failed `require` throws, for a destructor that must not let it out (the game's TestScenario::clear).
- **A name that the statements hide.** In test code `check`, `require` and `fail` are statements, so
  `check(a, b)` is a check with a message. When a call of that name there would find a declaration (a local
  lambda, a member, a fixture's member, a function of an enclosing namespace, a member of a base
  that depends on a template parameter, which the instance reports, or a function of an argument's
  namespace) the statement is a compile error, "rename the declaration": the call would otherwise
  silently be read as a check.
  Elsewhere they stay ordinary names, and a test may call one qualified (`math::check(1)`,
  `::check(1)`). Decided (kovarex, 2026-11-07): a global `check` is no exception, and a member named
  check stays fine: `obj.check(x)` and `this->check(x)` are member access, never the statement
  (kpp/test_check_member_call).
Spec snippets: kpp/test_suite_helper_uses_require, kpp/test_before_each, kpp/test_current_name,
kpp/test_run_by_name, kpp/test_run_by_name_unknown, kpp/test_run_captured, kpp/test_run_hook,
kpp/test_ended_public, kpp/test_check_hidden_name_is_error, kpp/test_check_member_call, kpp/test_check_hidden_by_dependent_base_is_error,
kpp/test_check_dependent_base_without_the_name, kpp/test_check_hidden_by_argument_namespace_is_error,
kpp/test_with_dependencies, kpp/test_from, kpp/test_after, kpp/test_from_unknown_is_error, kpp/test_shuffle,
kpp/test_slow, kpp/test_called_from_kinds, kpp/test_called_from_never_stale, kpp/test_class_template_body_outside,
kpp/test_template_declared_test_later_is_error, kpp/test_class_template_body_outside_needs_test_is_error,
kpp/test_member_template_body_outside_needs_test_is_error, kpp/test_member_template_of_class_template_outside_needs_test_is_error, kpp/test_declared_test_later_is_error,
kpp/test_class_body_outside_needs_test_is_error, kpp/test_class_body_outside, kpp/test_function_redeclared_without_test, kpp/test_suite_order, kpp/test_suite_declaration_order,
kpp/test_same_name_in_two_suites, kpp/test_suite_declared_twice_is_error,
kpp/test_duplicate_name_in_suite_is_error, kpp/test_suite_depends_unknown_is_error,
kpp/test_suite_depends_cycle_is_error, kpp/test_suite_name_hides_nothing, kpp/test_suite_tags,
kpp/test_fixture, kpp/test_fixture_constructor_fails, kpp/test_generate, kpp/test_check_message,
kpp/test_fail, kpp/test_require_throws, kpp/test_check_call_values, kpp/test_check_mixed_sign,
kpp/test_function_called_from, kpp/test_dependents_annotated,
kpp/test_exit_on_failure_before_dependents, kpp/test_hooks, kpp/test_identifier_name,
kpp/test_check_char_pointers_is_error.

### 9. Exceptions and RTTI (decided 2026-10-24)
- kpp has exceptions, with C++'s `throw`/`try`/`catch` semantics, implemented on LLVM's
  unwinding (the Windows personality); the non-throwing path costs nothing. Factorio's 3,198
  `throw` sites convert as they are.
- `dynamic_cast` and `typeid` work for the whole hierarchy (Factorio's 647 casts): a type
  descriptor behind the vtable names the class and lists its direct bases with their offsets, so a
  downcast or a cross-cast through any base finds its target. No virtual bases: a `virtual` base
  specifier is an error (2026-10-31, classes/virtual_base_is_not_supported). A non-virtual base
  reached along two paths is two subobjects, as in C++: a qualified name (`b.Right::id`,
  `b.Right::get()`) goes through its own path, and a conversion to the repeated base itself is an
  error, ambiguous (2026-10-31, classes/repeated_base_conversion_is_ambiguous; the game has four
  such classes, Unit and SpiderUnit with Targetable twice, Loader and InfinityPipe with Freezable).
  `dynamic_cast` follows [expr.dynamic.cast] 8 and a handler [except.handle] 3: a base there twice or
  not public is no target (a descriptor says which base is public; a class with every base public
  and none twice takes the first hit, the fast path nearly every class is on). A failed cast to a
  reference throws BadCast, `typeid(*p)` of a null p BadTypeid (classes/dynamic_cast_public_unambiguous_subobject,
  classes/handler_takes_public_unambiguous_base, classes/failed_reference_cast_and_typeid_of_null_throw).
- The vtable is MSVC's and COM's (2026-10-29): the object's first word points at the first
  virtual function's slot, the slots follow in declaration order (an override takes its base's
  slot), `this` is the first argument and a class result goes through a hidden pointer after it.
  The type descriptor is the slot just before the first function (where MSVC keeps its RTTI
  locator), and the slot before it holds the offset of the subobject whose vtable pointer it is
  within the whole object (0 for the first one). So a COM interface (ID3D11Device) can be declared
  as a kpp class with its virtual functions in the header's order, and a C table of functions
  called through one (spec interop/c_object_with_vtable). Not MSVC's: a virtual destructor's slot
  is a plain destructor (MSVC's takes a flag; COM interfaces declare none), overloaded virtual
  functions keep declaration order (MSVC groups them; COM has no overloads), and a class argument
  is passed as a pointer to a copy where MSVC passes 1, 2, 4 and 8-byte structs in a register.
- Multiple inheritance (kovarex, 2026-10-28: full C++ multiple inheritance, without virtual bases;
  implemented 2026-10-29) is laid out as MSVC does: the polymorphic bases first, in declaration
  order, then the other bases, then the members (bases are still built in declaration order and
  destroyed in reverse). The first polymorphic base is the primary base: at offset 0, its vtable
  pointer is the class's own, and the class's new virtual functions extend its vtable. A
  polymorphic class without a polymorphic base has its own vtable pointer at offset 0, before its
  bases. Every other polymorphic base keeps its vtable pointer in its subobject, pointing at a
  vtable the class has for that subobject: the base's slots in the base's order, each holding the
  final overrider, or a thunk that moves `this` from the subobject to the overrider's class and
  jumps to it. A virtual function that overrides only a secondary base's function also gets a slot
  in the class's primary vtable (the Itanium ABI's choice; MSVC adjusts `this` in the overrider
  instead), so a call through the class itself needs no thunk. A call moves `this` to the subobject
  of the class that declares the function and calls through that subobject's vtable pointer.
  A covariant override whose result does not start with the class the slot gives back takes a new
  slot in the primary vtable (both C++ ABIs' choice), which calls through its class use; the
  slots it overrides keep their callers through a thunk that moves the result to the base they
  expect, a null pointer staying null. Each slot records the class its callers expect
  (2026-11-06; spec classes/covariant_override_moving_result_along_primary_bases).
  Constructors point every vtable pointer at the class's vtables once the bases are built, and a
  destructor points them back at its class's on entry, so virtual calls during construction and
  destruction reach the class being built or destroyed, as in C++. A copy gets its own class's
  vtable pointers and an assignment keeps the target's. `delete` through a base frees the whole
  object (found with the offset in slot -2), and `catch` takes any base of the thrown class. Until
  the layout file exists (the compile-time evaluation section), the code generator lays classes out
  under its shared lock.
- Bit fields are laid out as MSVC does: consecutive bit fields share a storage unit while their
  declared types have one size and their widths fit (`uint8_t x : 5; bool on : 1;` share a byte,
  `int a : 3; unsigned b : 4;` four bytes); a type of another size starts a new unit, aligned to
  that type (2026-10-29, spec classes/bitfields_of_one_size_share_a_unit). An unnamed bit field of
  width 0 (`int : 0;`) right after a bit field ends that unit: the next field starts at a boundary
  of its type's alignment, which the class takes too; anywhere else it is nothing, as MSVC has it
  (2026-11-05, spec classes/zero_width_bit_field_ends_its_unit). That is Windows' layout (the MSVC and
  the GNU dialect alike). Off Windows bit fields are laid out by SysV's rules, the target's C (2026-11-05,
  the targets table below): a bit field starts at the next free bit unless it would straddle a boundary
  of its type's size, then at its type's alignment; a zero-width one pads to its type's alignment;
  bit fields share bytes with each other whatever their types, and a member that is no bit field
  starts at the byte after them; a named one gives the class its type's alignment (an unnamed one
  does not). Bit fields that share bytes are one storage unit, a byte array at its exact offset, read
  and written as an integer of its bytes, align 1. A [[packed]] class keeps MSVC's rules. Spec
  interop/c_struct_bit_fields_match_c: a struct shared with C, its size, alignment, offsets and
  fields as C's on each target.
- Every empty base takes no storage (Itanium's rule), on every target: MSVC gives only the first
  empty base none unless the class says `__declspec(empty_bases)`, so a class with two empty bases
  (`struct T : Empty, Empty2 { char c; };`) is 1 byte in kpp and 2 with MSVC (found comparing with
  clang, 2026-11-06). kpp shares classes with C alone, which has no bases.
- A base's padding holds nothing of a class derived from it, on every target: the derived class's own
  parts start after the base's whole size. MSVC lays them into the padding an alignas base's alignment
  adds (`struct alignas(16) B16 { char x; }; struct S1 : B16 { int y; };` is 16 bytes with y at 4, in
  kpp 32 with y at 16), and Itanium (Linux, FreeBSD) into a non-POD base's tail padding (bit fields
  of a class derived from `struct BaseB { int x = 1; unsigned k : 2; }` start in BaseB's last bytes:
  8 bytes with g++, 12 in kpp), as for a polymorphic base (the targets table). So a copy of a base
  subobject, its size in bytes, never touches the derived class's members. Found comparing with clang
  and g++ (thread B's batteries, 2026-11-06); the same reason as the empty bases above.
- `[[no_unique_address]]` on a data member of an empty class: it takes no storage, on every target
  as an empty base (MSVC ignores the attribute; kovarex 2026-11-06), at offset 0 as Itanium's layout
  has it, and a class holding only such members is empty itself. Two objects of one type have two
  addresses, so a member whose class another part of the class holds (a base, another member, inside
  either) keeps its byte; so does one aligned to more than a byte. Itanium gives such a member an
  address inside the class without a byte of its own, so `struct { [[no_unique_address]] Empty a, b;
  int x; }` is 8 bytes in kpp and 4 with clang, `struct A : Empty { [[no_unique_address]] Empty e;
  int x; }` too, and with `alignas(16)` on Empty `{ [[no_unique_address]] Wide w; char c; }` is 32
  and 16: sizes alone, every address differs. On a class that is not empty the attribute changes
  nothing: a member's padding holds nothing of the class around it, the rule for a base's. Anywhere
  but on a non-static data member that is no bit field it is an error, as clang's. Specs
  classes/no_unique_address_*.
- On Windows a class with a vtable pointer of its own lays its bases and members out from their
  largest alignment after it, as MSVC does: `class Over { virtual ~Over(); char c; alignas(16)
  double d; }` has c at 16 and d at 32 (48 bytes), where Itanium's layout (the other targets) puts c
  right after the pointer, at 8 (2026-11-06, F's battery f9; spec classes/msvc_parts_after_own_vtable_pointer).
- A pointer to a data member is the member's offset in its class, -1 when null: 4 bytes on Windows
  (MSVC's ABI, as MSVC and clang for Windows have it), 8 on the other targets (Itanium's). A pointer to
  a member function is its function, and beside it the adjustment of `this` (16 bytes) where the class
  has a base at an offset (MSVC's multiple inheritance model). The code generator's layout and the
  compile-time sizeof and alignof follow one rule (GlobalScope::hasBaseAtOffset, setMsvcAbi), so
  `Size<sizeof(int A::*)>` and a run-time sizeof agree (2026-11-06, F's batteries; spec
  classes/data_member_pointer_size_on_windows).
  MSVC's unknown inheritance model (24 bytes, for a pointer to a member of a class incomplete where
  the pointer's type is written) has no counterpart: symbols do not depend on order, every class is
  known, so such a pointer has its class's size (the game's CommandWrapper::command, to a member
  function of CommandProcessor, is 8 bytes where the C++ build has 24).
- A bit field's value promotes by its width, as C++'s [conv.prom] 5 has it, on every target
  (kovarex 2026-11-05: "follow the standard here"; MSVC keeps the declared type): an operand of
  arithmetic, a comparison or `?:` that designates a bit field of an integer type is an int when
  int holds every value of its width, an unsigned int when only that does (32 bits of an unsigned
  type), and as its type when it is wider; one of an enumeration or bool promotes as its type. So
  with `unsigned u3 : 3` and `uint64_t w3 : 3`, `u3 - 2` is the int -1 and `(w3 - 3) / 2` is 0,
  where MSVC computes 4294967295 and 9223372036854775807. An operand designates the field through
  parentheses, an assignment, a prefix ++ or -- and a comma's right operand, as clang's. The
  checker types the operators so (GlobalScope's bitFieldPromotion, a class template's width by
  its instance's arguments), codegen and the compile-time interpreter compute them so. Specs
  language/bit_field_promotes_as_cpp, language/bit_field_promotes_by_its_instance_width. Overload
  resolution ranks a bit field argument's conversion to the type it promotes to as a promotion
  (2026-11-06, [over.ics.scs]): `f(s.u3)` with f(int) and f(i64), or f(int) and f(double), calls
  f(int), as clang does, where kpp found it ambiguous; its declared type stays an exact match
  (h(unsigned) over h(int)), and a 32-bit unsigned field, which promotes to unsigned, takes
  n(unsigned) over n(i64) (spec language/bit_field_argument_ranks_by_its_promotion). A structured binding of a bit field promotes as the field does (2026-11-06,
  [dcl.struct.bind] 5): `auto [a, b] = s;` over `uint64_t w3 : 3` makes `b - 3` the int -1, in the
  checker's types (bitFieldPromotion finds the binding's field), at compile time and in codegen
  (bitfieldOperand through bindingBitfield); spec language/structured_binding_of_bit_field_promotes_as_the_field.
- Implemented 2026-10-24 (throw, try/catch, rethrow; `dynamic_cast` not yet) on Windows SEH
  rather than the MSVC C++ ABI: `throw` builds the object on the heap and raises an SEH exception
  (code 0xE0000001) carrying the object, its type descriptor and its destructor; every `try`
  gets a filter function that searches the descriptor's bases for each handler's type in order
  (so `catch (const Base&)` works, with the base-subobject offset applied); destructors between
  the throw and the handler run in cleanup funclets; a handler owns the object and destroys it on
  exit unless it rethrows. Calls that may throw become `invoke`s only where something has to
  happen on the way out, so code without destructible locals and without `try` is unchanged.
  Uncaught exceptions print `unhandled exception of type T: message`, call the terminate handler
  `setTerminate` installed (one for the program, kpp's own on every target), and abort; the search
  ends the program before anything unwinds, the throw site on the stack (2026-11-04). Rules that follow
  from this: a destructor is noexcept unless it or a base's or member's says otherwise, as in C++
  (see "A destructor's exception specification"), and a `noexcept(false)` one throws to the scope that
  ran it (spec language/noexcept_false_destructor_throws_to_its_caller); `extern "C"` functions do not
  throw (calls to them stay
  plain calls); an exception cannot pass through a C callback frame that was not compiled for
  unwinding; a handler of a pointer by value or by const reference takes a thrown pointer that
  converts to it, as C++'s [except.handle] 3.3 and 3.4 have it (2026-11-05, 0b2ed657: a derived
  class's, a less qualified one, any object pointer for `void*`, nullptr), and `catch (Base*&)`, a
  reference to a non-const pointer, its own type alone (3.1), as the standard says (kovarex
  2026-11-05: "as C++"), where g++'s, clang's and MSVC's programs give it a converted pointer too
  (spec classes/catch_of_base_pointer_takes_derived_pointer); `new T(args)` leaks the memory when the constructor throws (to fix with the allocator work).
- `new T[n]` (2026-10-29) allocates n elements with malloc and builds each in order (a class by its
  default constructor; a scalar is left as it is, or zeroed by `new T[n]()`; `new T[n]{a, b}` builds
  the first ones from the list and value-initializes the rest). When T needs destroying, the block
  starts with a cookie holding n in its last 8 bytes, right before the elements, as MSVC's runtime
  lays it out (16 bytes for a T aligned to 16, malloc's alignment; more alignment is an error), and
  `delete[] p` reads n back, destroys the elements the last first, and frees the block; for a T
  that needs no destroying there is no cookie and `delete[]` only frees. The elements' destructors
  are T's own: `delete[]` through a pointer to a base of the elements' class is undefined, as in
  C++. A constructor that throws partway leaks the block and the elements built so far, as
  `new T(args)` does.

### 10. Draft types for forward references (decided 2026-10-26)

When the resolver encounters an unresolved type name during Phase 2 (e.g. a class used before
its definition is indexed), it creates a *draft* `ClassDecl` marked with `ATTR_Draft` instead
of returning `Unknown`. All references to the same name in the same namespace share the same
`Ty*` pointer (a `TyKind::Class` pointing at the draft Decl), so when the real definition
arrives the Decl is filled in and every use sees the resolved type without patching.

**Mechanics.** Drafts live in a separate sharded map (`DraftShard[16]` in `GlobalScope::Impl`,
keyed by `{Scope*, symbol}`) to avoid data races with the lock-free Phase 2 scope lookups.
`getOrCreateDraft` allocates a `ClassDecl` with `ATTR_Draft`, gives it an empty `ownScope`
parented to the target namespace, and inserts it into the shard under a lock. The target
namespace is the nearest enclosing `ScopeKind::Namespace` of the use site (or root). A second
lookup for the same key returns the existing draft.

**Template parameter guard.** Template parameters (`T`, `U`, ...) are not in scope during some
`convertType` paths, so an unresolved `T` must not get a draft class. During `finishIndex`,
`collectTemplateParamNames` walks the scope tree and collects every template parameter symbol
into a set; draft creation is skipped when the symbol is in that set.

**Qualified names.** Multi-part names like `Container::value_type` are handled by creating a
draft for the first part (`Container`) and chaining the rest as `DependentMember` types
(`draftType::value_type`). If the first part carries template arguments (`Map<K,V>::iterator`),
they are resolved and stored on the draft's class type. This way every reference to the same
qualified path through the same draft shares a single type chain.

**Result.** On Factorio, front-end metrics are unchanged (69.6% resolved, 19.3% dependent,
11.1% unresolved type names). Drafts give formerly-unknown types a stable `Ty*` identity,
which downstream phases (codegen, template instantiation) can use when the definition appears.

### 11. Evaluation order: C++17's rules (decided 2026-10-30)

kpp evaluates operands in the order C++17 sequences them, so that converted code means what it
meant:

- An assignment, `=` and every compound one, built-in or a class's or a free operator function,
  evaluates its right operand before its left ([expr.ass] 1, [over.match.oper] 2): `table[key] =
  *value` reads `*value` before `table[key]` inserts (CompiledMapGenSettings::createContext read a
  moved FlatMap entry before this). The right operand keeps its value category: a temporary or an
  xvalue is still moved from, an lvalue copied.
- A subscript `E1[E2]`, a shift `E1 << E2` and `E1 >> E2`, and `E1.*E2` and `E1->*E2` evaluate
  their left operand first; a call evaluates the callee (and a member call's object) before its
  arguments ([expr.sub], [expr.shift], [expr.mptr.oper], [expr.call] 8). Overloaded operators
  follow the built-in operator's order.
- `new` allocates before it evaluates the initializer's arguments ([expr.new] 19).
- Otherwise C++ leaves the order unspecified (a call's arguments among themselves, the operands
  of `+`), and so does kpp: code must not depend on it.

Allocation functions (2026-11-06). `new T` of a class with its own operator new (a member, or a
base's) calls it with the size and the placement arguments, `new (args) T` choosing among the
class's by overload resolution, and a delete-expression calls the class's operator delete after the
destructor: its `operator delete(void*)`, else `operator delete(void*, size_t)` with the class's
size, never for a null pointer. `new T[n]` and `delete[]` call the class's operator new[] and
operator delete[] alike, with the size the elements and the cookie take; a sized operator delete[]
gets the size operator new[] was asked for, which a cookie keeps for an element type without a
destructor too (as the Itanium ABI has it; clang for Windows passes one element's size). A delete
through a virtual destructor calls the operator delete of the object's own class, which slot -3 of
its vtable holds (a function calling it, null for free). When the constructor throws, the memory
goes back through the class's usual operator delete for `new T`, and for a placement form through
the one whose parameters after the first are its operator new's, or none. They are static members
whether or not they say so; the type checker chooses them and records them on the node
(NewExpr::allocator, deallocator; DeleteExpr::deallocator; design.md 24). A class's operator new
hides the global ones (`new (p) T` it does not take is an error), and `::new` and `::delete` pass
the class's by. The library declares the global ones, `::operator new(n)`, `operator new[](n)`,
`::operator delete(p)` (sized or not: stdlib/core/Sizes.kpp), malloc and free with BadAlloc, calling
each other as C++'s do (new[] is new, a sized one the unsized one, delete[] delete). A program may
replace any of them by defining it ([replacement.functions]): its definition is that function (the
library's is never a candidate, nor emitted), and it may declare placement forms of its own. When a
program declares any global allocation function, every new-expression and delete-expression without
a class's own calls the global ones (a delete the sized one, as clang's sized deallocation; delete[]
the sized one when there is a cookie; a virtual destructor's through slot -3 with the whole object's
size); otherwise new and delete allocate with malloc and free themselves. The library's containers
allocate with malloc either way (docs/stdlib-conformance.md). Not supported yet, and an error: an
operator new that is a template. Specs: classes/class_operator_new_and_delete_are_called,
classes/class_operator_delete_when_the_constructor_throws, classes/class_template_operator_new_and_delete,
classes/class_operator_new_array_and_delete_array, classes/polymorphic_class_operator_delete,
library/global_operator_new_and_delete, library/program_replaces_global_operator_new_and_delete, and the errors
classes/placement_new_without_the_class_operator_new, classes/delete_without_a_usual_class_operator_delete.

An assignment used as an lvalue (`int& r = (a[i++] = v);`) is the place it stored to; its left
operand is evaluated once. Specs: expressions/assignment_evaluates_right_operand_first,
expressions/assignment_right_operand_first_keeps_value_category.

A const object is never modified (2026-10-31): a built-in assignment, compound assignment,
increment or decrement of a const value, and the implicit copy or move assignment of a const
class object, are errors ([expr.ass] 1, [expr.pre.incr]), in a template's instance only when its
function is emitted. `volatile T` alone is volatile, not const. A non-const member function is
no candidate for a const object ([over.match.funcs] 4-5): calling one, or the class's own
operator, on a const object is an error. Specs: language/const_value_assigned,
language/const_value_incremented, language/volatile_is_not_const,
classes/non_const_method_on_const_object_is_error.

Volatile (2026-11-06). A read or a write of a volatile scalar is `load volatile` or `store volatile`:
through its name, a pointer, a reference, an element, a member of a volatile object, whose members are
volatile as a const object's are const. Before this nothing reached the IR and an optimized build moved
such reads out of loops (silent wrong answer, spec language/volatile_access_is_kept_by_the_optimizer).
In a volatile member function `this` points to a volatile object, so its members are volatile, a
mutable one too (language/volatile_member_function_reads_its_members_as_volatile, 2026-11-07). Open: a
volatile class copied whole, and a volatile bit field, are plain accesses. Volatile overloads as const does (classes/overloads_on_volatile): it
is part of a member function's identity, its name in the IR and its out-of-line match, a `volatile`
object takes volatile members only, and a qualification more is the worse conversion. An out-of-line
pair that differed only in const or volatile took the first definition found.

Smaller rules of C++ (2026-11-06), each with its spec and clang's and g++'s output:
`c ? throw x : a` is `a`, its value category too (language/conditional_beside_a_throw_is_the_other_branch).
An object no call operator of which takes the arguments is called through a conversion to a pointer or
a reference to a function, chosen by the arguments among its conversions (GlobalScope::surrogateFunction,
classes/object_called_through_a_conversion_to_a_function); a call operator that takes them is used
first, where C++ can call the two ambiguous (`t(4)` with `operator()(long)` and a conversion to `int (*)(int)`:
kpp calls the operator). An array captured by copy is copied element by element
(language/lambda_captures_an_array_by_copy). A variable template that is not const is one object for
each arguments, linkonce, from a constant initializer; one built at startup is a compile error
(templates/variable_template_objects). The built-in subscript takes the pointer on either side: the
walk swaps `2[a]` into `a[2]`, IndexExpr::indexFirst keeps the left operand first
(expressions/subscript_takes_the_pointer_on_either_side). A character literal of two to four characters
is an int (language/multicharacter_literals). `_mm_srli_si128` and `_mm_slli_si128` take the
instruction's byte as their count, zero from 16 on (interop/sse_byte_shift_past_the_register).

### 12. The build description: build.kpp (decided 2026-10-31)
A project's builds are described in `build.kpp` at its root: kpp, run by the compile-time
interpreter, so it can compute but only describes (no I/O, no processes, no C). The project's code
is in `src/`, its own libraries and forks in `libraries/`; registry libraries stay in a shared
cache, pinned in `build.lock`. One project may build several executables from shared sources, a
target leaving files out (Factorio's `Main`) rather than having its `main` replaced. With no
build.kpp, `src/` is one executable with debug and release. Writing `project.use("json")` adds a
library: the next build finds it (`libraries/`, cache, registry) and records it in build.lock. Until
the registry exists, a release brings the official libraries in its `libraries\`, found after the
project's own copy (kovarex, 2026-11-04: the installed kpp alone is enough; build.md, "Where
`use("json")` looks"). In
the project's directory the compiler alone builds it (`kpp`, `kpp --release`). A kpp library is a
project without an executable: its build.kpp says `project.library(NAME)` with its version,
license, minimum kpp, source roots and test roots, its sources join the program of whatever uses
it, and `kpp --test` runs its tests (build.md, "kpp libraries"). Details, the research behind it
and what it needs: [build.md](build.md), "The build description".

**Targets and configurations (decided 2026-11-01).** kovarex: "One axis is built targets, libraries
etc. Second axis is configuration, aka debug, relase, fast relase, valgrind etc. Each named
configuration contains a list of config constant defining it (mainly). When selecting what to
build, we select target + configuration". build() runs once and declares the matrix: named
configurations (`project.configuration("fastdebug", "debug")`, debug and release built in) hold
`config::` constants, `optimize`, `debugInfo` and compiler options; constants are set for the
project, a configuration, a target and a cell, the command line's `--config` over them all. `kpp
TARGET CONFIGURATION` builds one cell into build/<configuration>/<target>.exe, with its own compile
server and cache. On "If configuration is SteamRelease and BuildTarget is FactorioMain: do
something special": an optional `void configure(Cell& cell)` runs once per cell on copies of its
target and configuration. Visual Studio gets one project per target and one solution configuration
per named configuration; the language server checks the chosen cell. Details:
[build.md](build.md), "Targets and configurations".

**Library options (decided 2026-11-01).** kovarex: "when I add new library to my project, I should
have some clear way to get noticed of what config values should be provided to the library, with
defaults or something." A library's build.kpp declares each option with a type, a default (none:
required) and a description (`zlib.option("debug", false, "...")`); its code reads
`config::zlib::debug`. Users set it through `use("zlib").config(...)`, a `zlib::NAME` constant of the
project, a configuration, a target or a cell, or `--config zlib::NAME=VALUE`. An unset required
option, an undeclared one and a value of the wrong type are errors; `kpp options` lists each cell's
values and who set them. Details: [build.md](build.md), "Library options".

**config:: defaults in source (decided 2026-11-04).** kovarex: "yes config:: default should exist. I
would imagine, that libraries can also define their own configs, which can have default, so the
library can be used without filling many values, but the library user can specify them if they
want." A file declares one at its top level, first on its line: `config bool finalRelease = false;`,
`config i32 level = 6;`, `config StringView mode = "fast";`, `config Platform home = Platform::Linux;`
(an enumeration named as from the global scope); `config i32 port;` has no default and is required.
A library's is `config::LIB::NAME`, the program's `config::NAME`. The default is the lowest value:
anything that sets it (build.kpp's project, configuration, target, cell, use(), `--config`) wins, its
value checked against the declared type. Errors: a required one nothing sets, two declarations of one
name that differ, a value of the wrong type, a declaration of kpp's own (debug, platform, mingw,
tests), a library's name declared both in source and by build.kpp's option(), and a `config`
declaration anywhere else (in a namespace, a class, a block). A top-level `config` followed by a name
is always one: a type named `config` there is an error. kpp reads the declarations from the tokens
(src/config.cpp) before any file is parsed, since the parser's declaration-scope if constexpr,
`when` and `requires` read config:: values; the parser never sees one. kpp takes a library's as its
options (`kpp options` shows the default and where it is declared). option() stays for now; whether
source declarations replace it is open. Details: [build.md](build.md), "config:: defaults in
source"; specs tests/spec/kpp/config_default_*.

**The game's DEBUG as config::debug (decided by kovarex, 2026-11-02).** "Obviously we need B the
proper way, we need to have debug and release configuration working properly." The converter
preprocessed the game as Factorio's FastDebug (DEBUG and _DEBUG defined), so the kpp release build
was FastDebug's code without kpp's asserts: 392 DEBUG conditionals compiled in, IsDebug true, a
HashSet in every TransportLine (found by thread B measuring the release build against C++). Each
conditional on DEBUG, _DEBUG or NDEBUG becomes `if constexpr (config::debug)` (NDEBUG
`!config::debug`) at the scope it stands at: function bodies (273), class bodies (61: debug-only
members) and namespace scope (57). A condition's platform parts fold first (`defined(WIN32) &&
!defined(DEBUG)` is `!config::debug`). The converter converts the game twice, with DEBUG and without,
marks each conditional in both, and merges the two texts per file; a difference outside the marks is
reported, never chosen silently. A discarded config branch in a function body is not name-checked,
so it may name what only the other configuration declares (spec
kpp/config_debug_branch_names_debug_only_members). The unit tests' CRCs and the menu maps are checked
in both configurations, and the release build is checked to hold no DEBUG code.

As converted (thread C, 2026-11-02):
- `kpp --convert --convert-marks [--convert-release]` converts the game as FastDebug or as Release
  preprocesses it (Release: NDEBUG for _DEBUG and DEBUG, no DISABLE_EXPRESSION_ID, as fbuild.bff's
  .BuildVanillaRelease), each #if group marked where its first and last directive stood, and each use
  of a macro of the file marked too (a macro may be defined otherwise in the other configuration: Lua's
  api_check, GameRenderer's CHECK_AND_ADVANCE_STATE). `kpp --convert-merge DEBUG RELEASE OUT CHOICES`
  makes the two one text. A group whose two texts are the same is written once, whatever its condition:
  so the platform parts of a condition fold, and a macro that a configuration's #if defines is
  followed to its uses, with no condition parsed. A group whose texts differ, where a statement,
  member or declaration stands, is written by section 19: each declaration of one configuration as
  `when (config::debug)` (`when (!config::debug)`), a local, member, static or namespace-scope one alike,
  so it is the enclosing scope's (a later group's use sees it, an object lives to the end of its block,
  a member is in the layout where the configuration has it); one declaration in each that differs only
  in its initializer as one choosing, `static constexpr u32 ITERATIONS = config::debug ? 16 : 128;`;
  and the statements between as `if constexpr (config::debug) { ... }` blocks, in order. A macro's use
  as a statement is `if constexpr (config::debug) { debug; } else { release; }`. A difference anywhere
  else is reported, and fails convert.cmd.
- A group whose debug text is its release text with asserts (Lua's check_exp, `(lua_assert(c), (e))`
  in debug and `(e)` in release) is written as the debug one: kpp's assert is the configuration's own
  (nothing, its expression unchecked, where config::debug is false).
- overrides/configuration-choices.txt takes a group no `if constexpr` can choose between from one
  configuration for both, with the reason: Main.cpp's create-console option, a part of a cxxopts
  call chain that exists in Release only.
- The converter's own rewrite of a header's macro now stands over the header's definition (C++ files):
  AlwaysInline.hpp's ALWAYS_INLINE, nothing under DEBUG and `__forceinline` otherwise, is
  `[[forceinline]]` in both. A function-like macro that is nothing (CHECK_AND_ADVANCE_STATE in
  Release) is expanded to nothing at its uses, where its uses were left as calls before.
- Counts (C:\Factorio 4c178a5b915, the whole conversion): 4,811 files, 554 groups whose
  configurations differ, in them 159 declarations written `when` and 5 initializers choosing; 459 groups
  whose configurations differ by asserts alone, 1 chosen.
- convert.cmd checks it (tests\configurations.kpp and debug-only.py, "configurations"): the converted
  BuildConstants' IsDebug is true in debug and false in release (`IsDebug = config::debug ? true :
  false`), and TransportLine's referenceSet and Rcu's writeLockCount occur only where config::debug is
  true, in a `when (config::debug)` declaration or an `if constexpr (config::debug)`.
- The scope question (2026-11-02) is answered by section 19: TestNoisePerformance's ITERATIONS chooses
  its value; FogShaderRenderLogic's ROW_STEP is two `when` constants of one name (its release one has a
  comment of its own, which one choosing would lose), and the `static_assert` after them names the one
  that exists; LogisticManager's neighbors, DeliverJob's point, LatencyOneTimeEffect's itTick and
  TextureCopyHelperDX11's sourceWidth are `when` locals.

**The game's FinalRelease as config::finalRelease (thread C, 2026-11-04; docs/final-release.md).**
The official builds are FinalRelease of the all-in-one target: Release with FINAL_RELEASE,
PACKAGED_BUILD and ALL_IN_ONE=1. `kpp --convert-final` converts the game so (with --convert-marks),
and `--convert-merge DEBUG RELEASE OUT CHOICES FINAL` merges the three. All three defines come and go
together, so one constant, `config::finalRelease`, chooses them:
- A group whose release and final texts are the same is chosen by `config::debug` as before. One
  whose debug and release texts are the same, but whose final text differs, is chosen by
  `config::finalRelease` the same way: `when`, `config::finalRelease ? a : b`, `if constexpr` (197 in
  the game). A final build is never a debug one (BuildConstants.hpp's #error).
- One whose three texts all differ (2: IMGUI_ENABLED blocks, which FinalRelease leaves out, with
  DEBUG groups inside) has its debug and release texts merged first, and that text against the final
  one. A `when` inside reads `when (!config::finalRelease && config::debug)`.
- Links of an if-else chain that one configuration alone has (`#ifndef FINAL_RELEASE else if (b)
  {...} #endif`: Main.cpp, PlayerData.cpp, the tests' TestRun.cpp) keep their places, each condition
  joined with the configuration's: `else if (!config::finalRelease && (b))`, `else if
  (!config::finalRelease) x;`. A static_assert of one configuration (Map.cpp's) is
  `static_assert(!config::finalRelease || (c), "...")`.
- A group inside an expression is taken from one configuration (overrides\configuration-choices.txt,
  `release`, with the reason). There are five: SpriteLoaders' PACKAGED_BUILD condition, the developer
  options frame, and three cxxopts developer options. So the kpp final game keeps those developer
  options, and refuses only the internal mods' 16-bit sprites, as Release does.
- The program declares the constant with its default (build.md, "config:: defaults in source"):
  libraries/CommonUtil/FinalRelease.kpp, an added file of the conversion, holds `config bool
  finalRelease = false;` (CommonUtil, as Agui reads it too). factorio-in-kpp's final configuration
  sets it true; every other build, kpp alone included, takes the default (until 2026-11-04 the
  scripts passed `--config finalRelease=false`).

**The game's platforms as config::platform (thread C, 2026-11-05).** The game is converted as Windows
and as Linux too (`--convert-platform linux`, clang's x86_64-linux-gnu predefines and fbuild.bff's
Linux defines), each configuration's two texts made one by `--convert-merge-platform` as the
configurations are (`config::platform == Platform::Windows`, `== Platform::Linux`, each named, never
an `else`), then the configurations by `--convert-merge` (convert.cmd, since 2026-11-05;
CONVERT_LINUX=0 converts Windows alone).

**One spelling on every platform (kovarex, 2026-11-05).** Code that differs between the platforms
outside an #if group cannot be merged, so a name the game spells per platform is one spelling,
declared per platform:
- Strings are UTF-8 on every platform; kpp has no wide or native string literal. The game's wide
  strings (`L ## x` in AppManager's ARG_STRING_TYPE and the tests' ForkTask STR, wchar_t program
  arguments, StringUtil::NativeString) are UTF-8 Strings on Windows as on Linux, converted to wide
  only where the Windows API takes them: wmain's arguments (converted to UTF-8 before fmain, which
  takes `char**` on every platform), ShellExecuteW's parameters (AppManager) and CreateProcessW's
  command line (ForkTask). A path's text in them is its `string()`, UTF-8.
- These are hand edits of the C++ before the converter reads it: programs/factorio/overrides/
  source-edits.txt (`@ FILE: why`, the old lines after `- `, the new after `+ `; every occurrence is
  replaced, on every platform and configuration, and an old text the file no longer has fails the
  conversion, so an edit is never silently stale).
- A C header's macro with a known kpp spelling is that spelling on every platform: errno is
  `(*platform::errnoLocation())` (the stdlib's accessor: `_errno()`, glibc's `__errno_location()`,
  FreeBSD's `__error()`), alloca is the builtin `__builtin_alloca`, which the C import declares on
  every target (malloc.h made it `_alloca` on Windows, glibc and SDL_stdinc.h `__builtin_alloca`). INVALID_SOCKET is a name on both: the game's constant on Linux,
  winsock2.h's, which the C import takes, on Windows.
- A string macro written out at its uses is marked there with the literals it joins, and a use
  whose literals differ is one expression choosing: InputState's `"left-" WIN_KEY_NAME` is
  `(config::platform == Platform::Windows ? "left-" "windows" : "left-" "super")`. A declaration whose
  initializers are literals that differ, strings included, chooses its value as a configuration's
  does: `const auto PLATFORM = config::platform == Platform::Windows ? "win64" : "linux64";`, also
  where a group holds several declarations of which some are the same.
- A use of a macro that is another name (FileOperations' ftello64: `_ftelli64` or `ftello`) is
  marked too, and differs as a group does.
- A group whose platforms' texts differ where no statement or declaration stands (a link of
  cxxopts' option chain, an entry of a brace list or an enum, a constructor's initializer, `||
  condition` in an `if`, the `if (signal != SIGHUP)` before a statement) is widened, on both alike,
  to the statement or declaration around it, which each platform then has whole: `if constexpr
  (config::platform == Platform::Windows) { ... }` and `... Linux) { ... }` blocks, or a `when`
  declaration for each (a constructor, an enum, a variable). The statement's other groups are inside
  it with their marks, and the configurations' merge after it merges them there.
- The forward declarations a converted file keeps at its top (a class no file of the platform
  defines: ChildProcessAgent on Windows) are both platforms' together, one platform's own `when`
  that platform (`when (config::platform == Platform::Windows) class ChildProcessAgent;`).
- Lua's hexadecimal numbers are lobject.c's own lua_strx2number on every platform, as on Windows
  (Linux's luaconf.h made it strtod: a source edit).
- StackWalker is Windows' alone (not in Linux's build): its files `requires config::platform ==
  Platform::Windows`. So is a file whose code is all in #if groups the other platform leaves out
  (the DX11 files, inside `#if defined(WIN32)`): its platform's text as converted, so that the
  configurations' merge merges its groups as before (as one platform group it was each definition
  twice, `when (config::finalRelease && ...)` and `when (!config::finalRelease && ...)`).
- The platform merge writes each group's marks back as they stood (alone on their lines or within
  one), so that the configurations' merge after it cuts each configuration's text alike.
- The C libraries (ogg, zlib, glad, zstd, minizip, fontstash, trio, stb_image) are converted for both
  platforms too (--c-sources with --convert-platform linux: the build's Linux defines and clang's
  predefines as C, release, glibc's LP64 limits and x87's long double; the Windows names their #if
  chains test are known undefined there) and merged by config::platform, each C macro's use marked
  as the game's are. Their own differences are source edits (c-libraries\NAME\source-edits.txt):
  xxhash's XXH_swap32 and XXH_swap64 are its functions on every platform, minizip's snprintf the C
  library's; and trio's TRIO_NO_FLOORL, TRIO_NO_CEILL and TRIO_NO_FMODL keep it from C's long double
  functions, which kpp calls on no platform but Windows (where long double is double).
- Texts that differ in comments alone (zlib.h's list of the library's names) are Windows'.
- The POSIX headers the game's Linux code includes (unistd.h, poll.h, termios.h, sys/socket.h,
  arpa/inet.h, netdb.h, sys/wait.h, arpa/nameser.h, resolv.h...) are imports by their paths, Linux's
  alone in c-imports-linux.kpp; a header only another platform's #if branch includes is no file of
  this one (RazerChroma's SDK headers are Windows' alone); each platform's build leaves out the
  other's files, as fbuild.bff's (Windows' PngLoad, StacktraceToStream and UnitTest++'s Posix
  SignalTranslator, Linux's minizip iowin32), so they are files of one platform; crc32fast is little endian without its own
  __LITTLE_ENDIAN and __BIG_ENDIAN (glibc's endian.h has them).
- Member macros are each platform's (winsock2.h's s_addr and s6_addr on Windows; glibc's s6_addr,
  sa_handler, st_mtime and ifa_broadaddr on Linux), marked so a statement using one is each
  platform's; an if-else chain whose links differ by platform is each platform's whole if statement
  (a link joined with `config::platform == ...` compiled the other's code: zlib's _wopen);
  SIG_DFL is nullptr; intrin.h is imported on every platform (immintrin.h off Windows).
- Windows-only replacements that were override definitions (CrashHandler's minidump without SEH,
  InterruptibleStdioStream's read with fgets) are source edits of the Windows branches, so Linux keeps
  its own code. The game's Linux code that assumed what kpp does not have: a path is wide text on
  every platform (its Path widens UTF-8 as on Windows), the last write time comes from stat, no C++
  ABI names (demangle and the current exception's type), no inline assembly (zstd's cpuid), no long
  double functions (trio's powl, a test's nexttoward as nextafter).
- FreeBSD is converted too (`--convert-platform freebsd`): with the game's Linux code (its defines,
  LINUX among them: FreeBSD runs the POSIX code the game has for Linux) and clang's
  x86_64-unknown-freebsd14.3 predefines (freebsd-predefines.txt). Linux's and FreeBSD's conversions
  are merged first (`--convert-merge-platform LINUX FREEBSD OUT Linux FreeBSD`: 3 groups differ in the
  game), then that with Windows' (`... Windows Linux,FreeBSD`: the second platforms' condition
  `config::platform == Platform::Linux || config::platform == Platform::FreeBSD`), the C libraries
  alike. What FreeBSD does not have of Linux's is a source edit (no inotify: no directory watching; no
  malloc_trim); its member macros are its own (sa_handler, s6_addr, ifa_broadaddr), and PosixMacros.kpp
  has FreeBSD's sys/wait.h macros beside glibc's. A choice's second arm (`?:`) is Linux's and
  FreeBSD's together; another platform (macOS) takes it until it is converted as itself.
- convert.cmd: the whole game's platform merge has no problem (C:\Factorio 4c178a5b915, debug: 246
  groups whose platforms differ, 34 files of one platform), and the merged tree built for Windows
  passes as before (headline 0, tests errors 0, smoke tests 16/16).

### 13. Integer types and sizes (decided 2026-11-01)
kovarex, 2026-11-01:
- "Distinct fixed types, deprecate long".
- "in Factorio, we mostly used the uint32_t and such, so I know what exactly I'm dealing with, it
  should be the standard and language standard names, in C++ these are just custom made macros. It
  shouldn't be uint32_t, it should be the simple ones u8, u16, u32, u64, i8, i16, etc".
- "int, unsigned int, short etc, they become deprecated, I always didn't like that they were so
  ambigous."
- "all the sizes should be unsigned not signed".
- On the size width, decided by measurement on Factorio: "If it doesn't make a difference... just
  have one String/Vector". It made none: sizes are u64 (below).

**The rule.**
- **kpp's integer and floating types are the built-in names** `u8 u16 u32 u64 i8 i16 i32 i64` and
  `f32 f64`. They are the standard way to write a number in kpp. Each is one of C++'s types under
  another name (`u32` is `unsigned int`, `i64` is `long long`, `u8` is `unsigned char`, `f64` is
  `double`), so C interop, layouts and overloads are C++'s: a converted `f(int)` beside
  `f(int32_t)` is one function.
- **C's names are for C interop.** These are `int`, `unsigned`, `short`, `long`, `long long`,
  `signed char` and `unsigned char` as numbers, and `int8_t` ... `uint64_t`, `size_t`,
  `ptrdiff_t`, `intptr_t` and `uintptr_t`. They are kept, with C++'s meanings, for the imported C
  headers and `extern "C"` declarations and blocks (a binding's aliases and C struct mirrors go
  there too). Elsewhere they are deprecated: `kpp --c-type-warnings` warns at each one, naming
  kpp's type, and is never an error. It is opt-in until the converter and the standard library
  have moved, then on by default; whether it later becomes an error is kovarex's call.
- **The types are C++'s, distinct** ("Distinct fixed types, deprecate long"). `char`, `signed char`
  and `unsigned char` are three types, and `int`, `long` and `long long` are three (the kpp v0
  section's "Types" has the rules and specs). `long` is as wide as the target's C `long`: 32 bits
  on Windows, 64 on LP64 targets.
- **Plain `char` (proposed, kovarex to confirm).** It stays as the text character type (a UTF-8
  code unit: String's element, literals), which is no C integer name and gets no warning. Code
  that computes with bytes as numbers writes `u8` or `i8`. The alternative is a new name (`c8`) with
  `char` deprecated like the rest; it buys nothing a reader needs, and every literal and String API
  would change.
- **`bool` stays. `float`/`double` (kovarex to confirm)** are, like the integers, C's spellings of
  `f32`/`f64`: either deprecated the same way (the converter maps them) or kept as plain synonyms.
  The game writes them 20,728 times.

**Proposed built-ins, for kovarex to confirm.**
- **`u128`/`i128`: yes.** LLVM has `i128` on every target. Add, subtract, compare and shift are a
  few 64-bit instructions on x86-64 and arm64, and multiply is about three. Division and remainder
  call compiler-rt (`__divti3`, `__udivti3`, `__modti3`): tens of cycles, as in C. clang accepts
  `__int128` on Windows x64 too, where MSVC has none. Tested here with clang 19 (`clang-cl`):
  division and remainder are right, but the link needs VS's `clang_rt.builtins-x86_64.lib`, which
  kpp would add to the link line. No Windows C API takes one, so calls between kpp functions are
  kpp's own. On Windows x64 clang passes an i128 argument by pointer and returns it in xmm0, and
  LLVM's own library calls followed another convention until recent fixes (LLVM issue 136428).
  So kpp calls compiler-rt only through clang's lowering, never declares such calls itself, and a
  spec covers each operation on every target.
- **`usize` is `u64`**, the width of every size (below), so the game's `size_t` maps to `usize`
  with no type change.
- **`f16` later, not now.** LLVM's `half` is a storage type everywhere. arm64 (every Apple M, and
  ARMv8.2 and later) has native half arithmetic. x86-64 converts with F16C and computes in float,
  except with AVX-512 FP16, which is rare. clang's `_Float16` on Windows x64 works but calls
  compiler-rt's `__extendhfsf2`/`__truncsfhf2` without F16C (tested). It is useful for GPU data;
  the game keeps its own half packing.
- **No `bf16`, no `f128`.** bf16 is storage with conversions only (AVX-512 BF16, newer arm64).
  f128 is soft-float everywhere (compiler-rt's `__addtf3` ..., about a hundred times slower).
  clang refuses `__float128` on Windows x64 ("not supported on this target", tested), and Apple
  arm64 has none.

**What the converter does (C), counted in factorio-in-kpp (src, tests and the converted libraries,
2026-11-01).**
- `uint32_t` 15,689 → `u32`; `int32_t` 14,017 → `i32`; `uint8_t` 5,453 → `u8`; `uint64_t` 2,135
  → `u64`; `uint16_t` 1,458 → `u16`; `int64_t` 837 → `i64`; `int8_t` 466 → `i8`; `int16_t` 345 →
  `i16`.
- `int` 6,623 → `i32`; `unsigned char` 209 → `u8`; `unsigned int` 72 and `unsigned` 26 → `u32`;
  `long long` 59 → `i64`; `unsigned long long` 53 → `u64`; `unsigned short` 22 → `u16`; `short`
  19 → `i16`.
- `long` 61 and `unsigned long` 24 stay `long` where they meet a C API (COM's LONG, zlib's uLong,
  GDI+, curl: most of them), else `i32`/`u32`.
- `size_t` 5,104 → `usize`, `ptrdiff_t` 26 → `i64`, `uintptr_t` 21 → `u64`, `intptr_t` 2 → `i64`.
- `char` 5,646, `bool` 34,224 and `wchar_t` 753 stay; `float` 10,553 and `double` 10,175 as
  decided above.

As converted (thread C, 2026-11-02, C:\Factorio 4c178a5b915; the conversion report's counts, the
converted C++ of src, tests, CommonUtil, Agui, cxxopts, StackWalker, Lua, crc32fast and UnitTest++):
- `uint32_t` 16,421 → `u32`; `int32_t` 14,110 → `i32`; `uint8_t` 4,938 → `u8`; `uint64_t` 2,158 →
  `u64`; `uint16_t` 1,432 → `u16`; `int64_t` 832 → `i64`; `int8_t` 459 → `i8`; `int16_t` 341 → `i16`.
- `int` 5,626 → `i32`; `unsigned char` 200 → `u8`; `unsigned int` 55 and `unsigned` 24 → `u32`;
  `long long` 56 (4 of them `long long int`) → `i64`; `unsigned long long` 53 → `u64`; `unsigned short`
  14 → `u16`; `short` 18 → `i16`.
- `long` 21 and `unsigned long` 23 stay, all of them: C's long is a type of its own, which kpp has no
  other name for, so writing `i32` would change the type (an overload, a `long*` for
  InterlockedIncrement). Whether they meet a C API is not the question until kpp names the type.
- `size_t` 4,683 → `usize`, `ptrdiff_t` 23 → `i64`, `uintptr_t` 21 → `u64`, `intptr_t` 2 → `i64`.
- `float` 9,915 → `f32`, `double` 10,122 → `f64`.
- Kept C's: inside `extern "C"` declarations and blocks, in literals and comments, plain `char`,
  `long double`; and the C libraries (converted by C's rules: zstd, stb_image, fontstash, zlib, trio,
  glad, minizip, ogg) and the hand-written or generated bindings (ImGui.kpp, glm.kpp,
  ComInterfaces.kpp), which are C interop. The names the converter writes itself (a printf piece's
  `uint32_t(x)`) and macro expansions are renamed too: it is a pass over the converted text.
- The parser read kpp's names as plain names where C's are known types: `(usize)-1` and
  `pi_v<f32> * x` (spec language/kpp_number_names_are_types_to_the_parser).

Each rename changes no type, so the IR is unchanged. C's checks are the ones in
C-integer-names-plan: map the new names back and get the old conversion byte for byte. C names
stay in the imported headers, the kpp-native shims (C) and `extern "C"` declarations. The standard
library wrote `int` 983 times, `size_t` 99, `long long` 98, `ptrdiff_t` 92, `unsigned long` 84
(Win32 bindings, which belong in extern "C"), `uint64_t` 62, `unsigned long long` 48 and `uint8_t`
41. It writes kpp's names since 2026-11-05 (1,025 places in core, io, platform and random, by
`--c-type-warnings`, the IR unchanged); C's stay in stdlib/clib (the C library's own interface),
in `extern "C"`, and for `long`, `unsigned long` and `long double`, which kpp has no other name for.

**The size width (decided 2026-11-01: u64, one String/Vector, by measurement).** Containers' sizes
and indices are unsigned (decided). The question was their width: `u32`, `u64`, or a
target-sized `usize`. kovarex asked: "In standard library, it used often size_t, as like the
'natural size', which was at the time solving the 32bit versus 64bit architecture switch (that is
how I saw it). But these days basically everything is 64bit (is it? research). Also research if
there is any impracticality, if standard library is built specifically around 64 bit types instead
of the generic size_t." H benchmarked the stdlib with u32 and with u64 sizes on Factorio (C made it
a one-line switch, removed with the decision).
- **Which platforms are 64-bit.** All of the desktops: Windows x64 and arm64; macOS, which has run
  only 64-bit code since 10.15; Linux. In Steam's survey, nearly every Windows and Linux system is
  a 64-bit one (Windows 11 64-bit 70.8% and Windows 10 64-bit 26.7% in December 2025). The consoles
  are 64-bit: PS4/PS5 and Xbox One/Series are x86-64, Switch and Switch 2 are arm64. So is mobile:
  iOS since iOS 11, and Google Play has required 64-bit since 2019. The 32-bit ones left:
  - WebAssembly's wasm32. Memory64 shipped in Chrome 133 and Firefox 134 (early 2025); Safari's
    status is unclear, and browsers cap a memory at 16 GB.
  - Microcontrollers (Cortex-M) and old 32-bit ARM Linux, which kpp does not target.
- **What 64-bit sizes cost.**
  - Memory: a Vector is {pointer, size, capacity}, 24 bytes with u64 against 16 with u32. Small
    containers inside hot objects (an entity's vectors) cost a third more, and so does cache.
  - Indices stored in data (Factorio's IDs and 32-bit indices) are a separate matter: they stay
    u16/u32 whatever the containers use.
  - Loops: a `u32` index on x86-64 must be zero-extended to address memory, and unsigned
    wrap-around is defined, so LLVM cannot always widen the induction variable or prove a loop
    finite. That can cost a vectorised loop. A `u64` (or signed) index has neither problem.
- **What 32-bit sizes cost.** Every container holds at most 4G elements (a 4 GiB byte buffer at
  most). The game was written against C++'s 64-bit `size_t` (5,104 uses), so with u32 sizes each
  place that passes a `size_t` to a size parameter narrows implicitly.
- **Other languages.**
  - Pointer-sized unsigned: Rust (`usize`, Vec is 24 bytes) and Zig (`usize`).
  - Pointer-sized signed: Go (`int`, 64 bits) and Swift (`Int`).
  - 32-bit signed: C# and Java (`int` lengths).
  - C++ is the pointer-sized unsigned `size_t`, regretted by many (Stroustrup, Sutter, Carruth),
    hence C++20's `std::ssize`.
- **Unsigned traps, and how kpp can help.**
  - `for (u32 i = n - 1; i >= 0; --i)` never ends.
  - `size() - 1` of an empty container is the largest value.
  - `a.size() - b.size() < 0` is never true.
  - A signed/unsigned comparison converts the signed side.

  kpp can warn of each: a comparison that is always true or false on an unsigned (`x >= 0`,
  `x < 0`), an unsigned subtraction compared against 0, and a mixed comparison (as C++20's
  `std::cmp_less` exists for it). Debug builds could trap a size subtraction that wraps.
- **What a decision changes.**
  - u64: one String/Vector everywhere, `usize` = `u64`, and the game's `size_t` maps to `usize`
    unchanged.
  - u32: C's switch, the game's narrowing sites, and a 4G limit.
  - Had the measurement shown a difference that matters, there were ways to have both:
    - a size-type parameter with a default (`Vector<T, u32>`);
    - separate compact containers (`CompactVector<T>` with u32 sizes, for hot objects);
    - a per-project setting. That one changes every library's layout, so kpp.net's packages would
      need a build per setting; it is the worst of the three.
- **The measurement** (H, kpp fe933d5b on Factorio 4c178a5b915; method and tables in
  [test-matrix.md, "Container sizes: u32 against u64"](test-matrix.md#container-sizes-u32-against-u64-kpp-fe933d5b-factorio-4c178a5b915-2026-11-01)).
  u64 against u32, with the same results as C++ at both widths (45/45 map checksums, 9,501/9,501
  unit-test CRCs):
  - time: release 0.983× on the 45 menu maps and 0.994× in the unit tests; debug 1.001× and 1.005×;
  - size: the exe +1.3% in release; peak commit +5.8 MB (median, release) of about 440 MB;
  - each container's header grows by the size fields (Vector 16 → 24 bytes, String 24 → 32), and
    nothing else does;
  - the reason u64 is not slower: the game indexes with `int`. At u32 that int becomes a u32 and is
    zero-extended for the address, which LLVM cannot hoist out of the loop (u32 wraps); at u64 it is
    sign-extended from an nsw counter, which induction-variable widening removes. `zext i32 to i64`
    in the release IR falls from 54,274 to 7,401.

  So the memory cost above is real but small, the loop cost goes the other way, and the 4G limit
  and the narrowing sites go away. Following kovarex's rule, every size is u64 with no switch:
  section 4's sizes bullet (C's) has the library side.

### 14. Text output: arguments in sequence, `Text`, `appendTo` and `<<` (decided 2026-11-01)

kovarex, 2026-11-01: format strings (`printf`'s `%d`, `format`'s `{}`) are fragile, because a text
that is not a literal can contain `%` or `{}` ("someone just does printf(someText)"), and C++
streams are not ideal either. kpp takes the form most languages chose (Pascal's `writeln`,
Python's and Lua's `print`, Go's `fmt.Print`, Nim's `echo`, Abseil's `StrCat`): the pieces as
arguments, one after another, with nothing parsed.

- **Calls:** `print("Hello ", name, " you are ", age, " years old");`. `print(someText)` prints the
  text as it is, always. `println` is `print` with a line end. There is no `printf` in kpp's
  standard library (kovarex, 2026-11-01); C's `printf` exists only as the imported C function that
  converted C code calls.
- **`Text`, the parameter type** (kovarex chose the name): a function that takes text from its
  caller declares `void log(Text text)` and is called as `log("Hello ", name)`, with no
  `log(format(...))` and no variadic template per function. A `Text` refers to the caller's
  arguments, like `StringView`: it is valid during the call only, and storing it (a member, a
  global, a return value, a capture that outlives the call) is a compile error. Its interface is
  `text.appendTo(out)` and `toString(text)`; it cannot be iterated (kovarex: the pieces can be of
  any type, so it is neither useful nor practical).
- **`Text` among overloads:** each argument packed into a `Text` ranks as one C's `...` takes, below
  every standard and user-defined conversion, so an overload taking the arguments as they are wins
  (spec `language/text_parameter_in_overloads`). A `Text` that takes no pieces ranks below an
  overload that matches the same arguments without one: `make(String)` beside
  `make(String&&, Text)` given one String takes `make(String)`, where C++ would call the pair with a
  `...` taking nothing ambiguous (the coordinator and thread C, 2026-11-02, for PropertyItem's
  `makeProperty`; spec `language/text_with_no_pieces_ranks_below_exact`).
- **`appendTo`, the one conversion a type defines:** `void appendTo(String& out) const`. Every
  other form comes from it: `toString(x)` (a new String), `out << x`, `a + b`, `print`, `Text`.
  There is no second, String-returning method to write (kovarex: one method, so a deep hierarchy
  is written in O(n) into one buffer, never O(n^2) through intermediate strings).
- **`<<` chains appends**, in the direction of Factorio's `Serialiser`/`MapSerialiser` (saving is
  `<<`; `>>` stays the reading direction, as `MapDeserialiser`'s): `out << left << " = " << right;`
  calls each piece's `appendTo` on `out` and returns `out`. **`<<` is the only append** (kovarex,
  2026-11-01): String has no `+=`, so `s += 65` cannot be read as a character or as "65"; the
  converter writes C++'s `std::string +=` as `<<` (`out << char(x)` where C++ appended an integer
  as a character). A `<<` chain into a String mixed with `&`, `|` or `^` without parentheses is a
  compile error, since `&` binds looser than `<<`. **`a + b` stays** for one-liners (kovarex: "the
  alternatives don't allow one liners in simple places"): a new String, when one side is a String
  or StringView and the other has `appendTo` (`String s = "x = " + equation;`).
- **Built in and in the standard library:** every integer, float, `bool`, `char`, `String`,
  `StringView` and container has `appendTo`; formatting options are small objects with it
  (kovarex, 2026-11-01, one for every printf specifier the converter meets): `hex(x)`,
  `hex(x, digits)`, `hexUpper(x)`, `hexUpper(x, digits)`, `octal(x)`, `fixed(x, decimals)`,
  `significant(x, digits)` (C's `%g`), `scientific(x, digits)` (C's `%e`), `withSign(piece)` (C's
  `%+`), `alignRight(x, width)` and `alignLeft(x, width)` (fill optional), `zeroPadded(x, width)`
  (the sign first, as C's `%05d`), `address(p)` (16 uppercase hex digits, as MSVC's `%p`). `out << 65` appends the text "65"; a character is
  appended only from a `char`. A float or double prints, by default, the shortest text that reads
  back to exactly the same value (`0.1` is "0.1", `0.1 + 0.2` is "0.30000000000000004"); kovarex,
  2026-11-01: "by default, it should always print all the information, so nothing is lost".
  `fixed(x, n)` is for a chosen number of decimals.
- **One form, the member, for every type** (kovarex, 2026-11-01: "x.appendTo should be a language
  feature ... there is just one way to write this, so for example in templates ... it won't need a
  special case when the type is u32 compared to some class"). `x.appendTo(out)` is valid for every
  built-in type (integers, f32, f64, bool, char): the language provides it, written once in the
  stdlib as the built-in's member. An enum's `appendTo` is provided too and writes its enumerator's
  name (`Direction::north` is "north"; a value that is no enumerator writes `Direction(7)`).
  There is no free `appendTo` for users: a struct imported from a C header or a class of another
  library is printed field by field or wrapped.
- **No silent fallback:** printing, `<<`, `+` or `Text` with a type that has no `appendTo` is a
  compile error naming the type; a pointer prints only through an explicit `address(p)`.
- **Localisation** (where the order of the parts changes per language) is not this mechanism: a
  later localisation library with its own tools (kovarex). `format("{}", ...)` stays only with a
  compile-time literal format; a non-literal format is a compile error.
- **Converted code is kpp too** (kovarex, 2026-11-01: "converting code to kpp is getting kpp code"):
  the converter rewrites every printf-family call, in the game (`ssprintf`, `printf`) and in
  converted C libraries, into `print`/`println`/`toString`/`<<` with the formatting objects for
  the specifiers; where C++ read a value narrower than its type (`%u` of a 64-bit value) it writes
  the narrowing (`u32(x)`), so the text stays identical (the harnesses and the game's CRCs check
  it). So **no format-string checking exists in the language**: the rule "a printf-like call's
  format matches its arguments" is retired. C's `...` stays only at the C boundary: calling a C
  variadic function, and a converted library whose own interface is variadic (trio's printf
  implementation, zlib's `gzprintf`, stb's `writef`); such a call is an ordinary variadic call,
  never format-checked (spec `language/variadic_with_own_format`).
- **Later:** `appendTo` may take a general writer (a file, a socket) besides `String`, so that a
  large save streams to its destination; nothing above changes.

Owners: thread D the language side (`Text` as a parameter type and its lifetime rule, packing a
call's arguments into it, the missing-`appendTo` error); thread C the library side (`appendTo`
throughout the stdlib, `operator<<` and `operator+`, `print`, `toString`, and the formatting objects,
docs/stdlib/).

As converted (thread C, 2026-11-02):
- A call of `ssprintf` with a literal format is `toString(pieces)`, a `printf` statement
  `print(pieces)`: the format's text between its conversions as literals (raw ones for a raw
  format), each conversion its argument as the piece that writes it (`%u` `uint32_t(x)`, the 32 bits
  printf read; `%s` `StringView(p)`, or the String a `.c_str()` came from; `%5.2f`
  `alignRight(fixed(x, 2), 5)`; `%08x` `hex(x, 8)`; `%#010x` `"0x", hex(x, 8)`; `%c` `char(x)`; `%p`
  `address(p)`), `::` before a library name a member, local or parameter hides (NoiseProgram's
  `toString()`, Color's `hex()`).
- **The game's ssprintf is trio's text, not the C runtime's.** CommonUtil's ssprintf formats with
  `trio_vsnprintf` (StringUtil.cpp), whose floating conversions round as trio does: `%f` of 1.4140625
  is `1.414063` (a tie away from zero), where the C runtime and `fixed(x, 6)` give `1.414062`; `%.16f`
  of the double LuaPrintNumbers' 11th random value is `0.6643904868979008`, the exact rounding
  `...007`. The game's checksums hold that text (two unit tests' map CRCs). So ssprintf's `%f`,
  `%g` and `%e` are `trioFixed`, `trioSignificant` and `trioScientific`, the game's own pieces
  (programs/factorio/overrides/added/CommonUtil/TrioPieces.kpp, wrapping the converted trio's
  snprintf); widths, `+` and the `0` flag are the stdlib's `alignRight`, `withSign` and
  `zeroPadded`. A `printf` statement prints through the C runtime, whose text `fixed` and
  `significant` are (1.8 million values compared). The stdlib keeps exact rounding (docs/stdlib/text.md);
  whether the game switches to it is a change of the game's behaviour and checksums: kovarex's call,
  later.
- Counts (C:\Factorio 4c178a5b915): 3,306 ssprintf calls written `toString(pieces)` and 146
  `printf` statements `print(pieces)`; 605 `trioFixed`, 259 `trioSignificant` and 1 `trioScientific`
  among them.
- Left as they are (counted in the conversion report): 8 `*` widths and 2 `*` precisions, 32 calls
  whose format is chosen at run time (`global->localiseFormat(...).c_str()`, a translated key, a
  variable), about 460 calls inside macros the converter expands (the unit tests' CHECK macros, whose
  expansion is text the rewrite does not see), and conversions without a piece (the ' ' and `'` flags,
  `%E` `%G` `%a`, precision on integers and strings, wide strings: none in these calls).
- Step 2 (kpp 8613ccd6, whose message names only docs/build.md: the converter's step 2 and the Util
  smoke test's Log stub were swept into it from a work-in-progress commit; it had passed convert and the
  game gate, and master was gated in full after it). The game's own format functions (C-variadic,
  their last named parameter `format`: Logging's log, logVerbose and logAndAbortOrThrow, TagWriter,
  addDebugProperty, makeProperty, AbortException::make, InteractiveTestSelect::printf; 16
  declarations) take `Text text`, their bodies `toString(text)` where they called vssprintf, so the
  text is still written only where the C++ wrote it (TagWriter after its early return for saves not for
  humans, addDebugProperty when debug properties show). Their calls, and the Log macros' and
  SAVING_TAG's, are pieces: 1,090 macro calls, 139 direct calls, and 455 format calls inside macro
  expansions (a text-level rewrite of the expansion, each value with its type written: `int32_t(x)`,
  `StringView(p)`). A format chosen at run time, or literals the rewrite does not read (a C header's
  string macro among them), passes `ssprintf(format, ...)` as one piece, trio's text at run time, or
  the format alone as `StringView(format)` (43); a std::string there is no format (it took the
  string overload in C++ too). A `%s` of a pointer to another type (glGetString's `const GLubyte*`),
  or of an expression whose type the converter does not know, is `StringView((const char*)(x))`, as
  printf read it. D's fd763247 ranks a Text taking no pieces below an exact overload, so
  `makeProperty(x)` keeps `makeProperty(String)`. Left: 20 format calls in macro expansions whose
  arguments the text-level rewrite does not split (they compile as they are: strings, no
  conversions).
- A `%s` of a `.c_str()` or `.what()` call is C text whatever the class (kpp dd480a7c): 810 of the
  1,084 `StringView((const char*)(x))` became `StringView(x)`, 274 left.
- std::to_string, by the overload C++ calls for its argument: an integer's is `toString(x)` (9 the
  converter's types show), a floating value's `toString(fixed(x, 6))` (8; std::to_string writes C's
  `%f` through the C runtime, not trio), and the rest's `toStringAsStd(x)` (307: a bool, a char or an
  enumeration, which C++ promotes to int, and the many arguments whose type the converter does not
  know: members, calls' results, locals), the game's template
  (programs/factorio/overrides/added/CommonUtil/ToStringAsStd.kpp) that picks among those by the type
  the compiler knows.
- printf is retired (kovarex, 2026-11-02: converted code is kpp). The C libraries' calls are
  pieces too: zstd's 104 `fprintf(stderr, ...)` (its dictBuilder's DISPLAYLEVEL messages, in macro
  expansions) `print(stderr, pieces)`, the stdlib's print(FILE*, Text) as C++23's std::print(FILE*,
  ...) (docs/stdlib/text.md); minizip's 5 printf statements `print(pieces)`. A `*` width is the
  argument before the value, the piece's width (alignRight and alignLeft take a negative width as
  printf does, aligned left): LuaHelper's 8 `printf("%*c...", depth, ' ', ...)` are
  `print(alignRight(char(' '), depth), ...)`. The C libraries' own variadic interfaces keep C's `...`
  (trio's printf family, zlib's gzprintf, stb's writef), and a write into a buffer has no piece. The
  exceptions, left as C writes them: minizip's 5 `_snprintf`, zlib's 3 `snprintf` and gzprintf's
  `vsnprintf`, stb_image's `sprintf`; in the game, PropertyTreeJson's `snprintf(buffer, ..., "u%04x",
  c)`, StringUtil's `sprintf(s, "%.14g", value)` and PerformanceLogger's 2 `snprintf(buffer, N,
  format, ...)` of a run-time format.

### 15. File-local names: `private` at file scope (decided 2026-11-01)

kovarex, 2026-11-01: "lets keep the private for per file private". A top-level declaration marked
`private` (`private const int COPY = 1;`, `private void helper() {...}`, a private type or
variable) is visible only in the file that declares it:
- inside its file it hides a global name of the same spelling; two files' `private` names never
  clash; naming it from another file is a compile error ("helper is private to zlib/inflate.kpp");
- everything not marked stays global and order-independent, as before.

Why (measured 2026-11-01 by the coordinator, a text scan of top-level declarations): Factorio's
own source has 125 file-level `static` declarations and 7 anonymous namespaces in 7,191 files, with
no name repeated across files, so there it marks internal helpers. The 15 C libraries the game
uses have about 16,900 file-local declarations (`static` functions and variables, `#define` in `.c`
files) and about 500 names reused across files (SDL3 316, curl 82, jpeg 28, zstd 26, Lua 24), which
the converter would otherwise rename one by one, again at every upstream update.

The converter maps C's file-level `static` and a `.c` file's own macros (as constants) and C++'s
anonymous namespaces to it. Owners: thread A the language (resolver, specs), thread C the
converter. It replaces the renaming lists (`private-names.txt`) and the macros written out as
values.

As built (thread A, 2026-11-01; specs `kpp/private_*`):
- `private` goes before the whole declaration (`private template<typename T> struct Box {...};`)
  and applies to every name it declares: a function, a variable or constant, a class, an enum, an
  alias, a template. A member defined out of line (`int Packer::add(int)`) is its class's.
- Only in the global namespace: `private` inside a namespace is an error ("private is for a file's
  top-level declarations, not a namespace's"; the game's 7 anonymous namespaces are all at file
  scope), and so is a private namespace, import or `extern "C"` declaration.
- A file's private names are a scope of their own. A lookup made for the file (from its functions,
  its global classes' members, its namespaces' functions, and `::name`) finds them before the
  global scope, so they hide a global or standard-library name of the same spelling there. A global
  template whose body calls its file's private helper calls that helper wherever it is
  instantiated.
- Two private declarations of one name in one file are the usual clash ("top-level names are
  global"); two files' never meet. Naming another file's private name is "'helper' is private to
  PATH".
- In the IR a private entity's symbols and types carry its file (`inflate.kpp@<its file's
  tag>::helper`), so two files' private functions, globals, classes, vtables and template
  instances of one name stay apart. The tag is the one that names the file's lambdas, a hash of
  its path under its root, unique in the program (B, 2026-11-04; it was the hash of the absolute
  path, so the names, the PDB and the modules' hashes differed from checkout to checkout).
- The compile server rebuilds in full when a function made private or global is added or removed
  (the private scope comes from a whole index); body edits in a file with private names stay
  incremental.

As converted (thread C, 2026-11-02; the C libraries' harnesses byte-identical):
- A `.c` file's top-level `static` functions and variables are `private` (the `static` goes, also
  one a keyword macro writes: zstd's `FORCE_INLINE_TEMPLATE` is `private inline`); so are the types
  it defines that no header of its library spells (trio's math.c `ieee_double_shape_type`), its
  anonymous enums, and its functions whose declarations name such a type. 1,317 declarations in
  the 8 converted C libraries; the renames of statics with their file's name are gone (cover.c's
  `g_displayLevel` was `cover_g_displayLevel`).
- A C header given alone (a header-only library's implementation, stb_truetype.h): its statics.
  Its types stay global, since they may be its C interface's.
- A header's own names stay global: `private` would keep them from the files that include it.
  private-names.txt stays for those (zlib's inflate.h enumerator `OS`, beside an imported `OS`;
  stb_truetype.h's fallback packer types `stbrp_*`), renamed with their file's name as before.
- A `.c` file's own object-like macro that is a number the same at every use is a
  `private const auto` of its file, its uses its name: defined once (not #undef'd), at file
  scope, its body (the header macros it names written out, as the preprocessor rescans) a literal,
  a negative one or parenthesized as a whole, holding a number, no string and no name but its
  file's other such constants and type words (`sizeof(int)`). 107 in the C libraries (zstd 47,
  minizip 27, zlib 16, trio 14, glad 3). The rest stay written out at their uses: `#define SIZE
  1024*1024` (`x / SIZE` would change), strings (sizeof), bodies naming a variable (the use's local
  in C), macros defined twice, and every header's macro (each library's interface: its home is the
  library's scope, section 16, where a scoped library's interface headers make them constants).
- C++ (the game): a source file's `static` functions and variables at the global namespace's scope
  and its anonymous namespaces at that scope, unwrapped, their declarations each `private` (Atlas.cpp's
  helpers, AppManager.cpp's ConfirmationBoxConnectToServer): 93 declarations, the 7 anonymous
  namespaces. Not in a named namespace (private is no kpp there) nor in a header (other files
  include it; a header's `static` goes, as before).
- Not yet: declarations the converter's parse does not read (zstd_compress.c's
  ZSTD_encodeSequences family, after a parse error at `FSE_CTable const*`), which keep their
  `static`.

### 16. Library scopes: qualified by default, a library may be unscoped (decided 2026-11-01)

kovarex, 2026-11-01: "adding library means that I have to change unrelated code to remove
ambiguity is not nice"; "when I read zlib::close, I instantly know what is going on, compared to
close"; "libraries can decide. The good standard for libraries like zlib, etc, would be to be
scoped ... std (or core) of kpp would be marked unscoped, but someone can make a library which is
unscoped if they wanted."

- **A library's names are qualified by its name by default:** a program or another library writes
  `zlib::compress`, `json::Value`. Inside the library its own names are bare. The qualifier is the
  library's name (`project.library(NAME)`, the registry name), so it is unique.
- **A library may declare itself unscoped** in its build.kpp (`lib.unscoped = true`): its names are
  bare everywhere, as today. The standard library is unscoped, so `String`, `Vector` and `print`
  stay plain. An unscoped library's names share the one global scope: a clash with the program,
  another unscoped library or a C import is the compile error of "One global scope" below, at
  build time, never a silent pick.
- **Adding a scoped library never changes the meaning of existing code**, and never makes it
  ambiguous: its names are reachable only through its qualifier.
- **No per-file imports or aliases** (`using namespace zlib` does not exist for libraries): a
  snippet means the same in every file of the program, as before.
- **Unchanged:** the program's own names, imported C headers (C has no scopes) and C++ namespaces
  written in code stay as they are; `private` (section 15) still makes a name file-local; inside one
  library, two declarations of one name are the global-clash error.
- **For the registry:** scoped is the norm for published libraries; unscoped is shown on the
  library's page and is meant for a project's own libraries and foundations like the stdlib.
- **For converted code:** each converted library (zlib, ogg, ...) is scoped by its name, and the
  converter qualifies the game's calls into it (`zlib::inflate`), while its C ABI symbols keep their
  C names for C callers. Whether Factorio's own CommonUtil, Agui and Util are scoped is decided when
  they become libraries.

Owners: thread A lookup and the errors; thread E `unscoped` and the qualifier in build.kpp's
description (done: kpp passes each used library's roots as kpp's `--library-root NAME DIR` or
`--unscoped-library-root NAME DIR`, build.md "Library scopes"); thread C the stdlib marked unscoped and the converter's qualification; thread I the
registry's docs. This supersedes the "distinct names" answer below for libraries; the package tree
for browsing stays as proposed.

As built (thread A, 2026-11-02; specs `kpp/library_*`, `kpp/two_libraries_*`, `kpp/unscoped_*`,
`kpp/using_namespace_of_a_library_is_error`):
- kpp takes `--library-root NAME DIR` and `--unscoped-library-root NAME DIR`, both repeatable (one
  library may have several roots, its tests' among them); DIR may be a file, `--exclude` applies,
  the positional roots are the program. NAME must be an identifier and no keyword; one NAME given
  both scoped and unscoped is an error. Files are ordered the standard library first, then the
  libraries in the order given, then the program. A build of a library's roots alone needs no
  positional root.
- A scoped library's files are as if their top level were inside `namespace NAME { }`: their
  top-level declarations have that namespace for parent, so lookup inside the library finds its
  names bare, the program finds them as `NAME::name`, argument-dependent lookup finds a library
  class's functions (a key's `hashOf`), and the IR's symbols and types carry `NAME::`, so two
  libraries' functions, globals, classes, vtables and template instances of one name stay apart.
  An `extern "C"` function keeps its C name. `::name` in a library is the global scope's name,
  not the library's.
- `private` in a library's file (section 15) works as in the program's: its private names hide the
  library's own of the same spelling in that file.
- A bare use of a scoped library's name outside it is "'compress' is not declared; did you mean
  zlib::compress?". A declaration of the program that has a scoped library's name at the top
  level, or a namespace of the program that opens it (`namespace zlib { }`, `namespace zlib::x`),
  is "'zlib' is the name of the library zlib, which qualifies its names"; `using namespace zlib;`
  and `namespace z = zlib;`, anywhere, are "'zlib' is a library: its names are written
  zlib::name". Two declarations of one name in one scoped library are the usual clash.
- An unscoped library's names are the global scope's, as the program's are; its name is for the
  diagnostics alone (`zlib::x` does not reach an unscoped library). A clash with the program's says
  "declared here and in the library util (PATH)"; the program's declaration is the one reported.
- The compile server rebuilds in full when a scoped library gains or loses a top-level function
  (its namespace comes from a whole index); body edits stay incremental.

As converted (thread C, 2026-11-03; zstd first, then zlib, minizip, stb_image, fontstash and trio;
glad stays unscoped, its build.kpp says why; CommonUtil, Agui and Util not this round):
- A converted C library is scoped when its `c-libraries/NAME/interface.txt` names its interface
  headers (zstd's zstd.h, zstd_errors.h and zdict.h). They are converted with the library, not kept as C
  imports: their declarations are kpp in the library's scope. The library is a kpp library of the
  game's build.kpp (`e.use("zstd", local("libraries/zstd"))`, its build.kpp `root(".")`, copied
  beside it by convert.cmd), kpp's `--library-root zstd` in convert.cmd's builds. Its C functions
  keep `extern "C"` and their C names, so the C symbols are unchanged.
- **An interface header's macros are the scope's names:** a number is a constant (`const auto
  ZSTD_CONTENTSIZE_UNKNOWN = (0ULL - 1);`) by section 15's rule for a `.c` file's own constant,
  public, and also when it is a choice (`((int)(sizeof(size_t) == 4 ? ... : ...))`), another
  constant's name (`ZSTD_CHAINLOG_MIN ZSTD_HASHLOG_MIN`) or an enumerator's (`ZSTD_STRATEGY_MIN
  ZSTD_fast`, of the enumeration's type). The library's files write it by name where C's code
  names it, and as its value inside another macro's expansion, as the preprocessor rescans
  (ZSTD_VERSION_STRING stringizes the version's numbers). A function-like macro computing a value
  from its arguments is a function, each parameter its own template type, so the arithmetic is
  the macro's on the argument's type: `template<typename T1> constexpr auto
  ZSTD_COMPRESSBOUND(T1 srcSize) { return ...; }`. Uses inside the library stay expanded.
- **What is not expressed is reported, never left out silently:** each such macro is a TODO of
  the conversion report, "macro of the library's interface not expressed in kpp, its users have no
  NAME". zstd: 54 constants and 1 function, and 6 not expressed: 4 of the version's text,
  ZSTD_LIB_VERSION (`1.4.0` as tokens), ZSTD_QUOTE and ZSTD_EXPAND_AND_QUOTE (stringizing) and
  ZSTD_VERSION_STRING (made of them; ZSTD_versionString() gives it), and zdict.h's deprecation
  attribute, ZDICT_DEPRECATED and the ZDICT_GCC_VERSION it tests. Guards and linkage words
  (ZSTDLIB_API) are no value.
- **The checks:** zstd's harness (programs/factorio/harness/zstd.c) prints each constant's value,
  size and signedness, and ZSTD_COMPRESSBOUND on a size_t, an int, an unsigned char and a
  negative long long with its result's size; the C build and the converted one print the same
  bytes (2,719 lines). The harness converts a scoped library as convert.cmd does: the library on
  its own, then the driver as the program.
- **The game:** kpp `--convert-library NAME DIR` gives the conversion the converted library; its
  names are the top-level declarations that are not private of its interface files, those the
  converter marks so: what C's users see by including the headers, not an internal header's
  (zstd's mem.h declares `BYTE`, which the game takes from Windows' headers). The game's uses are
  written qualified (`zstd::ZSTD_compress`, `zstd::ZSTD_inBuffer`), except a name the program
  declares itself, and its includes of the library's headers (`zstd/zstd.h`) import nothing. The
  harness driver: 247 uses qualified.
- **zlib and minizip (2026-11-04):** zlib's interface is zlib.h and zconf.h; minizip's is unzip.h,
  zip.h and ioapi.h. minizip uses zlib (c-libraries/minizip/uses.txt: its conversion and its
  build.kpp's `use("zlib", ...)`), so zlib's names are `zlib::` in it too.
  - Counts: zlib has 38 constants and 5 functions; minizip has 29 constants and 8 functions.
  - Not expressed (TODOs of the report): zlib's `zlib_version` (`zlibVersion()`, which its users
    call instead), and minizip's fopen64, ftello64 and fseeko64 (macros naming the C runtime's
    functions). A macro beside a function of its name (gzgetc) is the function. zlib's identity
    macro OF, guards and linkage words are no value. C's standard names (SEEK_SET) stay the C
    headers'.
  - What made them convert: a local's name shadowing a library name stays unqualified (a local
    `compress`), and the program's own redeclarations of the library's names (ZipPackage's forward
    declarations) are removed and their uses qualified. Casts the converter inserts write the
    type qualified, and `__int64` in an alias is `long long`.
  - The harnesses print every constant's value and size (zlib 4,115 lines, minizip 1,015), and the
    C and converted builds print the same bytes. In the game, 75 uses of `zlib::` and 62 of
    `minizip::` (4 files each).
- **fontstash (2026-11-04):** fontstash.h and the stb_truetype.h it includes are the whole library
  (header-only), so its interface headers are its sources too. Its c-headers.txt is gone: a header
  that file lists stays a C import, and its declarations are left out of the conversion.
  - Counts: 21 constants, 11 functions and an alias in the scope. Not expressed: 19 (stb_truetype's
    configuration macros, which name the C runtime's functions, STBTT_sqrt and STBTT_malloc, and
    FONS_NOTUSED). The game uses none of them.
  - A header-only library defines macros inside its functions (stb_truetype's STBTT__CSERR): only a
    #define at the file's scope is an interface name (a header's `extern "C" {` block is the file's
    scope). The others stay the local macros they were.
  - The harness prints its 9 constants' and 13 enumerators' values and sizes (an enumerator's size
    since sizeof takes one, 7bdb93be), 3,289 lines, the same in both builds. In the game, 52 uses of
    `fontstash::`.
- **stb_image and trio (2026-11-04):** stb_image's interface is stb_image.hpp and
  stb_image_write.h; trio's is trio.h and triodef.h. With them every C library the game converts is
  scoped but ogg and glad.
  - Counts: stb_image has 6 constants and 17 functions, trio 11 functions and 2 aliases. Not
    expressed: stb_image 20 (its configuration macros naming the C runtime's functions, STBI_MALLOC,
    STBI_ASSERT, stbi_lrot), trio 12 (TRIO_CONST, TRIO_PROTO_PRINTF and the like, words of its
    declarations). The game uses none of them.
  - Only an interface header's include is removed: another header of the library's directory
    (stb_dxt.h, stb_image_resize.h) stays the C import it was.
  - A scoped header's include still gives its includer the C headers the library imports (its
    c-imports.kpp's): Lua's override_printf.h took limits.h's UCHAR_MAX (for MAXUPVAL) and
    stdio.h's BUFSIZ (LUAL_BUFFERSIZE) through trio.h.
  - A global written `::name` is `stb_image::name` (stbi_write_png_compression_level), and a name
    of the library inside a macro's expansion text is qualified too (Lua's override_printf.h
    defines fprintf as trio_fprintf, so lauxlib's `fprintf(stderr, ...)` is `trio::trio_fprintf`).
  - The harnesses: 4,236 and 2,278 lines, the same in the C and the converted builds. In the game,
    13 uses of `stb_image::` (2 files) and 20 of `trio::` (7 files, Lua's and CommonUtil's,
    TrioPieces.kpp's floating text among them).

**The language side as built (thread D, 2026-11-01; specs `language/text_*`):**

- **What a Text is:** stdlib/core/Text.kpp, which the compiler builds directly as it does
  InitializerList: a pointer to the pieces and their count, each piece the address of its
  argument and the function that appends it. The called function is compiled once; each piece
  type has one small appending function (`appendMemberPiece<T>`, `appendFreePiece<T>`,
  `appendCTextPiece`).
- **Which arguments:** a parameter of type `Text` is the last one. It takes every argument from
  its position on, zero or more, in order, evaluated left to right. A single argument that is
  already a Text passes through. Calls, member calls, calls through a lambda or a function
  pointer, constructors (`Problem("bad ", point)`, `throw`, a variable's `(...)`, `new`, a
  member's initializer) and an operator's right operand (`out << a`) pack. Packing ranks
  as C's `...` does, below every standard and user-defined conversion, so an overload taking the
  arguments as they are wins.
- **Lifetime:** the arguments live to the end of the caller's full expression, as a reference
  parameter's temporaries do.
- **The type's conversion (an error naming the type otherwise):**
  - a class: its own or a base's `void appendTo(String& out) const` (a class converting to
    something printable has no appendTo of its own);
  - a built-in type: its `appendTo`, which `x.appendTo(out)` calls as for a class, in a template
    too (`t.appendTo(out)` with T = u32). The standard library writes it as a free
    `void appendTo(T value, String& out)` per built-in type in stdlib/core/Text.kpp (decided
    2026-11-01, the coordinator for kovarex: option (b)). The checker rewrites the member call on a
    built-in into a call of the library's function taking exactly T, so no promotion makes a u8
    print through the i32's. An enumeration gets its enumerator's name the same way, or
    `Direction(7)` for a value that is no enumerator (Text.kpp's template, through the compiler's
    intrinsics kppEnumeratorName and kppEnumTypeName); one with its own appendTo method uses it.
    Specs: language/append_to_on_built_in_types, language/append_to_on_enumerations;
  - a free `appendTo` the program writes is an error, as it would never be called;
  - a string literal or an array of chars: C text, to its first zero;
  - a pointer is no piece (the error points to `StringView(p)` or `address(p)`).
- **Compile errors (the lifetime rule):**
  - a Text anywhere but a parameter or a local initialized from a Text;
  - a data member, a global or static, a function's result, a pointer to one, `new`;
  - a class template argument (`Vector<Text>`);
  - a lambda capturing a Text (or such a lambda) given to another type (a `Function`, a class
    parameter, an assignment). As its own type it may be called, kept in a local and passed to
    a function template;
  - a Text not last, defaulted, or a parameter of an `extern "C"` function.
  - Errors inside the library's code are reported at the program's line that asked for it.
- **Text in `+` and `<<`:** a string literal plus or minus an integer is C++'s pointer
  arithmetic, never text, so it is an error (`&"abc"[n]` writes the pointer). So is a literal
  plus a pointer or another literal. A `<<` chain into a String mixed with `&`, `|` or `^`
  without parentheses (`out << flags & mask`, which is `(out << flags) & mask`) is an error.
  Specs: language/string_literal_plus_integer_is_error,
  language/shift_chain_with_bitwise_operator_is_error.
- **`format` with a literal format:** through C++20 `consteval`, which kpp now enforces. A call
  of a consteval function outside another consteval function, including an implicit consteval
  converting constructor, is evaluated where it is written. One that is no constant (a run-time
  argument, a throw) is an error naming the function. The library's
  `consteval implicit FormatString(const char*)` makes `format(runtimeText, x)` an error, and
  its constructor can reject a malformed pattern.
- **Pieces against the format (2026-11-02):** a parameter whose class has
  `consteval void checkPieces(ContainerSize pieces) const`, right before a Text parameter, is
  checked at every call: checkPieces runs at compile time on the argument's constant value with
  the call's piece count (a pack expansion counts its elements, per instantiation), and a throw is
  an error at the call naming the class, the count and the message. So `format("{} {}", a)` never
  compiles once FormatString counts its `{}` and declares checkPieces. An argument that is no
  constant there (a FormatString parameter passed on to format) is not checked again: the call
  that built it was. Chosen over C++'s `format_string<Args...>` (a FormatString<N> parameter of a
  template format), which would make every format call a template instance (the coordinator's
  ruling). A consteval conversion to a deduced function template's parameter
  (`CountedFormat<sizeof...(A)>`) is evaluated for the call's deduced arguments.
  Specs: language/text_check_pieces, language/text_check_pieces_mismatch_is_error,
  language/consteval_conversion_to_deduced_parameter(_mismatch_is_error).

### 17. A self-contained release (decided 2026-11-02)

kovarex, 2026-11-02, after the first installer asked for Visual Studio's clang: "I definetly want
self contained package". A kpp release compiles and links a program on a Windows machine with
nothing else installed: no Visual Studio, no Build Tools, no Windows SDK.
- The release carries its own back end: LLVM's clang and lld (Apache-2.0 WITH LLVM-exception, the
  same licence as kpp), and the C headers, import libraries and C runtime a program links against.
  The candidate for the last is MinGW-w64's UCRT variant (headers and import libraries generated
  from its .def files, linking the ucrtbase.dll every Windows 10 and 11 has), as Zig does; the
  Windows SDK's own files are not redistributable.
- An installed Visual Studio or Windows SDK stays usable, chosen explicitly; never a silent
  fallback from one to the other.
- The check: on a clean Windows (Windows Sandbox), the installer, `kpp new hello`, `kpp hello --run`
  work with nothing else installed; the game's build is the second check (D3D, COM and the
  SDK-only headers it imports), and any header the bundled set lacks is listed, not guessed.
- Staged: first clang and lld bundled (the Windows SDK still needed), then the headers, import
  libraries and runtime. Thread B owns it, with C for the stdlib's C runtime use and E for the
  installer and package.
- Any host builds any target (kovarex, 2026-11-02): the bundled lld links every format, and each
  target's headers and libraries are laid out side by side (Windows from MinGW-w64, Linux, FreeBSD's
  base, macOS later), so `--target` works from Windows and Linux alike (macOS as a host is under
  consideration, 2026-11-06, "Targets and hosts"); tests still run on the target.
- Less Microsoft-specific tooling is preferred (kovarex: "I personally prefer needing less microsoft
  related stuff than more"): the destination leans to the GNU dialect on Windows too (the Itanium
  exception model and DWARF, which Linux and macOS need anyway), reached in steps; the game keeps an
  explicit `--toolchain msvc` until its Visual Studio-built C++ libraries are converted or rebuilt.
- Measured before deciding (kovarex: "incremental rebuild time is one of the most critical parts of
  the kpp advantage over C++"): the game's debug leaf-edit and wide-edit rebuild with lld against
  today's incremental link.exe, the release link, release run time (menu maps, unit tests) of the
  MSVC and the GNU dialect, and executable and debug-info sizes. A slower edit loop keeps the
  incremental link.exe as an explicit debug option, never a silent switch.

### 18. Versions (decided 2026-11-02)

kovarex: "every commit should add to the smallest version". kpp's version is MAJOR.MINOR.PATCH:
- **MAJOR.MINOR** is the file VERSION at the repository's root. It changes only by kovarex's decision.
- **PATCH** is the number of commits on master's first-parent history since the commit that last
  changed VERSION. The commit that changes VERSION is X.Y.0 and every later commit adds 1, so no
  file is edited per commit and parallel threads never conflict over it. kovarex's example: after
  0.0.158 comes 0.1.0, not 0.1.159.
- The build works it out (compiler/version.cmake) and puts it in kpp, with the commit. A build
  with uncommitted changes adds "+dirty". A build outside git (a source zip) says "unknown" rather
  than guessing. A shallow clone that lacks VERSION's last change is an error, so CI fetches the
  whole history.
- `kpp --version` prints it. A release is named by it (tag v<version>): package.ps1 takes it from
  the kpp it packages and never from its command line (kovarex: "take the version from the
  versioning system always, not from command line").
- VERSION starts at 0.2: CI had already published v0.1.141 to v0.1.146 under the run-number
  scheme, and 0.2.0 sorts above them.

docs/build.md, "Versions", has the details.

### 19. Conditional declarations: `when (condition) declaration;` (decided 2026-11-03)

A declaration that exists in the current scope only when a constant condition holds:
```cpp
{
  when (config::timeMeasuring) TimeMeasurer timeMeasurer("buildPhase");
  buildPhase();
}   // when it exists, its destructor runs here, as in the C++
```
- The condition is any constant expression, as `if constexpr`'s: a `config::` value, or a property of
  a template's types (decided per instance).
- When it is false, the declaration does not exist: no storage, no constructor, and its initializer is
  neither evaluated nor checked, as a discarded branch. A false `operator=` leaves the class its implicit
  assignment, as when it is not declared at all (`#ifdef DEBUG` around it in the game's classes).
- When it is true, it is an ordinary declaration of that scope: the same lifetime and the same place
  of its destructor.
- A use of the name is checked in the configuration (and template instance) being built: where the
  declaration does not exist there, the use is a compile error in that build, never a silent fallback
  to another declaration of the same spelling (a `when` declaration that does not exist still hides
  outer names). A use inside a discarded branch (`if constexpr` on a false condition, or another
  `when` that does not exist) is not checked, as any discarded code.
  kovarex, 2026-11-03, first decided a structural check reporting a misplaced use in every
  configuration, then reversed it after the counter-example of several `when` declarations of one name
  under unrelated or platform conditions: "the conditions and logic can be complex, and we can't cover
  everything ... only report errors for this specific configuration being built right now".
- `if constexpr` keeps C++'s scoping; kpp adds no exception to it.
- It stands wherever a declaration may: in a block, at namespace scope, and as a class member
  (kovarex, 2026-11-03: "Having it for members makes 100% sense", for statistics and debug members).
  A `when` member is part of the class's layout only where its condition holds, per configuration and
  per class template instance. A constructor's member initializer for a `when` member exists exactly
  when the member does, as part of that member's declaration.
- Several `when` declarations may declare one name in one scope (kovarex, 2026-11-03), as an
  `#ifdef`/`#else` pair did: `when (config::debug) DebugTimer timer("x"); when (!config::debug)
  NullTimer timer("x");`. It is decided at compile time: where two of them exist at once, in a
  configuration or a template instance, that is a clash and a compile error there.
- `when` is a keyword. It is not one in C++, and the converted game never uses it as a name.

Why (kovarex, 2026-11-03): converting `#ifdef DEBUG` to `if constexpr (config::debug) { }` gave each
block a scope of its own, which broke a debug-only local used by a later block, a constant whose
value differs per configuration, and an RAII object such as a time measurer, whose lifetime would end
with the block. Making `if constexpr` scope-free for config conditions, or adding a separate
scope-free `if`, was rejected as a special exception ("not very elegant"). A constant that differs
per configuration converts to `constexpr i32 ROW_STEP = config::debug ? 4 : 2;`; a declaration that
exists only in some configurations is a `when`. kovarex: "it solves at least 2-3 problems we had, and
is simple."

### 20. `T^`: the library's UniquePtr<T> written short (decided 2026-11-04)
`T^` is sugar for the standard library's `UniquePtr<T>` (with its default deleter), and nothing
else: the compiler has no owner type of its own, so overloads, templates, deduction, traits, layout,
destruction and the C++ guarantees are UniquePtr's (docs/stdlib/uniqueptr.md). `Item^` and
`UniquePtr<Item>` are one type. Deleters and arrays keep their library names (`UniquePtr<T, D>`,
`UniqueArray<T>`). kovarex wants it for reading, `Vector<Item^>` over `Vector<UniquePtr<Item>>`, and
decided against a built-in after the lesson of Rust's `~T`: it was replaced by the library's
`Box<T>` because a type built into the compiler (its own deref, move and drop, no allocator
parameter, no methods or impls of its own) made library pointers second-class. That lesson is about
where the type lives, not the spelling; Swift's `[T]` and `T?` are sugar printed back as sugar.
kovarex: "lets learn from mistake of others, and make it syntactic sugar, while still being the
UniquePtr". The research is docs/unique-pointer-research.md (option B as sugar).
- **Grammar.** `^` is a declarator beside `*`, `&` and `&&`, with the same rules for `const`:
  `const Item^` owns a const Item, `Item^ const` is a const owner (`Item^ const&` for a parameter),
  `Item^*` a pointer to an owner. The parser writes it as `::UniquePtr<T>`. It is the declarator
  only where what follows cannot start an operand: not before `(`, a literal, a unary operator,
  `sizeof`, `new`, `this`, `true`, `false` or `nullptr`, so `a ^ (b)` and `x ^ 3` stay exclusive or.
  A name may follow (`Item^ item;`); where a type is only tried (a template argument, a cast's
  parentheses) `a ^ b` is then no type and is read again as an expression; as a statement, `a ^ b;`
  is a declaration, as `a * b;` is. There is no functional cast through `^` (`UniquePtr<A>(p)` keeps
  the long name, as C++ has no `T*(x)`). Imported C headers keep `^` as exclusive or.
- **Printing.** Errors and the language server's hover print `UniquePtr<T, DefaultDelete<T>>` as
  `T^` (TypeTable.cpp, toString), and so does an error's `(in ...)`, which names the function as the
  program writes it (`(in owned(Item^&))`, `(in sum<int32>(const Vector<int32>&), requested by
  main())`, `(in a lambda in main(), line 29)`; codegen's displayFunctionName, B, 2026-11-04), no
  longer by its IR name (`@"owned(UniquePtr<Item, DefaultDelete<Item>>&)"`). Qualified names (the
  IR's, debug information's) keep the class's own, so the IR does not change; the debugger's display
  names are F's (natvis, debug type names).
  The debug information names the class `T^` (its identifier stays the qualified name), so Visual
  Studio's Watch and Locals show `Item^` as the type (docs/debugging.md, "Values in Visual
  Studio"); its evaluator reads `^` in an expression as C++/CLI's handle, so a cast to `Item^*`
  does not evaluate there.

### 21. Placement new over const objects (decided 2026-11-04)
A placement new ends the object in its place and starts another there. kovarex's three rules ("Ok,
to all", 2026-11-04):
- **Over an object declared const: an error.** A const local, a parameter taken by value, a const
  global or static, a member or element of one, where the place visibly names it: `new (&limit)
  int(4)`, `new (&pair.b) int(7)`, `new (&table[1]) int(10)`, `new (table) ...` for a const array.
  "a placement new over 'limit', which is declared const: a const object is never replaced". A
  const object's value may be folded where it is read, and its storage may be read-only; C++ makes
  replacing one undefined ([basic.life]/10). A reference names an object declared elsewhere and
  is not visibly const; neither is anything reached through a pointer.
- **Over an object with const or reference members: allowed, replaced transparently** (C++20's
  [basic.life]/8 without its exceptions): the old name, an old pointer and an old reference reach the
  new object, its const members' and references' new values included. kpp has no `launder`; the code
  generator never assumes a const member or a reference member unchanged across a placement new
  (no invariant loads, no folding of a member's value from its initializer).
- **Through a pointer, const_cast's included: allowed, as in C++.** The containers of const
  elements construct into their storage through a `const T*` (section "kpp v0", pointer
  conversions), and `new (const_cast<T*>(view)) T(...)` over an object not declared const is
  C++'s rule. Over one declared const it is undefined; a Debug check that the address is not in
  read-only memory is allowed by the decision and not built (a thread that adds it says so here).

Specs: language/placement_new_over_const_object_is_error, language/placement_new_replaces_transparently.

### 22. A grammar without name lookup (decided 2026-11-04)
kpp parses every file before any name is known (section 2), so where C++ needs to know whether a
name is a type or a template, kpp decides by syntax alone and the resolver rejects what does not
fit, never reading it another way. This settles the open question "Syntax must be parseable without
name lookup" (kovarex, 2026-11-04).
- **`a * b;` is a declaration.** A statement of a name, `*` (or `&`, `&&`, `^`) and a name, then `;`,
  `=`, `{`, `(`, `,` or `[`, declares the second name, a pointer to (reference to, owner of) the
  first. With `a` a variable it is an error ("'a' is a variable, not a type"), never a product whose
  value is dropped; `a b;` the same (before this a variable's name used as a type silently meant
  its type, the reading that only sizeof's operand has).
- **`T(x);` is an expression**, a temporary T made from x (C++ declares x there). Parentheses in a
  declarator only group: `int (*twice)(int)`, `int (&row)[4]`; `int (x);` is the expression
  `int(x)`.
- **No chained comparisons without parentheses.** A relational operator with a relational operand,
  or an equality operator with an equality operand, is an error: `a < b > c` (C++'s
  `(a < b) > c`, a template-id to a reader), `a < b < c`, `a == b != c`. Parenthesised, or a
  relational inside an equality (`a < b == c < d`), it is as C++. kpp's own sources only: the
  converter's C++ input keeps C++'s reading.
- **`a < b >> c` needs parentheses** (kovarex, 2026-11-05: "Yes, rule 3"). A `<` whose list a `>>`
  closes reads two ways, C++'s comparison with a shift (`x < (y >> 2)`, with x a variable) and the
  template-id below (`x<y> > 2`, `>>` split into two `>`). Like a chained comparison it is an
  error naming both, never a guess: write `x < (y >> 2)` or `x<y> > 2`. Not when the arguments can
  only be types (`v<const T*>> 1`), and `>>` still closes nested template arguments
  (`Vector<Vector<int>>`, in a type or in an expression's template arguments, and a list opened
  inside another list's argument: `EnableIf<!isSame<T, A> && !isSame<T, B>>>`, the game's
  IDWithQualityFilter). Found as Atlas.kpp's
  `x < source->width >> (mip + 1)`, read as a template-id so that a mipmap loop never ran; the
  converter writes such C++ as `x < (source->width >> (mip + 1))` (C's 675a5395). Spec
  language/less_than_then_shift_needs_parentheses.
- **Template arguments by the token after `>`** (C#'s rule). In an expression, `name<...>` is a
  template-id when the token after its matching `>` is `(`, `::`, `{`, `)`, `,`, `>`, `;`, `]`, `.`,
  `->`, `...`, `>>`, `}`, `:` or the end, or a binary operator that cannot start an operand (`==`,
  `!=`, `&&`, `||`, `?`, `/`, `%`, `|`, `^`, `=`, `<`, `<=`, `<<`, `<=>`); so `f(a<b, c>(d))` is a
  call of the template a, and `f(a < b, c > d)` passes two comparisons. An argument that can only be
  a type (`v<const T*>`) makes it a template-id whatever follows; one with `&&` or `||` at its top
  level makes it a comparison. With the comparison chains an error, the guess can no longer pick a
  silent wrong reading: `a < b > c` is an error either way.
- **The most vexing parse, by scope** (kovarex accepted the coordinator's proposal, 2026-11-04). In a
  body `T x(...)` is always a variable, `T x();` too (value-initialized, where C++ declares a
  function); a body declares no function. At namespace and class scope `T f(...)` is always a
  function, and a variable there is written with `=` or `{}` (`Widget global{1, 2};`). Before, both
  were guessed from the parentheses' contents (a literal or an expression made a namespace-scope
  variable, empty or a builtin type a local function). kpp's own sources only: the converter's C++
  input keeps the guesses, and the converter writes the game's namespace- and class-scope objects
  `T x{args}` (`static constexpr CargoStationID EMPTY_CARGO_STATION_ID{CargoStationID(-1)};`). A
  typedef's declarator is a function type wherever it looks like one (`typedef int Local(int, int);`
  in a body). A function's parameter whose type names a variable is an error, so `Box box(four);`
  at namespace scope is "'four' is a variable, not a type", never an object. This replaces kpp v0's
  rule 2 below.
- **Parenthesised declarators** (2026-11-06). A statement's `T (*name)(...)`, with any run of `*`,
  `&`, `&&`, `C::*` and cv-qualifiers before the name, declares after any type; so a call of a call,
  `f(*p)(x);` and `f(**p)(x);`, reads as a declaration and is a compile error when f is no type
  (a grammar without lookup cannot tell them). Nested parentheses (`(*(*name))(...)`) and a
  following `[` or `=` declare only after a type no expression starts with, a keyword's, a qualified
  or a template name's: `f(*(p))(x);`, `row(*p)[0] = v;` are calls.

Specs: language/statement_type_star_name_is_declaration, statement_variable_star_name_is_error,
functional_cast_statement_is_expression, chained_comparison_is_error,
template_arguments_by_the_token_after, most_vexing_parse_by_scope,
namespace_scope_parentheses_declare_a_function, call_of_a_call_stays_a_call.

Default arguments add up over a function's declarations in one file, as in C++ ([dcl.fct.default] 4),
with a difference that order independence makes: each declaration has them all, so a call before the
declaration that adds one sees it (C++ rejects that call). A redeclaration of a template adds none,
as in C++. Declarations in two files do not add up: a call that needs both is a compile error. Spec
language/default_arguments_split_over_declarations.

### 23. Named return elision is guaranteed (decided 2026-11-04)
kovarex, 2026-11-04: "OK, guaranteed." C++ allows a compiler to build a returned local in the
caller's result (NRVO) and lets it copy or move instead; kpp always elides, so that what a return
costs, and whether it runs a constructor, does not depend on the optimizer or the build.
- **The rule.** When every `return` of a function returns the same local variable, that variable is
  built in the caller's result: no copy and no move, in debug and release alike, and its class need
  not be copyable or movable. "The same" is one declaration, not one name (two blocks' `T a; return
  a;` are two); the variable is automatic (not static, thread_local or extern), not volatile, not a
  reference, a structured binding, a catch parameter, a `when` declaration absent in the
  configuration or declared `alignas` (the caller's object has its class's alignment), and of the
  function's result class, cv ignored (`const T t; return t;` is elided; with `auto` the deduced
  type is compared, in each instance of a template or a generic lambda its own). The returns of a
  lambda or a local class in the body are their own functions'. A function with no return of it, or
  one that can fall off its end, has none. Every return naming the variable lies in its scope, so
  that scope covers them all.
- **Otherwise** `return name;` of a local, a by-value parameter or an rvalue reference (`M&& m`), the
  name possibly parenthesized, is C++'s implicit move ([class.copy.elision]/3): the name is an rvalue
  first, so the object is moved, never copied when it can be moved; a lambda's capture is the
  closure's member and is copied. `throw name;` moves likewise when no try block lies between the
  throw and the variable's scope. A function returning an rvalue reference given an lvalue of its
  type (`int&& f(int&& x) { return x; }`) is an error, as in C++20 (C++23's P2266 is not taken). A const local returned as another class binds the copy constructor through
  `const T&&`, as in C++ (WireAddedUndoAction::getWire's `const Wire` returned as `Optional<Wire>`).
  `--return-copy-warnings` (off by default) names each return that copies or moves, and why.
- **Lifetime.** The variable's object is the caller's: leaving its scope without returning (a loop's
  iteration) destroys it there, a throw after it is built destroys it in the result, and a return of
  it destroys every other local and leaves it built.
- **As built.** The rule is the resolver's (`GlobalScope::namedReturnVariable`, A: a function's, or
  a body's given its result type and the template arguments it is typed under, which codegen asks
  for each instance's typed copy and each lambda's operator); codegen builds the variable at the
  result's address (`sretAddr`; its debug variable there; a result that comes back in registers is a
  class copied as bits, with no constructor to skip) and skips its destructor on the return path
  alone (B); the interpreter elides too (D), so that a class that cannot be moved returns by name in
  a constant expression as well; kpp's inliner inlines no function whose result is returned in
  memory, so it never meets the variable (inlineCall refuses one, D). Specs
  language/named_return_elided, named_return_of_non_movable_type, named_return_fallback_moves,
  named_return_destroyed_in_slot_on_throw, named_return_loop_variable,
  named_return_typed_per_instance.
- **At compile time** (D) the interpreter's `return v;` of the variable is the result, as at run time:
  no copy or move constructor runs, v is destroyed once, with the result, and a template's variable
  is decided under the evaluation's arguments (language/consteval_named_return_variable_is_the_result).
  An address of v the evaluation took while it filled v (an element's through `operator[]`) names
  v's place in the callee, which ends at the return: a read through it, or a comparison of it, is
  then no constant, as C++'s constant evaluation rejects a pointer to an ended object, never an
  answer other than the program's, where that address is the result's.

### 23a. Safe navigation `?->` and the Elvis `?:` (built 2026-11-06; Q1 to Q4 decided and built 2026-11-07)
The readme's rule, with what it left open answered by kovarex on 2026-11-07: Q2 to Q4 as built, Q1 in a
new shape ("the rest should be optional").
- **`a?->b`** runs what follows only when a is set. Everything after a `?->` in the postfix chain belongs
  to it: members, calls with their arguments, subscripts (`a?->b->c`, `a?->f(g())`: g is not called when a
  is not set). Parentheses end the chain.
- **When a is not set** (Q1): a pointer result, raw or a class with `operator->` and a bool conversion
  ("unique ptr and such": UniquePtr, `T^`, SharedPtr), is null; a bool result is false; a void call does
  nothing; any other result makes the whole expression `Optional<T>`, so `i32 s = t?->engine->power;` is a
  compile error and `i32 s = t?->engine->power ?: -1;` works: `?:` on an Optional gives its value or the
  right side. Classes too: `t?->name` is `Optional<String>`; an Optional result stays one Optional (it is
  a class with operator-> and a bool conversion).
- **The result is always a value** (Q2), of the unqualified type, never an lvalue: `a?->x = 3;` is an
  error, and a function giving `T&` gives a T, copied (inside the Optional too). `void` is fine as a
  statement. A type that cannot be copied is an error at the `?->`.
- **Left of `?->`**: a pointer, or a class with `operator->` and a bool conversion (`T^`, Optional).
- **`a ?: b`** is `a ? a : b` with a evaluated once, typed as that conditional is, so an lvalue when both
  are lvalues of one type. `c ?::x : y` is C++'s conditional.
- **How it is built.** The parser writes them as ConditionalExpr: `(a) ? held : b` and `(a) ? held->b : default`,
  where the parenthesis (ParenExpr::holds) evaluates a once and HeldExpr is its value, so that whatever
  walks an expression walks them with no case of its own; a chain's `?->`s nest. The walker types a `?->`
  of a value as `Optional<T>` (Walker::safeNavigationOptional), so the code generator builds the Optional
  from the taken branch and the default is the empty one; `o ?: b` of an Optional o and a b of another type
  becomes `(o) ? *held : b`, Optional's operator*. `a?->x = 3` is an error whatever x is, an Optional's
  operator= too. Both are constants when
  their operands are. kpp's own sources only: the converter's C++ and C keep the nodes they had.

Specs: language/safe_navigation* (safe_navigation_gives_an_optional for Q1), language/elvis.

### 23b. Required `override` (an error since 2026-11-07, rules decided 2026-11-07)
Every function that overrides a virtual one says so. It landed in three steps: a warning under a flag,
counted on the game (13 functions, all cxxopts'), the converter writing the word (C, 8af8a46f), then the
error, at the in-class declaration: an out-of-line definition cannot say `override`. A class template is read per instance, each instance the program names, called or not,
as a virtual call reaches a member no walk does. Exempt: destructors (Q5, below). `final` alone says
`override` too (kovarex, 2026-11-07, Q6). The check is per instance (Q7): in a class template whose base
is its parameter, a mixin over polymorphic or plain bases, each instance overrides or not by its argument,
and the error is at the instance that overrides without saying so or says `override` of nothing, naming
it: "g says override and overrides no virtual function in Mixin<Bare>: remove `override`". An unmarked one
is "g overrides a virtual function: write `int g() const override`, not `int g() const`" (", in Over<Base>"
for an instance's). `virtual` beside `override` stays allowed (Q8). `--override-warnings` then listed only
the classes whose destructor would become virtual, and went when that rule was built (2026-11-09). Specs language/override_is_required,
classes/override_of_nothing_is_error, classes/override_of_nothing_in_an_instance_is_error.

**A polymorphic class's destructor is virtual** (kovarex, 2026-11-07, Q5; built 2026-11-09). A class with
a virtual function, its own or a base's, has a virtual destructor, written or not; `virtual ~X()` stays
allowed and says nothing more. "What is the point of a non-virtual destructor": `delete` through a base
of such a class is no longer undefined behaviour, and a destructor needs no `override`. A deliberate
difference from C++. Built (D, from A's queue):
- One test, Impl::hasVirtualDestructor: written so, a base's, or the class polymorphic and not marked
  [[foreign_vtable]] (its own or a base's). The checker reads it (`~X() override` over such a base overrides,
  an override's noexcept, the traits), the evaluator's `delete`, and codegen's vtable (collectVtables).
- The slot, where no base has one: a declared destructor takes it where it is declared, as one written
  `virtual` does, so the word changes no layout; an implicit one after the class's own virtual functions.
  The functions declared before it keep their slots, so a C table of them still matches
  (interop/c_object_with_vtable, whose Counter gained the slot after its two functions).
- Such a destructor is not trivial ([class.dtor] 8: no virtual one is): isTriviallyDestructible<Shape> is
  false where C++ answers true, as for `virtual ~Shape() = default`.
- An override that may throw of such a destructor, noexcept as its class's parts make it, is an error
  ([except.spec] 4), as for one written virtual (kovarex, 2026-11-09, of three options: the check for
  written virtual destructors alone, with an exception through such a base propagating or terminating,
  were the others): the base says `noexcept(false)`, or the override does not throw. In the game, three
  test fixtures holding a TestScenario (whose destructor may throw) under GameViewControl and
  LatencyInputActionQueueInterface; the converter's edits make them comply (C). Marking those two bases
  `noexcept(false)` spreads through multiple inheritance (NetworkInputListener has InputListener's
  written virtual destructor beside LatencyInputActionQueueInterface's).
- `--override-warnings`, which listed each class the rule would change ("X has a virtual function and an
  implicit destructor, which becomes virtual (its vtable gains the slot)"), went with it.
Specs classes/polymorphic_class_destructor_is_virtual (implicit, declared without `virtual`, protected, a
second base, an instance, UniquePtr, `p->~Base()`), classes/polymorphic_class_destructor_is_not_trivial,
classes/destructor_says_override_of_a_polymorphic_base, classes/destructor_override_of_nothing_is_error,
classes/polymorphic_class_with_a_deleted_destructor (its slot null, as a pure function's, a derived class's too),
classes/destructor_that_may_throw_under_a_polymorphic_base_is_error.

The count before it was built (2026-11-07, with --override-warnings): 942 classes of the game change (243 in src, 699 in tests), 890 with
an implicit destructor, 29 a protected one and 23 a public one; 48 gain the slot, every other one deriving
from them. Ours: none. One of the 48 is interop and would break: IUnknown, in the generated
src/Graphics/DX11/ComInterfaces.kpp, the root of 112 COM interfaces the system's objects are called through
(declared with no destructor, as COM objects are released): a slot in it moves every method of the 111
derived interfaces. The other 47 are the game's own (Freezable, Targetable, ThreadListener, ...).

**`[[foreign_vtable]]`** (kovarex, 2026-11-07: an explicit mark; an exemption of pure interfaces and the
COM types imported again were rejected): on a class it says an outside ABI fixes its vtable. It gets no
destructor there, and `delete` through a pointer to it is an error ("Device has [[foreign_vtable]] (from
Unknown): its vtable has no destructor, so delete cannot destroy it: release it the way its ABI says"), so
the rule's safety holds. The mark is inherited, so generate_com.py writes it once, on IUnknown, and its 112
interfaces have it (2026-11-09, with the rule), and so does a kpp class implementing one. Such a class may
declare a virtual destructor, its slot after the interface's functions, and then be deleted through itself:
the game's WindowsWriteStream (GDIPlus.cpp, an IStream for GDI+) declares one and its Release does `delete
this`. Built 2026-11-07: the attribute, the inherited lookup (Impl::foreignVtableClass), the delete error,
and --override-warnings leaving such classes out. Specs classes/foreign_vtable_keeps_its_layout,
classes/delete_through_a_foreign_vtable_is_error, interop/c_object_with_a_foreign_vtable_keeps_its_slots
(C's table of an interface deriving from another, called both ways, and an implementation deleting itself).

The rest of the count: the 13 functions are cxxopts' (abstract_value<T> : Value, a named base); no instance
of a class template whose base is its parameter overrides without saying so or says `override` of nothing,
in the game or in ours.

The game's count (factorio-in-kpp's src, libraries and tests at Factorio 4c178a5b915, tests.cmd's build
with --override-warnings --emit-llvm): 13 functions, all in libraries\cxxopts\cxxopts.kpp, and 490
destructors if Q5 does not exempt them.

An instance's errors come from the loop over the class instances the program names (the one that checks
their static_asserts), as no walk reaches a member only a virtual call reaches. They are reported outside
any walk, as those static_asserts are, so a compile server walks a file with one again from scratch
(BodyGraft's unitlessErrorIn) rather than keep it after its fix.

Specs: language/override_is_required, classes/override_of_nothing_is_error.

### 23c. Required `this.` and `This` (warning built 2026-11-07, rules decided 2026-11-07)
A member is named through the class, so that a new local or global never changes an existing line.
kovarex, 2026-11-07:
- **`this.x`** for an instance's data members and member functions (`this.f()`). Q13: `this` is a reference
  everywhere, never a reference in one place and a pointer in another, so the readme's "required `this->`"
  was a mistake for "required `this.`" (23d).
- **`This`**, a type keyword (Q9, after Rust's and Swift's `Self` and PHP's `self::`): the class lexically
  enclosing the code. In a class template it is the current instance (`Vector<T>`, the arguments not
  repeated); in a base class it is the base, never the class deriving from it; in an enum's methods the
  enum (`This::North`); it works in a static member function. Built 2026-11-07: the token stays an
  identifier, and the unqualified lookup binds `This` to the class around the code as its injected name
  would be (so `This::x` is a qualified member name: hover, go-to-definition and rename through it, F); in
  kpp code nothing may be named `This` ("`This` is a keyword: it names the class the code is in"), and
  outside every class it is "`This` names the class the code is in, and this code is in none". Imported C
  keeps it a name. Specs language/this_names_the_class, language/this_outside_a_class_is_error.
- **Statics** are `This::count` or `Train::count`. `this.count` of a static is an error (Rust's rule: it is no
  value of the object).
- **Nested types and enumerators** stay bare; `This::State` works too.
- **A bare name is a local, a parameter or a global, never a member** (Q10: "required this is the point of
  this, to make it very clear what is meant by saying just speed"). `void setX(i32 x) { this.x = x; }` is
  the form, with no warning for the parameter beside the member. A bare name that would find only a member
  is the error "write `this.speed`, not `speed`" ("write `This::count`, not `count`" for a static). A bare name with a member and a global
  of its spelling is the global, and no error (kovarex, 2026-11-07: "I would say no error"), neither for
  good nor during the conversion: its safety is --this-warnings reaching 0 on the game before required
  `this.` becomes an error, as those warnings list each such name, "(a bare x will be the one around the
  class)", the silent ones (21 in the game).
- **Where it does not apply** (Q11: "the : constructor doesn't need this as it always means this"): the
  member a member initializer names (`: speed(speed)` is the member from the parameter), `using`
  declarations, and the types in declarations. The initializer's arguments, default member initializers,
  default arguments and lambda bodies are code and need `this.` or `This::`.
  "The types in declarations" are those where `this` is no value: a parameter's type, a data member's type and a
  function's leading return type keep a bare name as the member (`char bytes[limit];`, `decltype(limit) f();`). A trailing
  return type is code, where `this` is in scope: a bare name there follows the rule, as in the body. A requires-expression
  follows the rule too, as the code around it does: the global, or the error, never a requirement silently unmet. Which
  names the rule takes is a mark the parser's last step sets (QualifiedName::requiresThis, markRequiredThis, 2026-11-09,
  D's review), because the evaluator meets a name in an array bound or a template argument before the walk has been there.
  lookupQualified applies it, for every reader of the name: the walk, the evaluator, a template argument and a constant the
  parser read as a type (`Box<limit>`, `sizeof(limit)`).
  Specs kpp/required_this_compile_time_reads_the_same_name, kpp/required_this_constant_read_as_a_type,
  kpp/required_this_types_in_declarations_keep_the_member, kpp/required_this_in_a_requires_expression,
  kpp/required_this_bare_member_in_a_requires_expression_is_error.
- **Whose code** (Q12): all kpp code, the game, the stdlib, the official libraries and the programs.
  Imported C and C++ headers are exempt, "so we can actually use C++ libraries, which we will have for a
  long time".

`This` is an identifier nowhere in the game or our kpp (comments only), so the keyword breaks nothing. The
converter writes `This::` for the statics named bare today, and cxxopts' `using Self = abstract_value<T>;`
can become `This`. (`This` itself is built, above; thread J's spec
language/this_names_the_class_in_nested_local_and_out_of_line_code covers nested and local classes, out-of-line
members, member templates, operators, hidden friends and a static's initializer, 2026-11-09.) In the class's own base
clause `This` names the class too (kovarex, 2026-11-09: CRTP, `class Widget : public Counter<This>`; a class
template's current instance, `Counter<Gadget<T>>`): the base clause is looked up outside the class, so the indexer
marks each `This` written there, under a decltype or an operand too, with the class, and the type lookup takes the mark
first: around a nested class the lookup finds the outer class, which until 2026-11-09 the walk, member lookup through the
bases and the evaluator took while codegen's layout took the nested class. `This::kind` is named through the type `This`
(the parser's QualifiedName::typeQualifier, as `decltype(e)::kind`), so it reads as the class's name would (built
2026-11-09, specs language/this_names_the_class_in_its_own_base_clause,
language/this_in_a_nested_class_base_clause_names_that_class, language/this_in_a_nested_class_base_clause_is_not_the_outer_class_is_error).
The converter writes `This` nowhere else (kovarex).

It lands as `override` does: a warning under a flag, counted, then the converter, then an error. The error is built
(thread J, 2026-11-09) under `--this-reference`, thread A's switch for `this` the kpp way (A's plan: `this` a reference beside
it, one option for both), in the program's own files (not the stdlib, isLibrary, until the option is the default, nor
imported C): a bare name whose lookup finds a data member, a member function or a static member of a class around the code
is looked up again outside every class around it (Walker::bareMemberRule); what that finds is the name, no error, and
nothing is the error with the warning's fix clause, "speed is a data member: write `this.speed`, not `speed`" (a static's
`This::count`). Specs kpp/required_this_bare_member_is_error, kpp/required_this_bare_name_is_never_a_member. The game
builds with the option once the converter has written the warnings' fixes (C) and `this.x` compiles (A's step 1).
`--this-warnings` is the first step and changes nothing else: each
unqualified name that finds a data member or a member function of a class is a warning at the name, "x is a
data member: write `this.x`, not `x`" ("a member function"), a static member's "a static member: write
`This::x`, not `x`", the editor's quick fix (F) reading the fix from that last clause. Not
warned: a name that finds a local or a parameter first (Q10), types, enumerators, nested classes, the name a
member initializer initializes (`: x(1)`, its argument is code), and a name inside a parameter's, a data
member's or a function's leading return type (Q11), where `this` is no value. A default member initializer, a
default argument, a trailing return type, a lambda in a member and an out-of-line member's body are code and
warned. Each name is listed once per line, whichever walks reached it (a class template's own and its
instances'). Until `this` is a reference and `This` exists the fixes it names do not compile: today `this->x`
and `C::x` are the spellings, and the warning counts the names either spelling needs.

**Dependent bases** (kovarex, 2026-11-07, decided and built): it follows from Q10. An unqualified name in a
class template never finds a member of a base that depends on its parameters (`template<class T> struct
D : B<T>`): it is what the scopes around the class have, the global, as in C++, which looks such a name up
where the template is written ([temp.dep] 3); `this.size` (`this->size` today) reaches the base's member,
and a base that does not depend (`B<int>`) is searched as any base. Until 2026-11-07 kpp took the primary
template's member there, ahead of the global (J's silent wrong answer); a base that is the parameter
itself (`: T`) was never searched. Specs language/unqualified_name_skips_a_dependent_base,
language/unqualified_name_only_in_a_dependent_base_is_error.

The counts (tests.cmd's whole-game build with --this-warnings --emit-llvm, places = a name on a line).
The game (factorio-in-kpp's src, libraries and tests at Factorio 4c178a5b915): 282 places for `this.` (134
data members and 148 member functions; 239 in tests, 37 in src, 6 in libraries) and 2,601 for `This::`
(2,011 in src, 392 in libraries, 198 in tests; `type` 202 times, getTypeStatic 65, addCommand 61): its code
style already writes `this->` for instance members and names statics bare. 21 places name a member that
has a global of its spelling (`eof`, `size`, `format`, `toString`, `operator==`, ...), which a missed
rewrite would turn into the global.

Ours (every library's tests, kppsite, kpplex, projectlib, each library's build.kpp): the stdlib has 69 places,
67 static members (59 `npos`, mostly default arguments) and 2 `pmf` in MemFnWrapper; projectlib has 81 data
members, 2 of them (`includes`) beside the stdlib's global includes; the libraries, kppsite, kpplex and the
build.kpp files none, as the code style already writes `this->`.

Dependent bases: no name in the game or in ours resolved through one, so the fix above changed nothing in
either: the count stays a safety check, and converted code relying on the old lookup gets `this.` anyway.

Spec: language/this_warnings_mode.

### 23d. `this` is a reference (decided 2026-11-07; built under --this-reference 2026-11-09)
kovarex answered Q13 to Q17 on 2026-11-07, as recommended but for Q13. `this` names the object, everywhere:
- **Member access is `this.x`** (Q13 a), the only form; `this->x` is the class's `operator->`, as on any
  object. Required `this.` (23c) is this rule's spelling. The game has 182,657 lines with `this->` for the
  converter to rewrite. Visual Studio's debug windows take `this.x` too (kovarex, 2026-11-09: "In debug
  window we need to write this. to make it consistent"; built in its extension, debugging.md, "this in
  the watch window"; VS Code's debugger loads no extension and stays C++'s).
- **`*this` is the class's `operator*`** (Q14), an error when it has none, saying "write this": `return *this;`
  is `return this;` (8,647 lines in the game), "one of the selling points of the change".
- **`&this` is the pointer** (Q15): `foo(&this)`, `return &this;`, `&this == other`, `delete &this` (5,411 lines
  pass `this`, 926 return it, 3,175 compare it, 163 `delete this`). `this == other` compares the objects through
  operator==, which is the one place a missed rewrite would compile silently (a class with an operator==
  taking a pointer): until the converter's count is 0, `this ==` and `this !=` in converted code are errors.
- **Lambdas** (kovarex, 2026-11-07, replacing Q16): `[&this]` captures the object by reference; a copy
  only as an explicit init-capture, `[copy = this]` (any name, used as `copy.x`; `this` itself is not
  captured then). `[this]` is the error "write `&this`, not `this`", as a silent copy would be a big
  gotcha and a C++ habit must fail loudly; `[*this]` is an error too. `[=]` never captures the object
  implicitly; `[&]` captures it by reference. The converter rewrites the game's 2,902 `[this]` to
  `[&this]` in this step.
- **`this` is never null** (Q17): `if (this)` and `this == nullptr` are errors, counted on the game first.

`this[7]`, `!this` and `this + 1` act on the object, as a reference's do. An enum's methods already have
`this` as the value. The order, as for `override`: warnings under a flag counted on the game, then C's
converter rewrite, then the errors.

`--this-reference-warnings` is the first step (2026-11-07) and changes nothing else: each use of `this` that
changes is a warning in the editor's fix form (23e): `this->` ("write `this.`, not `this->`"), `*this` ("write
`this`, not `*this`", saying so when the class has an operator* that `*this` will call), its address passed,
returned, stored, compared or deleted ("write `&this`, not `this`"), a `[this]` capture (the same), a `[=]`
lambda that uses the object ("write `=, &this`, not `=`"), and a null test, which has no fix to write. The
counts, lines of tests.cmd's whole-game build (factorio-in-kpp at Factorio 4c178a5b915) and of ours (every
library's tests, kppsite, kpplex, projectlib):

| | the game | ours |
|---|---|---|
| `this->` | 185,189 | 4,439 |
| `*this` | 3,655 | 230 |
| `*this` of a class with an operator* | 1,442 | 115 |
| the address (`&this`): passed, returned, stored | 8,180 | 51 |
| the address compared with another pointer | 519 | 35 |
| `delete this` | 8 | 0 |
| `[this]` | 2,191 | 7 |
| `[=]` using the object | 0 | 0 |
| a null test | 0 | 0 |

The address's two rows are one fix: a comparison through a class's operator== counts as passed. Spec
language/this_reference_warnings_mode.

`--this-reference` (the one option for `this` the kpp way, which required `this.` of 23c shares) has the second step (2026-11-09, thread A): the meaning itself, in the program's own files
(ParsedFile::thisReference). The standard library keeps C++'s pointer until the meaning becomes the default,
when it is converted with the switch, and so does what kpp generates (a test build's runner, `<tests>.kpp`);
an enum's methods keep `this` as the value. --this-reference-warnings says nothing of a `this.x` already
written the new way (required `this.`'s fix), whose `&this` fix would be wrong. All 11 libraries of libraries/
pass their tests with the option once both warning modes' fixes are applied (1,377 in 24 files, 2026-11-09).
That is step 3 of the plan, on master for libraries/: each library's build.kpp sets `thisReference` (docs/build.md),
so a library's files take the meaning whoever uses it (`--this-reference-library NAME`), and its own tests with it. The old and new
meanings collide (`return this;`, `f(this)`, `auto c = this;` and `this == other` each mean the pointer in
one and the object in the other), so a file takes one meaning whole, never both. The parser writes `this`
as an implicit `*this` around C++'s pointer (ThisExpr::form 1), so the walk, constant evaluation and codegen
see what `*this` gives them, and `&this` as the pointer itself (form 2), never a class's unary operator&.
`[&this]` is C++'s `[this]` capture. These are errors with their fix: `this->x` of a class with no
operator-> ("write `this.`, not `this->`"), `*this` of a class with no operator* ("write `this`, not
`*this`"), `[this]` ("write `&this`, not `this`"), and a `[=]` lambda that uses the object ("write `=,
&this`, not `=`"), and C++'s uses of `this` as the pointer ("write `&this`, not `this`"; 2026-11-09):
returned, stored or assigned as a pointer, passed to a pointer parameter or through C's `...` (which would
take the object itself, silently), cast to a pointer type, compared with a pointer where no operator of the
class takes it, deleted, and the place of a placement new. One converted to a template's own parameter
type is left to codegen's error. The null tests have no fix: `if (this)`, `this == nullptr` and `this &&`. `!this` acts on
the object, as said above. The plan (A-plan-this-reference in the coordinator's notes): C's converter
writes the new forms and the game builds with the option, then ours, then the default with the stdlib
converted. Specs language/this_is_a_reference, this_is_a_reference_through_bases_and_constants,
this_reference_old_forms_is_error, this_reference_address_uses_is_error,
this_reference_capture_of_this_is_error and this_reference_keeps_the_pointer_in_the_standard_library.

The converter's step (C, 2026-11-09; convert.cmd's `this` pass, last before the headline's builds): first required
`this.`'s fixes, from `--this-warnings` of each configuration and platform, applied once (apply-fixes.py --this) while
a bare name still finds the member, as under `--this-reference` one with a global of its spelling would be the global
silently; then `kpp --convert-this-reference DIR...` by each file's tokens: `this->x` as `this.x`, `this->*p` as
`this.*p`, `*this` as `this`, `(*this).x` as `this.x`, `*this.*p` as `this.*p`, a `*this` used as a condition as
`bool(this)` (`if (this)` is the error "never null"), and every other `this` as `&this` (`[this]` as `[&this]`). The
game builds with the option, build.kpp's and the scripts'. In the game (from C:\Factorio 4c178a5b915, the checkout's
files): 218,766 `this.`, 4,857 `this`, 10,896 `&this`; bare members 331 `this.x`, 4,585 `This::x` and 15 by the name of
the class around (a static of an enclosing class: a nested class's `This` is itself). The game's IR, Windows debug,
before and after: the same but for names and numbers that carry a token or a column (a local's or a local class's,
a specialization's, an assert's column, `source_location`'s), the assert texts themselves, one constant read as a
constant (`This::EXPRESSION_OFFSET`, loaded as a global before), and two adjacent startup initializers of two
files in the other order (Frame's and TabbedPane's default styles, which neither reads; across files the order is
unspecified). tests\convert_this checks each form against C++'s output.

### 23e. A fix in a message (F, 2026-11-07)
An error or warning whose fix is mechanical ends, before any " (design.md N)", with exactly one of two
clauses, which the editor's quick fix reads: "write `NEW`, not `OLD`" ("write `this.width`, not `width`",
"write `Direction::North`, not `North`", "write `int g() const override`, not `int g() const`") or "remove `OLD`".
OLD is the source's text as written, starting at the message's line and column: the server replaces it only
when that exact text starts there. Notes go before the clause. So far: --this-warnings, required `override`,
a bare enumerator of a scoped enum, `implicit` written twice or beside `explicit`, and explicit by default's
plain `explicit`, `explicit(cond)` and braced argument ("Explicit by default").

### 24. The compiler's own sources: laid out as Factorio's (decided 2026-11-05)
kovarex, 2026-11-05: the compiler's C++ follows Factorio's layout, "one class per file, classes with
capital letters, like LinkHelper.cpp", and refactorings that are clear wins are pushed without asking.
- **Layout.** One directory per component under compiler/src (Resolver, Codegen, Converter, Driver,
  Lsp), PascalCase file names, one class per file where there is a class (`Walker.hpp`,
  `OverloadResolution.cpp`). A class too large for one file is split by topic into files named after
  it (`WalkerCalls.cpp`, `WalkerOperators.cpp`).
- **Inside a class** (kovarex, 2026-11-05, for all of kpp's sources): member functions, nested
  types and statics first, data members last (moved without changing their order, so that the
  layout and every aggregate initialization stay as they were); a defaulted `operator==` where the
  comparison is memberwise; no constructor written that the language gives; private data where the
  class keeps an invariant.
- **How it gets there.** Each split is a pure move first, pushed alone and checked byte-identical
  (object code or the game's IR), then made into real headers and classes one push at a time, so
  that other threads' rebases stay short. The files' line endings stay as they were (resolver LF,
  codegen CRLF).
- **Decisions are made once.** What the checker decides (an operator's or constructor's callee, a
  conversion, the named return variable, a `when` member) is recorded on the typed tree, per template
  instance, and codegen and the interpreter read it instead of deciding again. The refactoring list
  and its evidence: the 2026-11-05 survey (items 1 to 14, tests T1 to T7).

### 25. move, forward and asConst are casts (decided 2026-11-09, built the same day)
kovarex approved thread D's design on 2026-11-09, with asConst and addressof besides move and forward.
clang has worked this way since version 15 for std::move, std::forward, std::as_const and std::addressof.
- **The decision.** A call of the library's `move`, `forward` or `asConst` is typed as the cast it is:
  - `move(x)` is an xvalue of x's type;
  - `forward<T>(x)` is T&& with the references collapsed, an lvalue for T = U&;
  - `asConst(x)` is a const lvalue of x.

  The type checker makes no instance for such a call: no record, no copy of the body, no walk. Code
  generation emits the operand's address (`GlobalScope::callIntrinsicOf`).
- **Keyed on declarations, never on a name.** The intrinsics are the library's own top-level function
  templates of those names, of the shape stdlib/core/Utility.kpp and UtilityExt.kpp declare: one type
  parameter, one reference parameter, a reference returned.
  - Lookup and overload resolution run as for any call. So a namespace's own `move` is its function,
    and ADL's non-template `move` beats the library's template.
  - The evaluator's value categories read the callee's declaration as well. Only in a template's own
    nodes, where the walk leaves a dependent call's callee unrecorded, do they fall back to the name.
- **What stays a function.**
  - The declarations stay in the library, for go to definition and documentation.
  - A pointer to one is a function's, though codegen does not take a template's address yet.
  - A call codegen does not inline (`--no-inline`) is a call: codegen instantiates the body then
    (`GlobalScope::instanceBody`).
  - The compile-time evaluator still runs a body through its instance. That leaves 42 instances in the
    game, a follow-up for the evaluator.
- **noexcept.** It follows the declarations, which are noexcept since thread C's 11009967, as std's are:
  `noexcept(move(x))` is true.
- **Measured.** One debug-info build of the game at -j 32, master 7bcca232 against the change:
  - instance records of move 8,478 -> 4, of forward 4,822 -> 38, of asConst 1 -> 0;
  - function template instantiations 65,200 -> 52,077;
  - instance requests 1,445,386 -> 1,361,953.

  The game's IR is byte-identical apart from its one asConst call, now the identity cast instead of an
  inlined trivial body.
- **`move` of a const object is a warning** (kovarex, 2026-11-09): its result is a const rvalue, which the
  copy constructor takes, so the object is copied. It keeps C++'s meaning and says so, except in a template's
  instance, whose argument may be what made the object const. Spec kpp/move_of_a_const_object_is_a_warning.
- **Not built.** addressof: the library declares none, and the game uses none.

## Code packages (proposed 2026-10-27, decided 2026-11-04)

**Decided** (kovarex, 2026-11-04): a directory is a package, named after the directory; an optional
README.md documents it; package names never appear in code. Changed from the proposal: no
`package.kpp` and no display name other than the directory's, and the description is the README's
first paragraph. Rollout: first `kpp --packages` and the language server's package tree and symbol
search with the package path; `packageof(T)` and the standard library's split into directories
after kovarex has seen the tree on the game.

**Goal.** A big codebase needs a structure above the class, so that a reader can find
`Vector` by walking *Standard Library > Containers > Vector*, or find the inserter code in
Factorio by walking *Factorio > Entity > Inserter*, without knowing the name in advance and
without understanding the rest of the program first. The IDE is the main consumer: the tree is
how a newcomer explores the code and how anyone finds a class they do not remember by heart.

**The rule that makes it different: a package is not a namespace.** Package names never appear
in code. Every type and function is referred to by its plain name from anywhere (`Vector<int>`,
never `std::Container::Vector<int>` and never `Container.Vector<int>`), and there is no
`using namespace`, `import` or `use` that changes what a name means in a file. A snippet copied
from one file to another, from a test into the program, or from documentation into a chat means
the same thing everywhere, which is section 2's "every project symbol is visible everywhere"
extended to the library.

### Declaring the hierarchy
- **A directory is a package.** The source roots given to the compiler (the program's
  directory, `stdlib/`, each library) are the top-level packages; every subdirectory below them
  is a child package. A file belongs to the package of the directory it is in. There is no
  per-file `package` statement: the directory is the single source of truth, so the tree cannot
  drift from the files, and moving a class to another package is moving its file.
- **The package's name is its directory's name**, always: there is no other display name.
- **An optional `README.md` in the directory documents the package** (kovarex, 2026-11-04: an md
  file, "so there can be more thorough document about the workings of the package"). Its first
  paragraph is the short description tools show (the tree's hover, `kpp --packages`); the rest
  is free documentation. Pictures and diagrams sit in the same directory, next to the README, and
  are linked by their bare file name (`![belts](belts.png)`); Mermaid blocks are fine for text
  diagrams. The compiler reads only `.kpp` files, so none of these affect compilation.
  ```
  stdlib/containers/README.md
  # containers

  Owning sequences and maps: Vector, HashMap, HashSet, UniqueArray.

  ![how a HashMap's slots are laid out](hashmap.svg)
  ```
- Siblings are listed alphabetically.
- C++ `namespace` stays what it is (an ordinary qualifying scope), for the code that wants
  qualification on purpose: Factorio's `agui::`, `ActionData::` and the `config::` constants.
  Packages and namespaces are independent: a namespace may span packages and a package may hold
  several namespaces.

### One global scope, so clashes are errors
- All top-level names of all packages, the standard library and C imports included, are in one
  scope. Two declarations of the same name in different packages are a compile error that names
  both packages and both locations:
  `error: 'Pair' is declared in Program > Tests (tests/pair.kpp:3) and in Standard Library >
  Utility (stdlib/core/Utility.kpp:12); top-level names are global, rename one of them`.
  Overloads of a function are not a clash (they are one name, as in C++), and neither are
  `static` functions or names in an unnamed namespace, which are local to their file.
- Checked between the program's own files too: two files declaring one name, or a variable and a
  function of one name, are the same error, at the later file (they were silently one entity: two
  files' `int total` were one variable). Two declarations that are one entity are no clash: one
  type named twice (`typedef struct Cell {...} Cell;`, an alias written in two files), one constant
  written alike in two files with no other name in it (`const auto M_1_2_PI = 0.159...;`, a macro of
  the game's and one of Lua's), a variable template's partial specialisation.
- Names local to their file are not implemented yet: the resolver and the code generator give
  every top-level name one entity, so a `static` variable or a name in an unnamed namespace that
  another declaration also claims (another file's, or the standard library's `empty`) is an error,
  "a name local to its file ... is not supported yet", until they are. (`static` functions of one
  name are one overload set; two of them with the same parameters are "defined twice".) The
  converted game has none. A build with a clash generates no code: lookup takes the name as one
  of the two, and what codegen made of that was only the clash's echo.
- This settles the open question "Library names and program names share one global scope" with
  the "distinct names" option, made into a rule the compiler checks instead of a convention. It
  also replaces the `kpp::` namespace floated in the Speed-test prototype thread: that one needs
  an implicit or explicit `using namespace kpp;` to keep `Vector` unqualified, which is exactly
  the context dependency this design avoids. The standard library becomes the top-level package
  *Standard Library*, not a namespace.
- The cost: the standard library takes its names from every program, so it should claim few and
  specific ones (`Pair` is a fair claim; `Node`, `Item` or `Data` are not), and two third-party
  libraries that both define `Logger` cannot be used together until one is renamed (or its
  author puts it in a namespace). That is the price of context-free names, and for a
  whole-program compiler with its own library it is small: Factorio's 3,872 headers contain only
  one file name twice (`MigrationHelpers.hpp`), so a codebase of that size already lives in one
  flat scope by habit.
- A later option if helpers clash too often: `internal` declarations, visible only inside their
  package and its children and exempt from the global clash check. Not proposed now.

### Tools and the IDE
- **The package tree view** is the persistent compiler's model shown as a tree: package, then
  its files, then the types and free functions each declares, with the package description on
  hover. This is how one explores an unknown codebase: open *Factorio*, see *Entity*, *Train*,
  *Gui*, *Logistics*..., open *Train*, see the classes.
- **Go to symbol** searches the one global scope, so a name is always one hit; the result shows
  its package path (`Inserter: Factorio > Entity`), and typing a package path narrows the search
  (`entity ins` finds `Inserter`). Hover and breadcrumbs show the path of the symbol under the
  cursor.
- **Moving between packages** is a file move; since names are global, no reference anywhere
  changes. A rename of a class is the only refactor that touches its users.
- `kpp --packages` prints the tree with the declarations in each package (for the command line
  and for scripts), and diagnostics name the package along with the file.
  - Done 2026-11-04 (step 1, the language server's tree and search to follow): the build's front
    end, then one line per package, its path from its root (the standard library's directory, a
    library's name, the program's directory), a `: ` and its README's first paragraph when it has
    one; under it each file with the names it declares at its top level, by kind:
    ```
    shop/orders: Orders and their lines.
      Order.kpp: types Order, OrderState, OrderId; functions orderCount; variables maxOrders
    ```
    An out-of-line member's definition is its class's, and a name declared twice (overloads) is
    listed once. A directory with no files of its own but packages under it is listed too.
    compiler/tests/packages.ps1 checks it (run_tests).
- Reflection (step 2): `packageof(T)` gives a constant descriptor (`name`, `description`,
  `parent`), so for example the test runner can run all tests of one package (`--filter
  "Factorio > Train"`).
- Cost to the compiler: the package is a field of the file, found once when the file list is
  built. Name lookup does not look at it, so it changes nothing in the front-end numbers.

### Packages for the converted Factorio and the standard library
- The converter keeps the source directory layout, so Factorio's own directories become its
  packages with no extra input: `src/Entity/Inserter.cpp` goes to *Factorio > Entity*, the 66
  directories under `src/` (AI, Belt, Blueprint, CircuitNetwork, Entity, Gui, Train...) are the
  second level, and the 340 files directly in `src/` are in *Factorio* itself until someone
  sorts them. CommonUtil and Agui are top-level packages of their own. The converter writes a
  README.md only when it has a description to give. Factorio's namespaces (`agui`,
  `ActionData`, `MTL`...) stay namespaces, because the code refers to them qualified.
- `stdlib/core` (23 files) would split into directories (step 2): *Containers* (Vector, HashMap,
  HashSet, UniqueArray, InitializerList), *Text* (String, StringView, WString, Format),
  *Memory* (Memory, UniquePtr, SharedPtr), *Functional* (Function, Algorithm, Hash, Utility,
  Optional), *Math*, *Threads* (Thread, Time) and *Language* (TypeInfo, Location). The move
  changes no code, since no name changes.

### Compared with C++ namespaces and Java packages

| | C++ nested namespaces | Java packages | kpp code packages |
|---|---|---|---|
| Part of a name | yes, `std::chrono::seconds` | yes, `java.util.List` | no, only `Vector` |
| Needs context to use a short name | `using namespace` / `using` | `import` per file | never |
| Same name in two places | allowed, disambiguated by qualification | allowed, disambiguated by import | compile error |
| Declared by | `namespace x { }` blocks, anywhere | `package a.b;` in each file, must match the directory | the directory, plus an optional README.md |
| What it is for | avoiding clashes, ADL | avoiding clashes, access control (package-private) | browsing and navigation only |
| A snippet means the same in any file | no (depends on usings and the enclosing namespace) | no (depends on imports) | yes |

C++ namespaces and Java packages solve name clashes by making names longer and then let each
file shorten them again, so the meaning of a short name depends on the file it is in. kpp
packages keep the hierarchy those systems give for browsing and drop the part that makes names
contextual; clashes are prevented by the compiler refusing them instead.

## struct and class (decided 2026-11-01: option 3, with friend)

**Decided** (kovarex, 2026-11-01): option 3, two keywords with a checked meaning ("I like 3, so we
keep struct and class, and just means more than before"), and `friend` stays ("for now lets keep
friend"). A struct deriving from a polymorphic class is no value type: it must be a class. Order:
the struct rule first (the converter turns the 31 game and test structs into classes), then access
checks as warnings over the converted game, errors once it is clean; the converter keeps the
game's 438 friend declarations. The analysis follows.

**As built (thread A, 2026-11-02): the struct rule, the default since the converter writes the game's
breaking structs as classes (C's 3e46a07c; `kpp --struct-rule` is accepted and changes nothing).** The parser records a `private:` or `protected:`
section in a struct ("a struct's members are all public"), a private or protected base ("a struct's
bases are all public") and a virtual function, pure function, override or virtual destructor ("a
struct has no virtual functions"); the resolver adds a struct deriving from a polymorphic class
directly or through a base ("struct Label derives from Widget, a polymorphic class"), each "(a struct
is a public value type, design.md "struct and class"): make it a class". They are errors
(`--access-warnings` warns of them too); a conversion's resolve of the C++ does not check them. Imported C structs,
unions and typed unions are exempt. Constructors, a destructor, methods, operators, static members,
nested types, a `public:` label, friend declarations and public bases that are not polymorphic stay
allowed, in templates and local structs too. The converted game breaks it 37 times (31 in src, 6 in
its tests: 26 + 5 sections, 1 + 1 virtual functions, 4 polymorphic bases); the standard library's
ExceptionHolderBase, ExceptionHolder, OutStream and OutFileStream are classes now. With the rule the
default, the 40 spec snippets and 8 of run_tests' programs that had such structs write them as classes
(`class X { public: ...`, their bases public); the rule's question (does a base declare a virtual
function) reads the member lists unrecorded, so that a layout edit of a class stays incremental. Specs classes/struct_private_member_is_error,
struct_virtual_function_is_error, struct_base_not_public_is_error,
struct_deriving_from_polymorphic_class_is_error and struct_is_a_public_value_type. Friend
declarations of classes and functions work (classes/friends, friend_declarations).

**As converted (thread C, 2026-11-02).** The converter writes the C++'s rule-breaking structs as
classes: `class`, then `public:` first so that the members keep their access, and `public` before
each base written without an access (a class's bases start private). The breaks are A's (a private
or protected member, a base that is not public, a virtual function, a polymorphic base, followed
across files), local structs in function bodies included. The game and its tests: 37 (31 with a
private or protected member, 4 with a polymorphic base, 2 with a virtual function), 9 bases written
public; `kpp --struct-rule` passes over the converted game and tests. The converter keeps the game's
409 friend declarations (it removed them all before) and its 32 hidden friends as written (it made
21 of them member operators and moved 11 to namespace scope before; A's b4239284 finds them through a
nested enum and makes them for a class nested in a class template: NotificationData's `Flags
operator|`, RectangleUnionLogic<P>::IntervalPoint's operator<). The access warnings over the converted
game went from 2,071 to 2, both LuaBinder naming LuaDebugAdapter's private luaIndex in a requires (a
silent difference from C++, spec classes/requires_expression_checks_access, D's).

**Hidden friends (thread A, 2026-11-02), as C++ has them.** A friend function defined in its class
is a function of the class's namespace that no ordinary lookup finds: only argument-dependent lookup,
through an argument of the class, of a class deriving from it, of an instance with it as a
template argument, of an enumeration or a class nested in it (C++'s associated classes: NotificationData's
`Flags operator|` for its nested `enum class Flags`). It takes no object (the compiler treats it as a static member: its body names the
class's private and static members and member types). Names and operators alike, unary ones
included; the candidates join the namespaces' functions of the name in one overload set. A class
template's friend is its instance's, one function per instance, and so is the friend of a class nested
in one (`RectangleUnionLogic<P>::IntervalPoint`'s operator<); a call whose arguments are of two
instances of one template, both with friends of the name, is an error ("not supported in one
call"), not one instance's friend chosen. A defaulted friend comparison (`friend bool
operator==(const C&, const C&) = default;`, `<=>` too) is the defaulted member comparison. Specs
classes/hidden_friend_functions, hidden_friend_lookup, hidden_friend_without_class_argument_is_error,
hidden_friends_of_two_instances_is_error, defaulted_friend_comparison, hidden_friend_of_nested_enum,
hidden_friend_in_nested_class_of_template; expressions/free_unary_operator
(a free unary operator was not emitted before, namespace ones included).

**The question** (kovarex): why are the standard library's types structs (`struct String`), and
what do two keywords buy when the only difference is that a struct starts public? He uses
`struct` as a hint to the reader that a type is small, mostly data, and `class` for a real object.

**Today** the two differ only as in C++: a class's members and bases start private, a struct's
public. kpp records access but checks it nowhere (its one use is class argument deduction's
aggregate test), so today the keywords mean the same thing. The library is written with
`struct` because all its members were public anyway and `public:` would have been noise. Nobody
decided that.

**What the code does** (`python compiler/bench/struct_survey.py`, a regex survey, right in
proportion):

| | struct | class |
|---|---|---|
| The game's C++ (src, CommonUtil, Agui) | 982, median 3 members: 70% plain data (no constructor, access section or virtual), 29% with a constructor, 3% with a private or protected section, 1% with virtual functions | 5,115, median 9 members: 45% with virtual functions, 50% with public members only |
| The standard library | 231 (String, Vector, Map, HashMap, Optional, UniquePtr, FunctionImpl...): none with a private section | none |

kovarex's convention holds in the game. 96% of its structs are public data, at most with
constructors and methods. The 35 exceptions (`struct_survey.py --offenders`) are 26 with a
private or protected section (VulkanImpl, Filesystem's Path, InputState's KeyState...) and 9 with
virtual functions (DebugTools' value structs, AsyncScenarioSaver's Observer). The library breaks
the convention: its containers are objects with invariants, written as public structs.

**Options.**
1. *C++'s rule, as now.* Costs nothing. Without access checks it is no rule at all, and with them
   the keyword still says nothing about what a type is.
2. *One keyword.* Saves one choice per type. It loses the hint kovarex reads, rewrites 1,000 (or
   5,000) of the game's definitions, and gains nothing at compile time.
3. *Two keywords with a checked meaning (recommended).* `struct` is a public value type: every
   member and base is public, and a `private:` or `protected:` section or a virtual function is
   an error ("a struct has only public members and no virtual functions: make X a class").
   Constructors, a destructor, methods and operators are fine. A struct without constructors is
   initialised member by member from a brace list, an aggregate as now. `class` is everything
   else, its members private until a `public:`. For `class` to mean something, access is checked
   too, private and protected with `friend` as in C++. That is separate work in the resolver:
   member lookup must know whose code it types and the friends, 442 `friend` declarations in the
   game.

**What option 3 costs.**
- The converter turns the game's 35 rule-breaking structs into classes, mechanically, with
  `public:` first.
- The library's types with invariants become classes with their state private, about 40 of them
  (String, Vector, Map, HashMap, Set, Deque, Optional, UniquePtr, Function and the like). Traits,
  tags, Pair, Tuple and the iterators stay structs.
- Declarations imported from C headers are structs of public data already.
- Access checks run as warnings over the converted game first, to find what the conversion must
  keep (tests reaching into the classes they test, for instance), and become errors once it is
  clean.
- The struct rule can come first: it is one check in the parser, and the game breaks it in 35
  places.

**Measured with the checker (2026-10-30).** `kpp --access-warnings` runs option 3's rules as
warnings only (off by default, no other change: the game's IR is the same byte for byte; on, it
adds 0.06 s to the 1.4 s resolve of the converted game and its tests). It reports every use of a
private or protected member outside its class and the classes around the use, the classes derived
from it (protected) and its friends, and every struct with a private or protected member, a base
that is not public or a virtual function. Constructors were not checked then: the checker does not
choose them, codegen does (step 2 below). Run over the converted game with its tests (scratch conversion of
165b365) and, during a conversion, over the C++ it reads, whose friends are still declared:
- *Structs.* The game has 1,016 structs and 5,220 classes; 26 structs would become classes, all
  for a private or protected member (VulkanImpl, Filesystem's Path, InputState's KeyState,
  ControllerButtonState and ControllerAxisState, the Backup of UndoItemStack and UndoRedoStack,
  RailPlanFinder's SearchData and SupportTracker, ValueToWidget, the GUI helpers...). None has a
  base that is not public or a virtual function of its own; 3 are polymorphic through a base only,
  which the rule as written lets through (a decision: a struct deriving from a polymorphic class
  is no value type either). The tests add 57 structs and 5 to change (their 17,131 classes are
  UnitTest++'s, one per test). The standard library's 231 structs (no class) have no private
  section; 4 would become classes for a virtual function: ExceptionHolderBase, ExceptionHolder,
  OutStream and OutFileStream.
- *Friends.* In the C++, 1,501 uses in 177 classes are legal only through a friend declaration:
  250 of the 438 are used, as 259 relationships, all but 2 uses through a friend class. They are
  implementation partners: EnergyStorage's members named by ElectricSubNetwork 97 times,
  LogisticCell's by LogisticNetwork 53, TargeterBase's by Targeters 49, EditorController's by
  GunLogic 47, LinkedContainer's by LinkedInventory 39, IntrusiveList's Hook by its List 32,
  MinHeap's items by the heap 24. The converter removes every friend declaration today ("kpp has
  no friends"), so the converted program has 1,808 uses that `class` access checks would reject:
  - 1,802 that a removed friend declaration allows: 1,765 through friend classes and 7 friend
    functions in the game, 23 in the tests, which reach the privates of what they test only
    through `friend class` declarations, and 7 the converter's `Map` -> `GameMap` rename hides
    from the comparison (HeuristicCrcGenerator, StopLock);
  - 2 converter artifacts: Optional's hidden friend operators, moved out to namespace scope;
  - 4 gaps of the checker (a protected member through a friend of the class it is named in).
  Over the C++ itself, which compiles, the checker flags 93 uses: 86 are one call the C++ input
  resolves to the wrong overload (Style::propertyStatus given a member pointer; the converted code
  resolves it right), 7 protected corner cases.
- *What option 3 would make the converter rewrite:* the 31 structs (game and tests) as classes
  with `public:` first; the library's 4 structs with virtual functions; and the friends. With
  `friend` in kpp (friend classes; friend functions are used twice) the converter keeps the 438
  declarations as written and the program checks clean but for the 2 operators, which become
  members. Without it, each of the 259 relationships becomes something else (public members, an
  accessor, the partner nested in the class), in 177 classes, by hand.

**Access warnings, step 1 (thread A, 2026-11-02).** Run again over the conversion of C:\Factorio
4c178a5b915 (factorio-in-kpp e4cfd9ba, `kpp factorio-test --check --access-warnings`): 2,071 uses
(2,048 game, 23 tests; 1,866 private, 205 protected) in a program with 0 friend declarations,
since the converter still removes all of them (409). Over the C++ it reads, friends declared: 1,551
uses that 260 of its 441 friend declarations allow, and 94 warnings, all quirks of reading the C++
(none in the converted code): 86 the Style::propertyStatus overload above, 3 computeConstant named
as an overload set, 1 a UnitTest++ macro's function name, Direction::rotate and
PaintEvent::graphicsContext. Two checker gaps fixed: a protected member named through a class
whose operator-> gives it (`handle->forceID` with a Targeter<Character>, in a friend of Character),
and a class template deriving from its parameter naming the base's protected members in an
instance (DragPaneWidget, agui::Layout's); specs classes/protected_member_through_friend_of_naming_class,
protected_member_in_instance_with_parameter_base. Next: the converter keeps the friend declarations
(and, with hidden friends in kpp, Optional's operators as written), then the count over the
converted code is what is left to decide.

**Access warnings, step 2: constructors (thread A, 2026-11-02).** The checker does not choose
constructors, codegen does, so codegen checks each one it calls (`callFunction`, through
GlobalScope::checkConstructorAccess) in the code it emits it for: direct initialization, default
construction, `new`, a base or member initializer. A class's implicit construction emitted where its
object is made calls its bases' and members' constructors as the class (a protected base constructor
is the derived class's to call). A copy or move through a user copy constructor is not checked (codegen
emits those apart). Over the same conversion: 2,169 uses (98 constructors more: EditorAction's private
constructors in EditorActionTestCreator 81 times, HeatBuffer's, UnitGroup's in Commander, EnumItems'
iterator's...), each one a friend of the C++ the converter removes; none less. Spec
classes/constructor_access_warnings.

**Access checks, step 3: errors (thread A, 2026-11-02, as decided: errors once the converted game is
clean).** With the converter keeping the game's 409 friend declarations and its hidden friends as
written (C), `--access-warnings` over the game, its tests and its C libraries reports 0 uses (2,161
that 319 of the friend declarations allow). So a use of a private or protected member its code may
not name is an error by default: in a function's or variable's walk a checker error of that walk (a
compile server's walk again takes it back when it is fixed), a constructor's from codegen as its error
in the function emitting the call (its emission again takes it back). `--access-warnings` still adds
the notes of the uses friends allow and the counts. A conversion's resolve of the C++ does not check
(its friends, and the quirks of reading C++ above). Specs classes/access_violation_is_error and
constructor_access_warnings (now errors); the access test of run_tests prints them as errors.

## Instantiations own their typed bodies (decided 2026-10-28: a copy per instantiation)

**Today.** A template's body exists once, in the parsed tree. Whatever types it for a set of
arguments (the checker's instantiation checks in the front end, then codegen through
GlobalScope::retypeFor before it emits each instantiation) writes the result into that one tree:
Expr::ty, NameExpr/MemberExpr::resolved, CallExpr::templateArgs, IfStmt::constexprKnown, the deduced
type of a local `auto` in its cachedType. The next instantiation overwrites it. So two threads
cannot type or emit instantiations of one template at the same time (they read each other's
types), the parallel front end already races on these writes (the headline moves by a few lines
between identical runs; `-j 1` gives about 2,000 fewer spurious overload failures than `-j 32`),
and codegen re-types every instantiation it emits although the checker typed it before.

**Proposal (kovarex, 2026-10-28): an instantiation is a thing of its own, built once.** Each
(template, arguments) gets its own typed body when it is first instantiated, and keeps it: the
checker types it, codegen reads it, nothing re-types it, and nothing writes into the template's
own tree after parsing, which becomes a read-only pattern. A non-template function is the
instantiation of itself with no arguments. Nested instantiations of the same template (variadic
recursion, `sum<int, int>` inside `sum<int, int, int>`), one node needed under two substitutions in
one function (a callee's default arguments for `f<int>()` and `f<float>()`), and a lambda body read
after its enclosing function was emitted all stop being hazards: each is a different
instantiation with a body of its own. Any thread can type or emit any instantiation. The output
no longer depends on which thread typed what last.

Two ways to hold an instantiation's types, measured on the full src (headline inputs, master of
2026-10-28 night): the front end's instantiation checks visit **6.59 M nodes** and type **3.97 M
expressions** (1.95 M of them names or member accesses, 0.45 M calls) in 15.4 k class and 46.8 k
function instantiations; codegen's re-typing visits another 3.58 M nodes and types 2.19 M
expressions in 102.7 k walks. The parsed tree is 3.58 M nodes in 322 MB (90 bytes a node); the
process peaks at 1.2 GB today.

1. **A typing table per instantiation**, keyed by node: the template's tree stays shared, the
   types live beside it. Dense tables (the template's nodes numbered once, one array per
   instantiation): about 90 MB for everything above; hash maps: about 400 MB. Every place that
   reads a type (about 60 in the resolver, 120 in codegen) reads it through the instantiation.
2. **A copy of the body per instantiation, typed in place** (the clang model): the template's
   tree is copied once per instantiation, the copy is typed and kept as that instantiation's
   body. Measured with the copier (compiler/src/instance.cpp) copying every body the front end's
   instantiation checks and codegen walk: **511 k copies (422 k from the front end, 89 k from
   codegen, mostly the same instantiations again), 12.1 M nodes, 909 MB, 1.6 s of CPU** (copies
   serialised under one lock for the measurement; spread over the threads it is a small part of a
   second). The process peaked at 2.0 GB instead of 1.2 GB. Most of the copies are member bodies
   of class instantiations (350 k, every member of every instantiated class, used or not);
   copying and typing a member only when it is first used, as C++ instantiates member
   definitions, would cut that. Readers do not change: codegen walks the instantiation's own nodes
   as it walks a plain function's today.

**What it buys, in time.** The speedup is parallel codegen, which either variant (or the
per-thread tables proposed first) makes safe; no variant is faster by itself. Codegen is 5.4 s
single-threaded on the full src; spread over 32 threads it is expected under 1 s (an estimate
until the parallel driver exists). On top of that, codegen stops re-typing what the checker
already typed: the re-typing is 17 to 24% of codegen's time in the profiles (2.19 M expressions,
102.7 k walks). The front end's instantiation checks become race-free, which makes the headline
reproducible run to run.

**Recommendation: 2**, kovarex's preference, since it fits comfortably. It removes the class of
bug at its root (there is one tree per instantiation, so nothing can read another's types), it
leaves the 180 readers alone, and the instantiations it builds are what the persistent compiler
keeps anyway. What it needs: a deep copy of a function body (every node kind, with references
inside the body to its own parameters, locals, labels and lambdas re-pointed to the copies); the
instantiation's identity stays the template's FunctionDecl plus its arguments, so lookups by
declaration (definitionOf, owningClass, qualified names) are unchanged; the checker types the
copy instead of the template; codegen emits from the copy and drops retypeFor and the per-element
re-typing of pack expansions (a pack expansion becomes one more instantiation-like copy per
element, or is typed per element in the copy). Memory can be won back by dropping an
instantiation's copy once it is emitted, if a whole-program build ever needs it; the persistent
compiler keeps them.

**Decided and under way.** kovarex chose 2 on 2026-10-28. Step 1 is done: codegen emits every
instantiation from its own copy, GlobalScope::instanceBody (built once per template, arguments and
object type by the first thread that asks, in that thread's arena, and typed there by the walk
retypeFor did); codegen no longer writes into a template's own nodes. On the headline inputs it is
at parity with master: 29,615 stderr lines against 29,567 on the same frozen tree, the difference
being one pre-existing overload bug (templates/lvalue_reference_parameter_takes_no_prvalue) whose
self-recursion goes deeper now that each copy's lambda is a closure type of its own. Codegen took
6.3 to 9.1 s against master's 5.9 to 7.8 s in those runs (the copying, until the checker types the
copies itself; the machine was shared with other builds, so the spread is wide).

Step 2 is done: the checker's instantiation checks (instantiateFunction, and every member body
instantiateClass checks) type a copy each instead of the template's nodes, under one record per
(function, arguments in any order, `this` type) that codegen's instanceBody shares, so codegen
emits what the checker typed. The template's own nodes are written by the generic pass only. A
callee's default arguments come from its instantiation's copy too. On the frozen tree: 29,196
stderr lines against master's 29,177 (noise), codegen 6.45 s against 6.89, 79.7 k of codegen's
94.9 k requests served by the checker's copies (the rest: instantiations the checker did not
reach), peak memory 1.94 GB against 1.34 (the copies of every member of every instantiated class,
as measured above). Next: per-instance member initializers and statics, pack expansions and
generic lambdas (codegen still re-types those in place), then the parallel codegen driver.

**Member bodies where they are used (2026-11-08, coordinator and D).** A class instance's member
bodies are copied and typed where they are used, as C++ instantiates a member function's definition
([temp.inst] 11): a call, an operator, range-for's begin/end, argument-dependent lookup's hidden
friend, or codegen's own request (instanceBody); with the class only its virtual member functions
and destructor (the vtable), its member specializations, and every member of an explicit
instantiation definition ([temp.explicit] 10). An error in a member nobody uses is no error, as in
C++ (most were shown only for emitted bodies already: reportCheck in an instance); the count
"static_assert: N failed in template instances' functions never emitted" counts the bodies walked,
fewer now. Numbers in compile-speed.md, "Step 4".

**The shape it has now (2026-10-29).** Everything typed for an instantiation is a copy owned by
it, built once under a claim-once record keyed by (declaration, arguments in any order, `this`
type), by whichever thread asks first:

- GlobalScope::instanceBody: a function's body, parameters and default arguments (the checker's
  instantiation checks build most of them; codegen builds the rest).
- GlobalScope::instanceInitializer: a data member's default initializer, a class template's
  static member's initializer, a variable template's initializer. A default member initializer is
  emitted with `this` the object it initialises.
- GlobalScope::lambdaInstance: a generic lambda's call operator per set of its own arguments (its
  parameters and body copied, its captures and closure the lambda's).
- GlobalScope::privateInstance: for the one emitter that re-types a function once per pack element
  (or `for constexpr` element), a copy of the instantiation nobody else reads; its lambdas keep the
  instantiation's closure types.

retypeFor survives only for that per-element re-typing of a private copy; nothing writes into a
template's own nodes after the generic pass. With that, codegen emits on all threads (level by
level: the program's functions, then each round of the instantiations they asked for), the
emitters sharing layouts, closures, globals and the output under one lock and keeping their own
caches. Its output does not depend on the threads or on timing: struct, closure and constant names
are hashes of their contents, a lambda's mark in an instantiation is a hash of the arguments as
printed, filters and function-local statics are named by where they are written, module texts
are sorted, and every use of a failed layout or global reports the same error.

## Compile-time evaluation: one interpreter (decided 2026-10-29; steps 1 to 3 done)

**Today.** Two evaluators compute constants: the checker's (constEval, for template arguments,
`if constexpr`, requires clauses, static_assert) and the code generator's (evalConst, for the
same plus static members and variable templates as it emits them). Both compute only 64-bit
integers from expressions; a call folds when the function's body is a single `return expr;`.
Measured on the converted game (2026-10-29): 75 static_asserts the checker cannot evaluate, for
floating point (`1.0 / ValuesPerOne > 1.0e-9`), a constexpr member function called on a constexpr
object (`BoundingBox::emptyValue().isZeroSize()`), constexpr arrays and strings
(`variantNames[i].empty()`, `layerSpriteOptions.size()`), fold expressions, and function bodies
with statements. Of about 1,480 constexpr function bodies in the game, 874 are one return, about
300 are constructors with member initializers, about 210 have locals, `if` or `switch`, about 30
have loops. The library's gcd and lcm had to be rewritten as single recursive returns to fold.

**Proposal.** One interpreter, used by every place that needs a constant; "compile-time
computation is ordinary code run by an interpreter" (section 3) made true.

*What it evaluates.* Ordinary kpp, on the typed tree (an instantiation's own typed copy, so
templates need nothing special):
- Values: integers, enumerations, bool and char of every width (wrapping as their types do),
  float and double (IEEE, as the target computes them), nullptr, a function or member pointer (to
  compare and to call), class objects (a value per field, bases first; aggregates and classes with
  a constexpr constructor), fixed-size arrays, string literals (read-only char arrays), and
  references and pointers into the interpreter's own objects (a local, a field, an element; no
  arithmetic beyond an array's elements, no cast between unrelated pointers).
- Expressions: every operator on those values, conversions (implicit and the casts between
  arithmetic, enumeration and class types along bases), calls of functions with a body (free,
  member on an object value, static, constructors and destructors, templates as the call deduces
  them, lambdas as objects), `sizeof`/`alignof` and field offsets (from the layout, below),
  the built-in traits, requires-expressions, fold expressions.
- Statements: local declarations, assignment and compound assignment, `++`/`--`, `if`/`else`,
  `switch`, `for`, `while`, `do`, range-for over arrays and over objects with begin/end, `break`,
  `continue`, `return`, `if constexpr`, `for constexpr`, `static_assert`.
- Not evaluated: what has no kpp body (C functions, intrinsics, the C library's math), `new` and
  `delete`, inline assembly, reading a global that is not a constant, exceptions. The library's
  own constexpr code (Limits, Array, StringView, Span) is ordinary code under these rules, with no
  case in the compiler.

*Which functions.* Any function whose body is in the subset, marked `constexpr` or not (question
1 below); the converted C++ marks them anyway.

*Limits*, implementation limits that are compile errors when reached: 1,000,000 steps (statements
and expressions executed) per constant, calls nested 512 deep, 64 MB of interpreter objects. The
error names the constant and the limit.

*Errors.* A constant that a context needs (static_assert, a template argument, an array bound, an
enumerator's value, `if constexpr`, a requires clause, a `constexpr` variable) and that does not
evaluate is a compile error naming the first thing that stopped it: "calls printf, which has no
kpp body", "reads the global gameTick, which is not a constant", "divides by zero (Math.kpp:12)",
"indexes element 4 of an array of 4", "reads the local x before it is set", "signed overflow in
int32 (question 2)". Never a silent pass, a guessed 0 or a fallback to run time. A `const` global
that is not needed as a constant and does not evaluate keeps today's run-time initialisation.

*Two modes.* Required: the contexts above, where a failure is the error just described. Try:
where a constant is welcome but not needed (the code generator folding an initializer or an
operand, a `const` global that may become constant data), a failure answers "not a constant",
silently, and the caller emits run-time code as it does today. The same evaluation serves both;
only what happens on failure differs.

*Folding is not evaluation.* Whatever question 2 decides for required constants, ordinary
run-time expressions the code generator folds keep run-time semantics: signed arithmetic wraps,
as the emitted code does, and never takes the erroring path.

*Where it plugs in.*
- The checker: constEval becomes the interpreter's entry for an expression in a context
  (scope, locals, substitution); constOfDecl, the constant parts of requires clauses, variable
  templates, static_assert and template arguments go through it. Traits stay built-ins
  (traitValue), which the interpreter calls. Template arguments keep the `Value` kind (64-bit
  integral, enumeration and bool constants); float or class constants as template arguments
  (C++20) are a later step, none needed by the game.
- The code generator: evalConst becomes a call to the same interpreter
  (GlobalScope::evaluate(expression, context, arguments, mode)), so the two can no longer
  disagree.
- Layout (agreed with the code generator thread, 2026-10-29): class sizes, alignments, field and
  base offsets (empty base optimisation, later multiple inheritance) become a pure function of the
  type in a file of their own, owned by the code generator thread, which derives its IR struct
  types from it. The checker runs before any code is generated, so the interpreter and the checker
  call that layout, not the code generator.
- alignas (2026-10-29): on a class, a data member, a global, a static or a local, as C++. LLVM
  aligns a struct field only to its type's ABI alignment, so where alignas asks for more (on the
  class, on a member, or a member's class has it) the padding C++ puts before such a member, and at
  the end of the class, is written out as `[n x i8]` fields, and the class keeps the size and the
  alignment sizeof and alignof give; an object of such a class, and a variable with its own alignas,
  gets an explicit `align` (its alloca, its global). A union's storage is integers of its alignment
  up to i128, so a member aligned to more makes the union over-aligned the same way. The heap is
  malloc's 16 bytes: `new` of a type aligned to more is not handled yet. Until the layout file
  exists the checker's sizes (resolver.cpp, constSizeOf and constAlignOf, for template arguments
  like `AlignedBytes<sizeof(T), alignof(T)>`) mirror these rules.
- Constant data: a constexpr global or static member whose value evaluates is emitted as constant
  data (its value lowered to an LLVM constant) instead of by a startup function.
- A cache of (declaration, arguments) to value, thread-safe like the instantiation records, so
  that `Limits<int32>::max()` or a table is computed once per program.

*Order of work*, each step measured on the static_asserts and the headline: (1) values (floats
included), expressions, calls of single-return bodies, static constexpr members of class template
instances, replacing both evaluators; (2) statements and loops (gcd and lcm back to loops; the
game's ~240 bodies with statements); (3) objects and constructors (the ~300 constexpr
constructors, `BoundingBox::emptyValue().isZeroSize()`, constexpr arrays and strings); (4)
constant data.

*Who builds it* (settled 2026-10-29): compiler/src/consteval.cpp, owned by the templates thread,
which also does step 1's cutover; constEval and evalConst become thin calls into it, the resolver
and code generator threads reviewing. Kept from constEval (174c22e): each name's evaluation starts
its own call depth (and, since step 3, each called function's body), nesting is bounded (1024, and
a check for stack room), and static members'
values are memoised per class instance (specs templates/pack_recursion_depth and
templates/variadic_partial_specialisation).

*Nesting is counted as if nothing were cached* (2026-11-08, the coordinator for kovarex: "an answer
must never depend on which thread does the work or how deep its stack already is"). Constants nest
1024 deep, the one asked included (a name's value needing the next one's: a template recursion;
`constexpr int v = Sum<1022>::value;` nests 1024, v then Sum<1022> down to Sum<0>), and calls 512 deep,
[implimits]' recommended limits and clang's defaults. A cached answer (a static member's value, a
trait's answer, a class's layout, a partial specialisation's match) keeps how much deeper its own
computation nested, and an asker it answers nests that much deeper; past a bound the evaluation
trips: no answer, why "constants nested more than 1024 deep", and nothing computed while it tripped
is cached. Each cached computation also has its own budget of steps and values and its own const
globals' values, and the values budget is per evaluation (it was a thread's for the whole build). C++
compilers count only the instantiations not made yet, so whether a deep recursion compiles depends on
the order things are asked (clang compiles Sum<1100> after Sum<25>, Sum<50>, ... and not alone); kpp's
declarations have no order and its threads no fixed one, so the count is the one with nothing made
before ([differences][diffnest]). Every thread kpp starts has the same stack, 1 GB reserved
(host::compilerStack: /STACK on Windows, glibc's thread default and main on a thread of its own on a
POSIX host; the compile server's build thread), so the bounds come before any thread's stack ends;
running out of it anyway is a trip, never an answer. A cached computation (and a partial specialisation's match, cached or
not) is computed as if asked from the top, whoever asks it from how deep, and cached computations
nested in one another count as a third bound, 4096 (2026-11-08, second step): a class template's
partial specialisation written 20 class templates deep was silently the primary template (the
match's own depth bounds counted from where the type was written: sizeof 1 for 100), and a class's
layout was refused to an evaluation more than 32 deep, or kept failed for every asker after a deep
one. Specs templates/partial_specialisation_picked_for_a_type_written_20_deep and
expressions/constant_aggregate_member_read_30_deep. So is a declaration's type, and an alias
template's type under its arguments (2026-11-08, third step), and a written type's own parts (a
pointee, a template argument) take no step of depth, only a name leading to another declaration's
text does (the parser bounds how deep a written type nests): a type written 21 class templates deep,
an alias of an alias 8 deep, a member alias naming its class's next instance's 8 times and a type
behind 30 pointers each named a class "not declared" (each step counted the asker's depth on, cut at
40); a declaration keeps how much deeper its type's computation nested beside the type
(Decl::typeNeeded), and a type under a substitution, kept in no declaration, is computed once within
the outermost such computation (SubstTypeMemo: `using A2 = P<A1, A1>;` in a class template doubled
the time per alias). A declaration whose type is asked while it is being computed (a C struct's
member `UOW UOW;`, the name finding the member before the typedef) has no type there, at once, and
the computation takes the name's other meaning; a computation a cycle was cut through from outside
it is not kept (it holds for the way in, not for every asker). Past 4096 levels the variable whose type it is gets the error "the type of d:
... nested more than 4096 deep" (clang's bound is 1024 nested instantiations). Specs
templates/types_named_however_deep_they_are_written_or_reached and
templates/type_nested_past_4096_declarations_is_error. Before, an 8 MB thread's stack ended about 980
constants deep (a static_assert of one 1000 deep failed where a constexpr variable's passed), and
which cached values a walk found decided whether a recursion past 1024 compiled (the same program
had 10, 115 and 187 errors at -j 1, 8 and 32). Specs templates/constant_recursion_1024_constants_deep_on_every_thread
and templates/constant_recursion_past_1024_is_error_whatever_was_computed_first.

[diffnest]: differences-from-cpp.md#constants-nest-1024-deep-counted-with-nothing-made-before-built

*Step 1, done 2026-10-29.* compiler/src/consteval.cpp holds the values (ConstValue: an integer
normalised to its type's width and signedness, an enumeration with the underlying type it was
declared with, bool, char, float or double) and C++'s arithmetic on them (ConstArith: literals
with every suffix and escape, integral promotion and the usual arithmetic conversions, modular
conversions between integer types, shifts in the promoted left type, IEEE `+ - * /` and
comparisons), in two modes: *Required* for the checker, where a signed overflow, a division by
zero (a floating one too), a floating result that is a NaN, a shift by the width or more, a float
out of an integer's range, or a value out of the range of values of an enumeration declared without
an underlying type ([dcl.enum]/8: `(E)2` for `enum E { A, B }`) fails the constant with
its reason (each as clang 19 refuses it; a floating overflow to an infinity stays a constant, as
there, and outside a required constant the folded value is IEEE's or the kept enumeration value,
as at run time) (static_assert reports it: `static assertion whose condition is no constant: signed
overflow in int32 (addition)`), and *Fold* for the code generator, where signed arithmetic wraps.
The walk types number literals with the same function, so a literal's type and value cannot
disagree. The checker's evaluator (constValue in resolver.cpp; constEval is its integer view)
covers step 1 and the part of step 2 that changes nothing: function bodies of returns, `if`,
`if constexpr`, locals initialised once and `assert` (`Limits<T>::max()`, the library's `max`),
member functions on an object whose result its type decides (`layers.size()` of an Array),
conversions through aliases, enumerations and type parameters (`size_t(Layer::Count)`,
`U(2 * limit)`), fold expressions over template packs, `sizeof` of a variable, and `alignof`
(which was computed as `sizeof`). Long operator chains (InputAction::inputActionCount's 300 terms)
are evaluated in a loop. The code generator's evalConst computes through the same ConstArith and
runs the same statements, on the callee's own instantiation (`Limits<uint16_t>::max()` in a
static_assert on a layout); its lookup of names (variables, enumerators, parameters) is still its
own, which the GlobalScope::evaluate entry replaces next. Limits as proposed: a million steps, 512
nested calls, the stack left. Measured on the converted game: static_asserts the compiler cannot
evaluate 39 -> 22; the headline 22,776 -> 22,562 paired on 9a5ce8d (path-to-factorio.md). Specs:
language/consteval_*.

*One entry, and step 2, done 2026-10-29.* The code generator's constants go through the same
evaluator: GlobalScope::evaluate(expression, file, context, arguments, hooks), the hooks being
the code generator's arithmetic mode (Fold), the integer types it stores enumerations in and its
layouts for `sizeof` and `alignof`; its own evaluator is gone, so the two can no longer disagree.
Step 2 runs assignments (plain and compound, converted to the variable's type), `++` and `--`,
`for`, `while` and `do` with `break` and `continue`, and `switch` with fallthrough and `default`,
on the locals and by-value parameters of the function being run; a void function runs for what it
checks (its asserts). Writing through a reference parameter, and a local reference, are no
constant until step 3 has objects (spec language/consteval_reference_parameter): never the local
copy's value. The step limit counts statements as well as expressions. Specs
language/consteval_loops_and_assignments, language/consteval_float_variable_templates.

*Step 3, objects and constant arrays, done 2026-10-29.* A value is also an object (a class's
parts, its bases then its data members in declaration order, or an array's elements; shared, and
copied when written, so no value changes under another), a pointer into an array (a string
literal's chars, a constant array; arithmetic and comparison within it, one past the end
included), or unset (a member or local no initializer set; reading it is no constant). Objects
come from aggregate initialization (brace elision: `Array<StringView, 6> names = {"a", ...}`,
designators), constructors (their member and base initializers, delegation, default member
initializers, then the body with `this` the object), implicit conversions by a constructor marked
`implicit`, and value- and default-initialization; member functions run on an object as `this`
(a const one, or one whose changes stay in the local it was called on), member operators
(`names[i]`) too; members and elements are read and assigned through places (`point.x`,
`items[i]`, `this->count`). A string literal is an array of chars, and the interpreter knows one
function of the C library: `strlen` of a pointer into a char array, which the library's
`StringView(const char*)` counts with (decided 2026-10-29: no other C function has a body to
run). Folds also expand a function parameter pack (`(sizeof(values) + ...)`). Not yet: class
operators other than member ones (`a == b` for a class), virtual calls, addresses of locals and
references to them, objects as template arguments, and objects as the code generator's constant
data (it builds them at startup). Specs language/consteval_objects,
language/consteval_arrays_and_strings. Measured paired on ec5bcb1: 22,160 -> 22,156, the
static_asserts on `variantNames[i]` and `layerNames[i]` evaluating.

*The rest of step 3, done 2026-10-29.* Class operators run as the class declares them: member and
free operators (`a < b` by a free `operator<`), `!=` rewritten from `==` and the relational ones
from `<=>`, a defaulted `==` or `<=>` comparing bases and members in order, a class's own `+=`
and `=`, and an explicit `operator bool` in a condition. A virtual call runs the overrider of the
object's own class (`s.sides()` on a Square given as a `const Shape&`): an object given to a base
reference keeps its class, one given by value is sliced. References are aliases: a local
reference, or a reference parameter given a local, a member or an element, reads and writes what
it refers to (`int& r = a; r += 10;`, `bump(a)`), and a `const` reference to a temporary holds a
copy. Two rules made the game's `BoundingBox::emptyValue().isZeroSize()` evaluate. A called
function's body counts its own nesting from zero: how deep the calls go is bounded by the guard of
512 nested calls and the check for stack room, so a `sizeof(T)` six calls deep converts its type as
written (the type walker, which was handed the evaluator's depth, stops at 40 levels). And a member
a body names (`this->leftTop`, `width` alone) is found by name in the class of the object the
evaluation holds when no walk has typed that body yet: a constexpr member function of a class
declared further down, or in a file walked later (BoundingBox.kpp after CameraWidgetUndoRedo.kpp),
since symbols are order-independent. Not yet: addresses of locals (`&x`), objects as template
arguments, objects as the code generator's constant data. Specs language/consteval_class_operators,
language/consteval_reference_parameter, language/consteval_deep_calls,
language/consteval_members_declared_later. Measured paired on 3100f7b: 22,035 -> 22,034, static
assertions the compiler cannot evaluate 16 -> 15.

*Step 4, the interpreter's half, done 2026-10-29.* Constant data is split with the code generator
thread (agreed 2026-10-29): the interpreter computes a global's or static member's value as the
type it initializes (GlobalScope::evaluateInitializer, the declared type given, so that `Flags f =
{A, B};` is a Flags), the code generator lowers what it can to an LLVM constant
(`constantText`, in globalForOnce) and builds the rest at startup as before. The values it may
lower, and how they are laid out, are the contract written above ConstValue in consteval.hpp:
Objects' parts bases then members in declaration order, bit fields each a part already fitted to
its width, arrays one part per element, Pointers into arrays the evaluation made (string
literals, a brace list's elements), and two new kinds: an Address of a declaration or of a part of
a variable (`&origin`, `&table[1]`, a function, `thrice` decaying to a pointer: a ConstTarget of
the declaration, the class template instance it belongs to, a function template's arguments and
the path into the value), and a MemberPointer (`&Point::y`, `&Calc::mul`). What the interpreter
gained for the game's tables: brace lists as values until their type is known (an argument, an
element of another list), InitializerList (a constant array of the elements and the list's
pointer and count), a class's InitializerList constructor first in list-initialization (the
BITFIELD flags), range-for over arrays (their elements as places: `for (int& v : items)` writes
them), brace lists and objects whose begin() and end() point into one array (a range changed by
the body is no constant rather than a stale read), reads, indexing, arithmetic and comparison
through Addresses, calls through function pointers, `.*` and `->*` on data members. And fixes
that were wrong constants or failures: a bit field holds its value at its width (a 3-bit field
given 13 read 13), an unnamed bit field takes no aggregate initializer (`{1, 2, 3}` gave it the
2), a member initializer is a direct-initialization (an explicit constructor takes `bits(x)`),
explicit(cond) with cond false converts, a function template called with the running function's
parameters deduces from their values (`Math::isNaN(d)`), and a literal is read from the tokens of
the file it was parsed from (LiteralExpr::file): 1,708 literal reads in the game used another
file's tokens, failing or, where the token there was a number too, computing with it. Measured
by evaluating every emitted global of class or array type as its type (a temporary probe in
globalForOnce): 158 of 349 were objects, 212 now; of the 137 left, 95 are String, Vector, Map and
the other containers that allocate, which never become constant data. The headline is neutral
(21,826 = 21,826, paired on 6690f44): the code generator does not use objects yet. Specs
language/consteval_initializer_lists, consteval_bitfields, consteval_unnamed_bitfield,
consteval_addresses, consteval_direct_initialization, consteval_conditional_explicit,
consteval_deduction_from_values.

*More of the game's tables, 2026-10-29.* Class template argument deduction in constants (`Pair {
"mask", SpriteOptions::Mask }`, `constexpr Array names = {...}`): the class the walk deduced, the
expression's type or the variable's (cachedType), unless it depends on an instance's arguments.
Writes through a member function's reference result (`result[i] = x` by an Array's operator[],
`front().a = 7`): such a call asked for a place runs with `this` referring to the caller's object
and its `return` giving the place it designates (TypeContext::returnPlace); only where the
expression is not evaluated for its value too (an assignment's left side, `++`, a reference
bound, a range, the object of a member call), so that no call runs twice. Constructors inherited
by `using Base::Base;` (no aggregate then), constructor templates (their parameters deduced from
the arguments: GamePair's forwarding constructor), casts to a reference type (`(T&&)value` in
forward and move) keeping the object, lambdas called where they are written (capturing nothing),
`"type"sv` as the chars it spells (as the walk types it), and implicit conversions by the
constructors the walk converts with (isExplicit: all but those explicit), where the interpreter
had taken only those marked implicit. Where the values' types leave two constructors equally
good (`Pair(A&&, B&&)` and `Pair(const A&, B&&)` for a string literal), the types the walk gave
the written arguments decide. The probe: 212 -> 227 of 347 evaluate; of the 120 left, 21 do not
allocate. Headline neutral (21,682 = 21,682, paired on 062c7de). Specs
language/consteval_class_template_deduction, consteval_reference_results,
consteval_inherited_constructors, consteval_constructor_templates, consteval_called_lambdas,
consteval_converting_constructors, consteval_sv_literals.

Left: addresses of the run's own locals (no use in the game's constant contexts); the code
generator's lowering (its thread's); lambdas that capture, and a table of lambdas as function
pointers (TransportBelt's); the 21 non-allocating tables left are one of a kind (a default
constructor that is not constant, Debug tools' values); `sqrt` (the library's has no kpp body
yet); float template arguments. The library's gcd and lcm can go back to loops. A namespace-scope
`Pair<int, Mode> five(a, m);` with variables as arguments is parsed as a function declaration
(the parser's guess there; the walk and the code generator see no variable either). The game's 9
static_asserts left need: VariantAlternative over a variant
(SegmentedUnitAI, 5), `sizeof` of a member through a reference (`maskUniform.uvTransform`) and of
another class's member (`GraphicsInterface::orthoProjectionMatrix`), and `typeID > 0` in
RendererContainer's instances (2), which the code generator evaluates.

*Objects of the run, 2026-10-31 (for build.kpp, section 12).* The interpreter runs the program's
object lifetimes, so that build.kpp can compute with Vector and String. Copies and moves run the
class's own copy and move constructors with C++'s value categories: `T b = a;` copies, as do
`T b(a)`, `T b{a}`, `T(a)`, `T{a}` and an aggregate's `W b{a}` (spec
language/consteval_direct_initialization_copies_and_moves),
`static_cast<T&&>(b)`, `move(b)` and `forward<T>(x)` of an rvalue move (the source's place given to
the constructor, so a move empties it; an xvalue whose place the run does not know, `move(v[0])`,
is no constant), a by-value parameter is copied or moved from its argument, an initializer is
evaluated once (language/consteval_initializer_evaluated_once), an operand of class type that a
call gives as an lvalue (`x = v[i]` through operator[], `x == v.front()`) is the object where it
is, the call run once on its object in place (language/consteval_operand_from_call_where_it_is),
`return local;` moves (a constant evaluation elides no named object, [class.copy.elision]/1; the
program elides it where section 23's rule holds, and the interpreter is to follow, D), a prvalue
initializes its object directly, and an implicit copy or move
runs the members' and elements' own (spec language/consteval_runs_copy_and_move_constructors).
Destructors run where the program runs them: a scope's locals at its end, the last first, also on
return, break and continue; a by-value parameter's at the end of its call; a temporary's at the end
of its full expression (one a reference parameter binds, the object a member function runs on, a
value discarded; a `const T&` local extends it to its scope); members after the body, then bases;
an array's elements the last first (spec language/consteval_runs_destructors). The run takes the
address of its own locals and parameters (`&x`, an array decaying to its first element) and works
through it as through any pointer: `*p`, `p->m`, `p[i]`, member calls, arithmetic and order within
an array, comparison; an address into a local whose lifetime ended is an error naming it, never
the value it had nor what took its place (specs language/consteval_pointer_to_local,
language/consteval_dangling_pointer_is_error). The run destroys only what it knows it made: a
binding that copies an object it could not refer to (a reference given a value the run holds no
place of, `this`) is never destroyed and gives no address, and an object an operator takes by
value is not destroyed yet. An evaluation whose result points at one of its own objects is no
constant. A local default-initialized (`List<int> l;`) is made where it lives: its constructor runs
with `this` the local, and a destructor runs on its object where it is (a local, a heap block, an
explicit `p->~T()`), so that an object pointing at itself (a List's sentinel) works at compile time
(language/consteval_local_made_in_place), whatever initializes it: arguments, a brace list, a copy or
a move (`T x(a)`, `T x = T(a)`, `T x{a, b}`, `T b = a;`: language/consteval_local_made_in_place_from_arguments). A structured binding's names refer
into its hidden object (language/consteval_structured_bindings), and `c ? a : b` of two lvalues is
one, a reference to it following it (language/consteval_reference_to_conditional_lvalue). A reference member
holds the address of the object it is bound to and is read and written through it, never a copy of
its value (language/consteval_reference_member_refers_to_object), and a lambda is a value, its closure,
whose captures by copy are copies and by reference (and `this`) addresses, its body run where it is
written whoever calls it (language/consteval_lambda_captures); a generic lambda's call runs the
instance of its call operator for the arguments the call deduced, as code generation calls it
(language/consteval_generic_lambda_call). A reference bound through a cast to a
reference type, through move() or forward() or a call returning `T&&`, refers to the object too
(language/consteval_reference_through_cast), and a call whose reference result a reference is bound
to runs once: a result that refers to a constant (a global's element) is its value, one that refers
to no object the run holds a place of is no constant, never a second run for a copy
(language/consteval_reference_call_runs_once). So do `*it` and `*optional` (the class's operator* run on
the operand where it is), `s[i]` through a pointer a call gives (String's `this->data()[index]`), `++a`,
`(a = 3)` and `(x, a)`, and a const reference a call returns given as an argument
(language/consteval_reference_to_operator_result). A constructor makes its bases, then its members, in
declaration order whatever order its initializers are written in (language/consteval_initialization_order),
in the object where it is made, so that an initializer, a default member initializer included, reads
the parts made before it, `this` and their addresses; one not made yet is unset, no constant
(language/consteval_initializer_reads_earlier_member). A virtual call runs the overrider of the
most-derived object that its object is a base part of, wherever a pointer or a reference to that part
leads (an array's element, a member), and in a base's destructor the base's own
(language/consteval_virtual_call_through_base_pointer); a function named with its class (`A::f()`,
`super::bump()`, `b.A::f()`) is that function, run on the object's part of its class
(language/consteval_qualified_call_is_no_virtual_call). A constructor is chosen, and a constructor
template's parameters deduced, by the arguments as written, a class object's value category included:
an lvalue is copied, never moved (language/consteval_forwarding_constructor_copies_lvalue), through a pack
too (`forward<Args>(args)...` by its elements' value categories: language/consteval_pack_forwarding_copies_lvalue).
A lambda's captures by copy are its closure's parts where the closure is, which a `mutable` lambda's
body may change and which keep the change for the next call, as the program has them, and its
reference parameters refer to their arguments (language/consteval_sort_and_lambda_state). In a lambda
that is not `mutable` the copies are const, as C++ has them, and writing one is a compile error
(language/lambda_copy_capture_is_const_unless_mutable). A typed union's alternative (`v[V::Number]`) is
read and written where it is, the union holding it (else no constant), and a typed union is destroyed
by destroying the alternative it holds (language/consteval_typed_union_alternative). A local rvalue
reference bound to a temporary (`String&& t = String("de");`, a `long&&` given an int) holds the
temporary as an rvalue reference parameter does and may change it: `move(t)` empties it
(language/consteval_rvalue_reference_to_temporary_changes_it). A constructor's initializer may name
a base through an alias (`using Leaf = TupleLeaf<...>;`, `: Leaf(head)`), so a Tuple is a constant
(language/consteval_base_initialized_through_alias). A union's member no initializer names is not
made (a String member's block was allocated and never freed), so a union holding a String, made in
it by placement new and destroyed there, is a constant (language/consteval_union_member_not_constructed).
A constructor's parameters bind their arguments as a function's do (one rule for both): a by-value
one is copied by its class's copy constructor (moved from an xvalue) and destroyed when the
constructor returns, a reference given a prvalue refers to a temporary of the full expression, which
a move empties and that end destroys, and a `const String&` given a StringView refers to the String
made from it; copy-initialization through a converting constructor binds the initializer by its
value category (`Optional<String> o = s;` copies s, `Optional<String> o = String("x");` moves from
the temporary), so an Optional<String> made from a String is a constant
(language/consteval_constructor_parameters_bind_as_function_parameters). `o->f()` and `o->x` of an
object of a class with operator-> (an Optional, an iterator, a handle) run its operator-> where the
object is, and a class object one returns runs its own in turn, down to a pointer: the member
function runs on the object that pointer points at (language/consteval_member_call_through_operator_arrow).
`dynamic_cast` reads the complete object a base part belongs to: a pointer becomes one to it, or
to its part of the class asked for (a derived class's object, a cross-cast to a second base), else
the null pointer; a reference the same where it is, and no constant where it has no such part
(at run time it throws BadCast) (language/consteval_dynamic_cast).

*A constexpr variable needs its constant, 2026-10-31.* A `constexpr` (or `constinit`) variable,
global, static member or local, whose initializer the interpreter cannot evaluate is an error
("the constexpr variable x is no constant", with the reason when there is one), never a variable
initialized at run time (C++ [dcl.constexpr] 10; spec
language/constexpr_variable_that_is_no_constant_is_error, found by thread E for build.kpp). It is
checked where its type is known, as a static_assert is: a template's instances, not the template;
one whose value needs the code generator's exact layouts is the code generator's, which reports it
the same way ("the constexpr variable x is no constant the compiler could evaluate"; it built
such a variable at startup without a word until 2026-11-05, D: spec
language/constexpr_variable_needing_layouts_that_is_no_constant_is_error). A value the interpreter
computes that no LLVM constant expresses yet (a typed union's) is still built at startup, as step 4
of "Compile-time evaluation" has it. Pointers to data members, alone or in arrays, are constant data
(language/constexpr_member_pointer_is_constant_data: built at startup, a global initialized before them
in their file read the first member), and so is a scalar whose braced initializer (`{ID(-1)}`) the
fold did not read. The game had 21
constexpr variables the interpreter could not evaluate, all initialized at run time without a
word; each was an interpreter gap, now closed (specs language/consteval_game_constexpr_tables,
language/consteval_unions, language/consteval_memcpy_and_memset): a char array from a string
literal; a class template named without its arguments in a call (`Pair(1, "one")`); a type
parameter naming a class constructed with several arguments (a CRTP base's `V(x, y)`); unions,
holding one member alive as C++ does (writing one ends the others, reading another is no
constant), anonymous struct and union members (their fields named as the class's), and typed
unions made from a value of one alternative's type or from a tag and value; `memcpy`, `memmove`
and `memset` over the evaluation's own objects by their types (a whole object, a scalar's bits as
another scalar of its size, element runs; anything else is no constant, the evaluator having typed
values, not bytes); `*optional` and `Optional`'s construction in place, for which `this` refers to
the object where it is (the caller's local, a temporary) rather than to a copy; a range-for over
a local Array writing through `auto&`; a `using namespace` in a body. Four of the game's globals
became constant data instead of being built at startup (`BlueprintBook::ICON_SHIFT`,
`CyclicFramePosition::EMPTY` as the positive quiet NaN, `RenderLayer::sortLayer`, Lua's
`udatatypename`); no other function's IR changed. A walker bug was found on the way: a global or
member took the type deduced for the declaration walked before it. Measured paired (front end on
the converted game, 5 interleaved rounds): 1.99 s before and after.

*Allocation, 2026-10-31.* `new T(args)`, `new T[n]{...}`, placement new into the evaluation's own
objects, `delete` and `delete[]` (destructors run) allocate from memory that belongs to the
outermost evaluation (C++20's transient allocation): all of it is freed before the evaluation
ends, else "memory allocated at File.kpp:7 is never freed"; a read of freed memory, freeing twice,
and `delete` of what `new[]` allocated (or the other way) are errors naming where (specs
language/consteval_new_and_delete, language/consteval_leak_is_error,
language/consteval_use_after_free_is_error). `malloc`, `calloc`, `realloc` and `free` allocate
from the same memory as the library does (spec language/consteval_malloc_and_free): bytes, typed
by the first cast to `T*` (`(T*)malloc(n * sizeof(T))`: n objects of T, uninitialized; calloc's
zero) or by a placement new into them; objects built there by placement new and ended by an
explicit destructor call (`slots[i].~T()`); realloc moving trivially copyable elements; `free` of
what malloc did not allocate, twice or through a pointer into a block, and `delete` of malloc's
memory, are errors naming where (spec language/consteval_malloc_errors). With them the library's
Vector and a small String (up to 22 characters) work in a constant. A longer String does not: its
small-string union is read through the member that is not alive (the mark byte in `smallChars`
while the heap fields are), which C++ makes undefined and the interpreter rejects; the fix is the
library's (proposed to its owner 2026-10-31; spec language/consteval_vector_of_strings, pending).

*isConstantEvaluated, 2026-10-31 (decided by the coordinator).* `if consteval { A } else { B }` and
`if !consteval` (C++23) work: A runs in a compile-time evaluation, B in the program, and B is the
only branch the program holds (the code generator emits nothing of A, nor anything only A calls).
The library's `isConstantEvaluated()` (std::is_constant_evaluated) is built on it: true at compile
time, false in the program (spec language/if_consteval_and_is_constant_evaluated). The library's
String uses `if consteval` to keep its heap representation at compile time, since reading its
small-string union's inactive member is undefined behaviour in a constant evaluation; its run-time
code, size and speed are unchanged (the game's IR byte-identical), and with it Vector<String> works
in a constant (spec language/consteval_vector_of_strings). Whether String's run-time reading of
that member is acceptable is a language question for kovarex.

*The driver's entry, 2026-10-31 (E's interface).* `GlobalScope::runBuild(configuration, error)`
computes a build description in the interpreter, without building an executable: a `Project`
default-constructed, its `configuration` the enumerator of that name (when Project has that
member: since 2026-11-01 projectlib's has none, and `--run-build matrix` describes every
configuration at once), `build(project)` run, then
`describe(project)`, whose String's characters it returns; both are destroyed after, so memory
left allocated is an error as in any constant. When that is no constant, the error names the
innermost statement that failed and why ("build.kpp:6: a call to puts, a C function, which has no
body to run at compile time"). `kpp --run-build CONFIGURATION files...` prints it (run_tests'
runbuild case); `--run-build debug,release` prints each in turn, which is how kpp describes a
project (project.cpp, through the compile server; projectlib's `describe`), with no describe.exe.

*Its cost, measured 2026-10-31 (stage 7).* A description at Factorio's scale, written with Vector and
String as projectlib will be (15 C libraries of 20 sources, 5 include directories and 5 defines
each, two executables; 481 lines of description; `C:\kpp-sandbox\scratch\templates\rb7`), with
`C:\kpp-sandbox\scratch\templates\runbuild-time.ps1` (10 interleaved rounds, `kpp --std stdlib
--quiet [--run-build release] projectlib.kpp build.kpp`): the front end alone 37 ms, with the
evaluation 7,429 ms, then 178 ms after a write into a value no other value shares (the binding's
own array) stopped copying the array: about 145 ms of interpretation per configuration. Two
gaps the description found, closed: an assignment operator runs on both objects where they are
(spec language/consteval_assignment_operator_runs_on_both_objects), and a class prvalue converted
for a parameter of another type (`path(path(a, b), c)` with a StringView parameter) is a temporary
destroyed at the end of its full expression.

*For build.kpp's descriptions, 2026-10-31.* A StringView compares with a literal or another
StringView at compile time (`memcmp` over chars, and an object compared with a pointer or array
by its class's operator, not as pointers), and a call, a member function's too, may leave trailing
parameters to their defaults (spec language/consteval_string_view_and_default_arguments). A
constant that calls a function without a body says so: "a call to puts, a C function, which has
no body to run at compile time" (spec language/consteval_reason_names_a_call_it_cannot_run).
String::find runs too: `memchr` and `strcmp` over chars as `memcmp` (2026-11-05), and a string
literal or an array binds a reference to an array (`const char (&)[4]`, `const int (&)[3]` given an
int[3], a forwarding `T&&`), so `Map<int, String>::emplace(2, "two")` is a constant (spec
language/consteval_string_literal_binds_array_reference).

The three questions were answered with the proposals on 2026-10-29 (the go-ahead came while
kovarex was away; his to revise): any function with a body may run at compile time, `constexpr`
optional; signed overflow at compile time is an error, as in C++ (spec
language/consteval_signed_overflow), while folding keeps wrap-around; floating point is the host's
IEEE arithmetic for `+`, `-`, `*`, `/` and `sqrt`, nothing else from the C math library.

## Benchmark target

Factorio: ~1.3M lines of own C++ in ~8,000 files (~1.6M with tests). The real build, measured
2026-10-23 on this machine (Ryzen 9 9950X, 16 cores, MSVC 14.44, FASTBuild with unity builds of
about 100 files and a precompiled header; details and the commands in
[factorio-build-baseline.md](factorio-build-baseline.md)), Debug x64 with the game and the test
executable together:

| scenario | wall time |
|---|---:|
| clean rebuild | 62 s (919 objects, 19 min 20 s of CPU) |
| one-line edit in an ordinary header | 16 s (5 objects; about 10 s of it is linking) |
| one-line edit in a header included by the PCH | 55 s (143 objects, nearly a full rebuild) |
| no-op rebuild | 0.1 s |
| unit tests, `factorio-test.exe --fork` | 59 s (9,012 tests) |

(The first estimate here was 2 to 4 minutes; the measurement replaced it.) A 10x win therefore
means a clean debug build in about 6 s and a one-line change, in any header, in under 2 s.
Where kpp stands (see [prototype-results.md](prototype-results.md)): the whole front end over
Factorio's own code runs in 0.32 s on 32 threads, and for a generated program with Factorio's
number of function bodies a debug build is 1.7 s from source to executable and 1.25 s after a
one-line change, release 3.8 s of LLVM; the converter will show what the real code costs.

## kpp v0: the first runnable subset (proposed 2026-10-23, not yet reviewed by kovarex)

The smallest language that is still kpp, as the target of the first code generator:

- **Files and scope.** No `#include`; the project is the set of files given to the compiler.
  Every top-level declaration is visible in every file, in any order. Namespaces as in C++.
- **Argument-dependent lookup, as in C++** (resolver thread, 2026-10-29, for kovarex's review).
  An unqualified call `f(args)` also finds the functions named `f` that the namespaces
  associated with its arguments declare: the innermost enclosing namespace of each argument's
  class or enumeration (through pointers, references and arrays) and of a class instance's type
  arguments. The functions found both ways are one overload set. It does not apply when
  ordinary lookup finds a class member, a local or something other than a function. Why: the
  converter keeps the game's unqualified calls as written (`shortNumberFormat(time,
  StringUtil::Compactness::Loose)` from namespace TimeUtil), so without the rule it would have to
  invent a qualification for each such call, a language rule hidden in the converter. Not yet:
  functions declared only as friends inside a class, associated classes, operators found
  through argument types. Spec: language/argument_dependent_lookup.
- **Class template argument deduction, as in C++** (resolver thread, 2026-10-29, for kovarex's
  review). `Box b("hi")` deduces the class's arguments from a constructor, from a deduction guide
  (`Box(const char*) -> Box<String>;`, `template<typename... Ts> Overload(Ts...) ->
  Overload<Ts...>;`), or, for an aggregate, from its bases and data members in order (C++20; a
  pack of bases, `struct Overload : Ts...`, takes the remaining elements). A guide competes with
  the constructors by its conversions and wins a tie; an explicit guide is no candidate for
  copy-initialisation (`T x = ...`). Why: guides are part of C++'s deduction, and dropping one
  (as the parser did) deduces other arguments than the code says, silently. Spec:
  templates/deduction_guide_target. With nothing to deduce from (`NP p;`, `NP()`, `new NP`) a pack is
  empty and a parameter takes its default, a class of no constructor having the guide of `C()`
  (2026-11-06, templates/class_arguments_with_no_initializer); a template's own walk leaves `NP p;` to
  its instances, as the name inside the class is the injected one, in a specialization that
  specialization (templates/class_name_inside_a_specialization). A pack of base classes (`struct Overload : Ts...`) is a base per
  element of its pack (2026-11-06, classes/pack_of_base_classes; an error from 2026-10-31 until
  something needed it, thread A's C++ feature survey), and so is the expansion of a pattern, `struct
  Many : Leaf<A>...` ([temp.variadic] 5), whose `Leaf<A>(a)...` (or `Ts(ts)...`) initializes each
  base from the pattern's element (2026-11-07, templates/pack_expansion_pattern_as_base); a lambda's closure is such a base too,
  its captures its subobject (classes/lambda_closure_as_base), converting to it as to any base
  (classes/conversion_to_closure_base). A closure's traits and its destructor's exception specification
  come from what it holds by value as the code generator lays it out (GlobalScope::closureValueParts,
  the same capture analysis, which the layout checks against): with a lambda-capture (`[=]` taking
  nothing included) no default constructor and a deleted copy assignment ([expr.prim.lambda.closure]
  13), its copies, moves and destruction trivial, nothrow or potentially-throwing as its parts' (2026-11-07,
  library/type_traits_of_closures; until then such a closure answered no trivially-* trait and every closure
  default-constructed, J's battery), and a class holding one by value overrides a noexcept destructor by
  what it holds (templates/destructor_loosening_noexcept_through_a_closure_member_is_error). A closure type's call operator is a member function for
  lookup and overload resolution, as in C++ ([expr.prim.lambda.closure] 4): `using Ts::operator()...;`
  names the closures' call operators, one overload set with the class's own, ranked by the one rule
  (rule 24; a generic lambda as a template), and a call through the class (`Overload o{[](int)
  {...}, ...}; o(41)`, the overload-set idiom given lambdas) calls the chosen lambda on its base
  subobject (2026-11-06, templates/overload_set_of_lambdas); two that take the arguments alike are an
  ambiguous call. A variable whose arguments are deduced keeps its declaration's const and volatile
  (`const Box b{1};` is a const Box<int>; until 2026-11-06 the const was dropped,
  templates/class_template_argument_deduction_keeps_const).
- **nullptr is the null pointer; the literal 0 is no pointer** (kovarex, 2026-11-04: "b is cleaner long
  term, and if we can convert it, I would prefer that, writing 0 instead of nullptr is not nice"; it
  replaced C++'s rule, kept from 2026-10-29 until the converter wrote nullptr, C's 6f248ebe). An integer
  0 of kpp's own sources that would become a pointer or member pointer without a cast is an error at
  the 0 itself: "the literal 0 is no pointer: write nullptr (design.md 4)" (an initializer, an
  assignment, a comparison either way round, an argument, a return, a template's return; a constant
  that is 0 too, "an integer 0 is no pointer"). `take(0)` with `take(int)` and `take(Entity*)` is
  take(int). So of overloads C++ finds alike, one that would take a literal 0 as a pointer is no
  candidate while another remains (2026-11-06, following from this rule): `take(0)` with take(u64)
  and take(const char*) is take(u64), where C++ finds the two conversions alike and the call
  ambiguous (String's `s.replace(0, 0, "<")` beside its iterator overload, constructors alike).
  One that C++ prefers, its pointer conversion the better (`show(0)` with show(const char*) and
  show(Big), Big made from a u64), stays the choice, and its 0 is the error above (where the
  converter writes nullptr), never another overload's call. Imported C keeps C's rule (its 0 and
  NULL are null pointers). Specs language/literal_zero_null_pointer,
  language/zero_literal_as_pointer_is_error, language/literal_zero_never_chooses_a_pointer_overload,
  language/literal_zero_for_a_better_pointer_overload_is_error.
- **A pointer converts without a cast only as in C++** (2026-10-31): to the same type with added
  qualifiers (at every level as [conv.qual] allows: `T* const*` to `const T* const*`), to `void*`
  from an object pointer, from a derived class to its base; a function or array to a pointer to it.
  Any other (`float*` to `int*`, `const int*` to `int*`, one function pointer type to another) is
  an error, in an initialisation, assignment, argument or return alike; imported C also converts
  to and from `void*`. Two pointers compare as their composite pointer type (a `const Derived*`
  against a `Base*` as a `const Base*`, moved to the base part). Placement new takes any object
  pointer as its place, a const one too (the library's containers of const elements construct
  into their storage so, where C++'s construct_at casts); over an object declared const, named
  visibly, it is an error (section 21). Specs:
  expressions/pointer_of_other_type_is_error, expressions/pointer_conversions_without_cast,
  expressions/function_pointer_of_other_type_is_error.
- **A C-variadic call is never format-checked** (2026-11-01, kovarex, section 14; it replaced
  "a printf-like call's format matches its arguments", decided 2026-10-31): kpp code prints through
  `print`/`toString`, the converter rewrites printf-family calls into them, and what stays variadic
  is a call at the C boundary or of a converted library's own variadic interface (trio, zlib's
  gzprintf, stb's writef), passed as written. Specs: language/variadic_with_own_format,
  language/variadic_calls_are_unchecked, language/printf_format_matching_arguments.
- **Grammar parseable without name lookup.** Three rules replace C++'s dependence on knowing
  what a name is:
  1. A statement that starts with a (qualified) name followed by `*`, `&`, `&&` chains and then
     an identifier is a declaration (`Foo* p;`); as an expression it would be useless anyway.
  2. `Type name(args)` is a declaration; `Type(args)` is an expression (no most vexing parse).
     Replaced by section 22 (2026-11-04): in a body `T x(...)` is a variable, at namespace and
     class scope a function; an object there is written `T x{args}`. (Until then, at namespace
     scope `T name(a, m);` with only names in the parentheses was an object when they were
     variables, decided by the resolver: spec language/object_declaration_from_variables, now
     written with braces.)
  3. `name<...>` is a template-id in type positions always. In expression positions it is one
     when one of its arguments can only be a type (a type keyword, `const`, a pointer, reference,
     array or function type, `decltype`, `typename`), whatever follows: `pi_v<float> * x`
     (decided 2026-10-29). Otherwise it is one when what follows the closing `>` cannot continue
     a comparison: `(`, `::`, `{`, `.`, `->`, `)`, `,`, `;`, `]`, `...`, `>`, or a binary operator
     that cannot start an operand (`==`, `!=`, `&&`, `||`, `?`, `/`, `%`, `|`, `^`, `=`, `<`,
     `<=`, `<<`, `<=>`). Anything else reads as comparisons (`a < b > c`, and `v<N> * 2` with a
     value as the argument): `(v<N>) * 2` is the template-id.
  4. `f(*p);` and `f(&x);` are calls. A parenthesised declarator is a declaration only when a
     parameter list or an array bound follows: `R (*f)(int)`, `T (&a)[3]`. Pointer and reference
     variables are written `T* p;`, never `T (*p);` (decided 2026-10-23, when `windows.h` showed
     that the C++ reading turns every `QueryPerformanceFrequency(&f);` into a declaration).
  5. `(f)(x)` is a call. `(T)(U)x` is two casts when `(U)` can only be a type (a builtin, a
     pointer, reference or const type, a template's instance): `(HANDLE)(long long)-1`, since no
     argument list is a bare type (2026-10-29). Imported C headers know their typedef names, so
     there `(HANDLE)(LONG_PTR)-1` is two casts as well.
  No `typename` or `template` disambiguators (as in the proposal).
- **Types.** `bool`, `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `char` (UTF-8 code unit),
  pointers, references, arrays, `const`. The fixed-size names are what kpp code writes; each is
  one of C++'s types under another name: `i8` is `signed char` (`int8_t`), `u8` `unsigned char`
  (`uint8_t`), `i16`/`u16` `short`, `i32`/`u32` `int` (`int32_t`), `i64`/`u64` `long long`
  (`int64_t`), `f32` `float`, `f64` `double`; `size_t`/`usize` is `u64` (the integer names since
  2026-10-24, `f32`/`f64` since 2026-11-01). So a converted `f(int)` beside `f(int32_t)` is one
  function, as in C++.
- **Distinct fixed types, deprecate long** (kovarex, 2026-11-01: "Distinct fixed types, deprecate
  long"; it decides Q1 of "Targets and hosts" below). The integer types are C++'s, with C++'s
  identities and overloads:
  - `char`, `signed char` and `unsigned char` are three types (`char` stays signed on every
    target); `__int8` is `char`, as MSVC has it. `char8_t`, `char16_t`, `char32_t` and `wchar_t`
    are types of their own, each with its own template instances: `wchar_t` is as wide and as
    signed as the target's C wchar_t (Q2) but never the `char16_t` or `char32_t` of its size
    (`Ty::isWchar`; until 2026-11-06 it was `char16_t` itself on Windows and shared `char32_t`'s
    instances elsewhere, E's limits battery). `char32_t` promotes to `unsigned int`, as `int`
    cannot hold its values, the other character types to `int` (C++ [conv.prom] 2,
    `TypeTable::promoted`: the checker, codegen and overload ranking alike).
  - `int`, `long` and `long long` are three types, `unsigned long` and its siblings likewise. So
    `f(int)` beside `f(long)` is two overloads, `X<int>` and `X<long>` two instances, `1L` a
    `long`, `int` to `long` an integral conversion (no promotion: a `short` promotes to `int`
    only), and the usual arithmetic conversions rank `long` between `int` and `long long` (on
    Windows `1u + 1L` is `unsigned long`, since `long` cannot hold every `unsigned int`).
  - `long` is C's type, for C interop only: imported headers, `extern "C"` declarations and blocks
    (a binding's aliases and C struct mirrors go there too), and the standard library's overloads
    that cover it (`toString(long)`, as `std::to_string` has). Its size follows the target's C ABI:
    32 bits on Windows, 64 on LP64 targets (`TypeTable::longBits`, set from the target).
    Elsewhere it is deprecated, as all of C's integer names are (section 13: `kpp
    --c-type-warnings`). kpp code writes `i32`/`i64` and `f64` for `long double`.

  Specs language/integer_types_are_distinct (the overloads, identities, literals and conversions
  of every C++ integer type, against clang's and MSVC's output for the same program) and
  language/c_type_names_outside_extern_c_warn. In the converted game (factorio-in-kpp, 2026-11-01, IR
  compared function by function with the names normalised) one call chose another overload, a fix:
  `toStr(int8_t)` (Util/ToStr.hpp, `std::to_string` of a signed char, which C++ promotes to int)
  called `toString(char)` and wrote a character; it now writes the number. Otherwise `signed char`
  parameters, which were `char`, are `int8` in the IR's names, and `Vector<signed char>` beside
  `Vector<char>` (two instances, as in C++) are new. The game writes `long` in 86 places outside
  extern "C", 40 of them in the DX11 COM interfaces.
- **Grammar rules' specs:** rule 1 language/declaration_statement_by_first_tokens, rule 2
  language/no_most_vexing_parse and language/object_declaration_from_variables, rule 3
  templates/variable_template_id_before_operator and language/template_id_or_comparison, rule 4
  language/call_statement_with_element_address, rule 5 language/cast_of_parenthesised_cast.
- **Declarations.** Functions, `struct`/`class` with methods, bases, constructors and
  destructors, `enum class` with methods, `using` aliases, global and local variables, `auto`.
  Simple templates (type parameters, full specialisation, `if constexpr`).
- **Statements and expressions.** The C++ set, with the proposal's rules: required `this.`,
  explicit by default, no implicit fallthrough, exhaustive `switch`, `?->` and `?:`.
- **C interop.** `extern "C"` declarations of functions and structs; those are the only
  externally linked names (nothing is mangled across kpp code, since everything is one program).
- **Not in v0:** exceptions, RTTI, virtual inheritance, `both_const`/`class_const`, typed
  unions (plain C unions exist, for interop), `accumulate`, coroutines (`co_return`, `co_yield` and
  `co_await` are compile errors in kpp source: until 2026-11-06 they parsed as `return` and as nothing).

**Backend plan.** The compiler writes textual LLVM IR for the whole program and runs the
bundled clang on it (`clang -O2 program.ll`). This needs no LLVM development libraries on the
machine (only the clang binaries exist here) and keeps every codegen decision in kpp's own
code; moving to the LLVM C++ API in-process is a later optimisation. Names in IR are the
qualified kpp names with parameter types, quoted, so there is no mangling scheme to design.

## Running and testing kpp code (proposed 2026-10-30, thread G; the open choices are kovarex's)
The goal: write some kpp and see it run, and write tests for kpp code, with no ceremony. The test
syntax is already decided (8.6: `test "name" { }`, `check`, `require`); what follows is the rest
of the mechanism, what is done, and the choices left to kovarex (marked **Q**).

### How to use it (works on master since 2026-10-30)
Run a program: `kpp --run` builds it (only what changed since the last run) and runs it with
the arguments after the file; the exit code is the program's.
```
// hello.kpp
int main(int argc, char** argv)
{
  println("hello, ", argc - 1, " arguments");
  return 0;
}
```
`compiler\build\kpp.exe --run hello.kpp a b` prints `hello, 2 arguments`.

Test code: write `test` blocks next to the code (or in a file of their own) and run them with
`kpp --test`, with the files or directories to build. A normal build skips the tests, and the
file's `main` is left out of a test build.
```
// square.kpp
int square(int x) { return x * x; }

test square
{
  check(square(3) == 9);
  check(square(-2) == 5);    // fails: square.kpp:7:3: check failed: square(-2) == 5 (4 == 5)
  require(square(0) == 0);   // a failed require ends the test
}
```
`compiler\build\kpp.exe --test square.kpp` prints each failure as above, `failed: square.kpp >
square` after a test that failed, and `1 test, 1 failed`. The exit code is 0 when every test
passed and 1 otherwise. Arguments after `--` go to the runner: `kpp --test src -- --filter
square` (a part of "file > name"; `*` matches anything), `-- --list`, and `-- --report -` (a JSON
line per test, for tools). Compiler options go before `--run` or `--test`: `--release`,
`--msvc-diagnostics`, `--no-server` (build without the compile server, which both use by
default). When the build reports an error, nothing runs. A small program or test takes about
0.15-0.17 s from the edit to the result, under 0.02 s when nothing changed.

### What exists (surveyed 2026-10-30 on 7f3e55c)
- **Running a snippet** is two commands: `kpp --exe out.exe file.kpp`, then `out.exe`. The
  program is built with the whole standard library (59 files; the front end takes 0.04 s of it).
- **The spec suite** (`compiler/tests/spec.cmd`, `run_spec.py`): one program per construct,
  compiled, run, stdout compared with `.expected`; headers `// spec:`, `// pending:`, `// error:`,
  and now `// run:` / `// exit:` (built and run in one step through `kpp --run`). It checks
  output, not assertions: it is the compiler's specification, not a test framework for kpp code.
- **`compiler/tests/run_tests.cmd`**: the same shape for `tests/run/*.kpp`, plus the access,
  determinism and incremental checks.
- **The converted game's unit tests** (`programs/factorio/tests.cmd`): UnitTest++ converted to kpp
  with the game, built into one `factorio-test.exe` with `--keep-going --stub-missing`, 9006
  tests passing (2026-10-29); `suites.py` runs one suite per process because a stub or a failed
  assertion ends the process.
- **The standard library has no tests of its own**; the spec suite's `library` area exercises it.
  `programs/kpplex` is a program with a timing script, no tests.
- **8.6 was not implemented**: `test "name" { }` was a parse error, and a parse error at namespace
  scope did not fail the build, which is why `kpp/tests_in_compiler` showed as passing. Since
  2026-10-30 the build fails (ad7671d, thread A; `language/namespace_scope_parse_error_fails_build`),
  the spec suite fails any snippet whose build printed an error whatever its exit code, and so do
  `--run` and `--test`.

### Latency today (2026-10-30, PC-PRACOVNA, other threads building at the same time: noisy)
The 10-line program (`fib` into a `Vector<int>`, `println`), PowerShell `Measure-Command`, six
rounds each, median (range):

| From the edit to the output | `kpp --exe`, then the program | `kpp --run` | `kpp --server --run` |
|---|---|---|---|
| file edited | 0.33 s (0.29-0.38) + 0.07 s | 0.47 s (0.44-0.78) | 0.61 s (0.53-0.81) |
| nothing changed | 0.30 s (0.28-0.33) + 0.07 s | 0.31 s (0.25-0.63) | 0.07 s (0.03-0.16) |

Where an edited build's time goes (`kpp --exe` without `--quiet`): the front end 0.03-0.05 s over
the 59 files, codegen 0.02 s, clang -O0 of the one changed module 0.12-0.27 s, the link
0.17-0.39 s: clang and the link are 80-90% of it. Two findings for the owners of the server
(thread A) and the link (thread B): the compile server's rebuild after an edit is slower than a
new process's (0.42-0.60 s against 0.29-0.38 s for `--exe`; clang and the link both slower in the
server), and a freshly linked 10-line executable takes 0.06-0.09 s to run (likely the virus
scanner reading the new file). A small test costs the same build: until 8.6 exists a test is a
snippet with asserts, 0.3-0.5 s from the edit to the result.

### (a) `kpp --run` (done 2026-10-30)
`kpp [options] --run SOURCE [program arguments]` builds SOURCE (a file or a directory) with the
options before `--run`, runs it with the arguments after SOURCE, and returns the program's exit
code; when the build fails, the compiler's errors and exit code. The executable and its module
objects live in `%TEMP%\kpp-run\<name>-<hash of the source's path and the options>\`, so the next
run rebuilds only the changed module and nothing is written next to the source. Every thread's runs
share that cache, which came to 734 directories and 191,078 files on 2026-11-03, when the kernel's
record of each cached file filled its pool: since then a run builds with `--modules 4` unless its
options name `--modules` (some 15 files a directory, against some 385 at kpp's default), and the
cache keeps the 100 most recently used directories. A run holds its directory's `lock` file open,
deletable by no one else, from before its build until its program ends, and the time of last use is
that file's; the cleanup, at the end of each run, deletes a directory only while it holds its lock
file alone (a run that finds it held waits, then makes the directory again), and one from before the
lock file only once it has been unchanged for an hour (tests\runcache.ps1). `--no-run` among the
options builds only and names the executable (`kpp --run: built PATH (not run: --no-run)`; NAME.exe,
NAME alone for Linux and FreeBSD, as a project's build/CONFIGURATION/NAME is); a
program for another `--target`, which this machine does not run, is an error without it, before
any build (2026-11-04: CI builds the libraries' tests for FreeBSD with `kpp --target freebsd
--no-run --test` and runs them in a FreeBSD VM). The build is a
kpp process of its own (a build ends its process when done), a client of the compile server: it
starts the server when none runs, and the server stops after an hour idle (one per kpp binary,
directory and argument list; 25-48 MB each for small programs). `--no-server` among the options
builds in that process instead, which the spec suite does so that it leaves no servers behind.
At first the server lost after an edit (above); since 6812a62 (thread A: the server's tools share
one console) it wins every time, so it is the default since the same day. Paired, interleaved, 8
rounds each, the 10-line program and the small test of (b), 2026-10-30 on a quiet machine:

| From the edit to the result (median, range) | default (server) | `--no-server` |
|---|---|---|
| `--run`, file edited | 0.147 s (0.143-0.156) | 0.174 s (0.171-0.185) |
| `--run`, nothing changed | 0.018 s (0.018-0.021) | 0.173 s (0.171-0.183) |
| `--test`, file edited | 0.168 s (0.165-0.181) | 0.179 s (0.176-0.185) |
| `--test`, nothing changed | 0.018 s (0.018-0.019) | 0.178 s (0.173-0.181) |

Spec snippet: `kpp/run_command`.
**Q1, decided (kovarex, 2026-11-06): no implicit run.** `kpp --run hello.kpp` is the form for a
snippet, `kpp new` and plain `kpp` for a real program, and no build.kpp is generated for a single
file. A bare file would clash with several sources (`kpp a.kpp b.kpp` keeps compiling both), and a
file starting with `#!` run as a script is not planned. `kpp hello.kpp` stays the front end's
statistics pass. `kpp --run hello.kpp a b` means the same in a project's directory (docs/build.md,
"The commands").

### (b) Native tests: the rest of 8.6
- **Declaration** as 8.6: `test name { ... }` at namespace scope or in a class. The name is a
  string (an identifier since 8.7); two tests of one name in one program are a compile error (not yet). A test's full name
  is its file's path as the build names it (later its package, "Code packages") and its name:
  `core/Vector.kpp > push_back grows`.
- **Checks.** `check(expr)` records a failure and continues; `require(expr)` records it and ends
  the test. The message is in the compiler's error format, so editors jump to it:
  `file:line:column: check failed: v.size() == 3 (2 == 3)`: the expression text and, for a
  comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`), both operands' values when `toString` or a
  `str()` method can say them (the text alone otherwise). The same values then go into `assert` messages (7.2's "operand
  values: later"). Inside a test, a failing `assert` or `release_assert` fails the test (the
  runner's `kpp_assert_failed` ends the test by throwing, section 9).
- **One command.** `kpp [options] --test ROOTS... [-- runner arguments]` builds the tests under
  ROOTS into one test executable (in `%TEMP%\kpp-run\`, as `--run` does) and runs it; `--tests`
  (8.6) stays the build-only switch, with `--exe`, for a binary to keep or debug. In a test build
  the program's `main` is left out and the runner is the entry point. The standard library's tests
  run only when a root names it. Without `--tests` or `--test`, the parser skips test declarations:
  they cost nothing.
- **The runner** (its arguments after `--`, or on the test executable): `--filter TEXT` (a
  substring of the full name, `*` matching anything; repeatable), `--list` (the tests, no run),
  `--report FILE` (below), `--jobs N` and `--durations FILE` (below; the test executable starts its processes). One line per failure and `failed: <full name>`
  after a failed test, then a summary: `12 tests, 1 failed`. The exit code is 0 when all pass, 1 when one failed, the compiler's when the
  build failed. **Q2 is settled by 8.7** (2026-11-06): `--filter` replaces 8.6's `--run-test <name or
  file>`; it matches the full name, and `--suite`, `--skip-suite` and `--tag` select beside it.
- **Isolation and parallelism.** **Q3 is settled by 8.7** (2026-11-06; test-suites.md (d), and its
  question 2 decides how `--exit-on-failure` acts under `--jobs`): by default the tests run in one
  process, one after another (the fastest; a crash ends the run and is reported as that test's);
  `--jobs N` schedules suites, or with `--durations` tests, over N processes of the same executable, and a crash fails only
  its test (the process is started again for the rest of its share), which is what `suites.py` does
  by hand for the converted UnitTest++. No threads inside one process: tests may use globals. A suite
  does not wait for its dependencies, as Factorio's `--fork` does not: the processes share nothing,
  and the output is said in the suites' order, so `(after failures in: A)` and the root failures
  come out the same; with `--exit-on-failure` it waits, so that a failure in a dependency stops the
  run before its dependents (2026-11-07: waiting always made the game's tests take the longest
  chain, ManualBuilding to Simulations, 203 of 234 s of one run). Without `--durations` whole suites
  go, the most tests first. With `--durations FILE`, an earlier run's `--report` (each line's
  `"test"`, `"suite"` and `"seconds"`), each test goes on its own, the longest first, as Factorio's
  `--fork` splits by its cached test times (ForkLogic::buildForkTasksFromCachedRuntimes), so a long
  suite's tests run in several processes at once; a test the file does not name counts its suite's
  seconds per test the file names, else the file's seconds per test (2026-11-07: whole suites stopped
  at Simulations, the longest; by test the floor is the longest test, Migrations.EntityRemovals).
  With `--exit-on-failure` suites stay whole, the longest first by the file's seconds for their tests
  plus their longest chain of dependents. `tests.cmd --shards N` keeps the report of the last run that
  passed as the next one's `--durations`. Measured on the game's 9,501 tests (2026-11-07, factorio-in-kpp
  5176c864e, two binaries of one tree, master's runner and per test, in a quiet window, interleaved ABCCBA,
  each run's load under 4% before it, `--durations` an earlier run's report): at 16, per test 34.3 and
  34.3 s against whole suites (`--durations`) 60.6 and 60.9 s and suites.py `--shards 16` 67.0 and 66.2 s;
  at 32, per test 28.4 and 28.3 s against 63.8 and 63.3 s and 60.5 and 60.8 s. At 16 it is the even
  share, 448 s of tests over 16 processes plus a worker's setup (simulated 33.0 s, no tail); at 32 each
  test runs on an SMT thread (655 s of tests in all, Migrations.EntityRemovals 15.5 s against 8.6 s),
  so the floor is that test. Measured before tests went on their own on the game's 9,501 tests
  (2026-11-07, factorio-in-kpp 9dae92d21, one binary, interleaved, each run after 20 s of load under
  10%): `--jobs 16 --durations` 61.5 and 61.2 s against suites.py `--shards 16` 67.8 and 69.8 s;
  at 32, 65.1 and 64.2 s against 61.6 and 64.8 s; without `--durations` (most tests first) 59.0 s at
  16. Both are at the floor: Simulations alone takes 54 s, and a worker's `testsSetup` about 5 s
  (C's 92.7 and 120.1 s at 16 waited for the dependencies). How: the N workers start once, and before
  the first process's `testsSetup`, since a program's may take seconds; the first process runs
  `testsSetup` too (the generators need it), selects as without `--jobs`, runs no test, and gives
  each idle worker the first suite in that order that may start, over a pipe. The workers send back what each test said, and the first
  process says it in the order of the suites, `failed:` lines, report and summary, so the output is
  the same whichever finished first (what a test prints itself goes straight to the shared stdout).
  A crash is `failed: S.t (it crashed: exit code 3)`, `"status":"crash"` in the report; a process that
  ends between tests with a suite in hand has nothing to blame, and the rest of the suite goes on.
  `testWorker()` is the worker's number from 1, 0 in the first process and without `--jobs`, for what
  processes must not share, such as a directory a hook writes to.
- **The converted UnitTest++.** Q4 is answered (kovarex, 2026-11-06): the converter (thread C)
  turns `SUITE`, `TEST`, the fixtures, `DEPENDENCY`, the generators and the check macros into
  native suites, tests and `require`, and the game's tests run under the native runner with its
  hooks (8.7; the mapping in [test-suites.md](test-suites.md)). Until 2026-11-06 they stayed UnitTest++,
  converted as they are.
- **Spec snippets stay** output comparisons: they specify the compiler (the pass/pending table);
  tests check kpp code. Each test construct (declaration, check values, require, the failure
  format, filtering) gets its spec snippet first, as every feature does.

### (c) Visual Studio (thread E) and the IDE (thread F)
Both drive the same command line and read the same machine-readable report; neither parses the
human output.
- **Discovery**: `kpp --test ROOTS -- --list --report -` writes one JSON object per line,
  `{"test":"core/Vector.kpp > push_back grows","file":"C:/.../Vector.kpp","line":120}`. Listing
  needs only the front end (no codegen, no link), so it can run on every save; today `--list`
  runs in the built test executable, and a listing from the front end alone comes later.
- **Running**: `--report FILE` (`-` for stdout) writes per test `{"test":...,"file":...,"line":...,
  "status":"pass"|"fail"|"crash","seconds":...,"failures":[{"file":...,"line":...,"column":...,
  "kind":"check"|"require"|"assert","expression":"v.size() == 3","values":["2","3"]}],
  "output":"..."}` and last `{"summary":{"tests":12,"failed":1,"seconds":0.004}}`. Running one
  test from an editor's gutter is `--filter` with its full name. Built on 2026-10-30: everything
  above except `seconds`, `output` and the `crash` status; `kind` may also be `exception` (an
  exception left the test). Spec snippet kpp/test_report.
- **Visual Studio**: the Test Explorer needs a test adapter (thread E's extension) running the two
  commands above; a failure's file, line and column open the source. A project's "Start Without
  Debugging" is `kpp --run`; debugging builds with `--exe` (or `--tests --exe`) and starts the
  executable under the debugger. Compile errors already reach the Error List through
  `--msvc-diagnostics` (456eda1, docs/build.md); check failures are in the compiler's error
  format, so the same option shows them there without the adapter.
- **The IDE (thread F)**: the same JSON; with the persistent compiler, the IDE's server answers
  `--list` from the build it already holds.

### How it is built (done 2026-10-30, thread G)
1. `--run` and `--test`: `compiler/src/run.cpp`; `main.cpp` hands them over before anything else.
2. The parser (A's file, `parseTest`): with `config::tests` (set by `--tests`), `test "name" { }`
   becomes a function `void __kpp_test_<file hash>_<n>()` (static in a class) and a variable
   `int __kpp_test_registered_<...> = __kppRegisterTest(&__kpp_test_<...>, "name");`, whose
   initializer registers the test at startup. Its caller location is the test's, which gives the
   file and line. So there is no table to generate, and a test in a class or a namespace needs
   no qualified name. In a test's body, `check(cond);` and `require(cond);` are assert statements
   (`AssertStmt::testKind` 1 and 2). A comparison `a OP b` becomes `__kppEqual(a, b)` (or
   `NotEqual`, `Less`, `LessEqual`, `Greater`, `GreaterEqual`); when the comparison fails, the
   call keeps the operands' text for the message. The program's `main` is renamed. Without
   `config::tests`, the parser skips the declaration.
3. Codegen (B's file, a dozen lines): a failing check calls `kpp_check_failed(file, line, column,
   text, kind)` and goes on. For a require, the call may throw (an invoke), so the test's objects
   are destroyed.
4. The runner is kpp source the driver adds as one more file, `<tests>.kpp`
   (`compiler/src/tests.cpp`), as it adds `<config>`: its `main`, `kpp_check_failed`, a
   `kpp_assert_failed` that fails the running test, and the comparison templates. The standard
   library is untouched.
5. Not yet: a compile error for two tests of one name, leaving the standard library's
   tests out of a program's run (it has none yet).

Latency of a small test (one `test` with a loop and two checks, `kpp --test fib.kpp`, six rounds
on 2026-10-30, other threads building): 0.34 s median from the edit to the summary (0.32-0.69 s),
0.27 s with nothing changed (0.25-0.45 s). Again after `--test` began reading the build's output
for errors, the machine quieter: 0.22 s median after an edit (0.20-0.40 s), 0.19 s with nothing
changed (0.16-0.26 s). Cold (the cached objects deleted before each round), on a heavily loaded
machine: 2.3 s median (1.5-2.8 s); in the same rounds, warm after an edit 0.91 s (0.46-1.69 s) and
warm unchanged 0.87 s (0.31-1.36 s). The load decides more than the build does; a cold run builds
the standard library's modules once.

## Targets and hosts (decided 2026-10-31; the design proposed by thread B, Q1 to Q6 kovarex's)

**Decision (kovarex, 2026-10-31):** "kpp will have to work obviously in all reasonable platforms, so
windows/mac/linux is minimum." kpp.net, the website and library registry written in kpp, is hosted
on NearlyFreeSpeech.NET, which runs FreeBSD. That decision has two axes:

- **Targets:** what kpp emits and links for. The minimum is Windows x64 (the default), Linux x86-64
  and arm64, macOS arm64 (Apple Silicon) and x86-64, and FreeBSD x86-64 for kpp.net.
- **Hosts:** where kpp itself (C++20) builds and runs. That is Windows and Linux (done 2026-11-06,
  stage 5 below); macOS as a host is under consideration (below).

On both axes Windows stays the default, with its output unchanged: the game, the tests, and the IR
byte for byte unless a difference is shown to be a fix.

**This stage (kovarex, 2026-11-06; "Roadmap" has the stages):** macOS as a host moves to under
consideration, not decided. Building every target, macOS included, on Windows and Linux is enough
for this stage, so macOS as a target stays planned. What this section says of a Mac host (section 5,
`xcrun`, kpp hosted on macOS) waits for that decision.

**Order (proposed):**
1. Linux x86-64: the most tooling, CI runners, and the closest to FreeBSD. It is also testable on
   this machine (WSL).
2. FreeBSD x86-64.
3. macOS arm64, with Linux arm64 alongside: one AArch64 lowering serves both.
4. macOS x86-64.

| Target | Triple | Object, linker | C calling convention | Exceptions | Oldest system (Q6) | Tested on |
|---|---|---|---|---|---|---|
| Windows x64 (default) | `x86_64-pc-windows-msvc` | COFF, lld-link | MS x64 | SEH (today's) | Windows 10 | this machine, CI |
| Linux x86-64 | `x86_64-unknown-linux-gnu` | ELF, ld.lld | SysV AMD64 | Itanium tables, kpp's personality | glibc 2.31 (Debian 11, Ubuntu 20.04) | WSL here, CI ubuntu |
| FreeBSD x86-64 | `x86_64-unknown-freebsd14` | ELF, ld.lld | SysV AMD64 | the same | FreeBSD 14 | NFS, or a FreeBSD VM (Q3) |
| Linux arm64 | `aarch64-unknown-linux-gnu` | ELF, ld.lld | AAPCS64 | the same | glibc 2.31 | CI ubuntu arm64, qemu-user in WSL |
| macOS arm64 | `arm64-apple-macosx11.0.0` | Mach-O, ld64 (or ld64.lld) | Apple's AAPCS64 | the same, compact unwind | macOS 11.0 | CI macOS arm64 (Q5) |
| macOS x86-64 | `x86_64-apple-macosx10.15.0` | Mach-O, ld64 | SysV AMD64 | the same | macOS 10.15 | CI macOS x86-64 (Q5) |

### What exists (surveyed 2026-10-31 on a52cb2a8)

- **Codegen is mostly target-neutral.** It writes one triple and data layout
  (`x86_64-pc-windows-msvc`, `e-m:w-...`). A hello world's IR with only those two strings changed
  to `x86_64-unknown-linux-gnu` and `e-m:e-...` compiles with VS's clang 19, links with gcc and
  prints its line under WSL (Ubuntu, glibc 2.39).
- **What is Windows-only in the emitted code:**
  - exceptions: SEH funclets, `__C_specific_handler`, `RaiseException`, a filter function per
    `try`;
  - the runtime's stderr through `__acrt_iob_func` (8 places);
  - one `dllimport`;
  - the CodeView module flag;
  - the `wmain` entry;
  - COMDATs on every shared definition (`$x = comdat any`), which ELF has and Mach-O does not;
  - the link line: lld-link's `/OPT`, `/ENTRY`, `/INCLUDE`, `legacy_stdio_definitions` and the
    Windows libraries (main.cpp `linkFlags`).
- **The front end fixes Windows' C types:** `long` is 32 bits and `wchar_t` 16
  (resolver.cpp `case Kw::wchar_t_`, "Windows LLP64"), and the printf format check assumes LLP64.
- **The C import** runs `clang -E` with clang's default target, which reads MSVC's and the UCRT's
  headers.
- **The standard library** calls Win32 through `stdlib/core/Win32.kpp` (namespace `win32`) and
  the UCRT's C functions. Thread, Time, Filesystem, Error and SharedPtr use them. Its 32-bit
  atomics are declared with `long`. Filesystem and WString are UTF-16 (wchar_t). The library has
  93 lines with a plain `long` and 120 with `wchar_t`; the game (kpp2, 4,126 files) has 232 and
  394.
- **Classes by value:** every call passes a class argument as a pointer to a copy and returns a
  class through a hidden slot, extern "C" functions included. For C functions that is MS x64's
  rule only above 8 bytes, where MS x64 passes 1, 2, 4 and 8-byte structs in a register. That gap
  exists on Windows today.
- **kpp as a host program** uses Win32 in its sources (lines matching Win32 APIs):
  - server.cpp 49: named pipes and processes;
  - codegen.cpp 22: threads with 1 GB stacks, an SEH guard against stack overflow,
    `TerminateProcess`;
  - lsp.cpp 16, convert.cpp 14, run.cpp 13, project.cpp 11, resolver.cpp 6, parser.cpp 5, main.cpp
    4 and source.cpp 4. source.cpp finds clang through vswhere.

  CMakeLists.txt has MSVC flags and `/STACK`. The test entry points are `run_tests.cmd` and
  `spec.cmd`; the other tests are PowerShell scripts and the spec runner is Python.
- **The toolchain on this machine:** VS's clang 19.1.5 ships `ld.lld` and `ld64.lld`. WSL2 runs
  Ubuntu with glibc 2.39, gcc and clang: Linux binaries can be run and tested here.

### 1. The target model

- **`--target <name or triple>`** with the short names `windows`, `linux`, `linux-arm64`,
  `freebsd`, `macos` (arm64), `macos-x64`. The default is the host's own platform, which is
  `windows` on Windows. The driver keeps one Target record:
  - triple and data layout;
  - the object format;
  - the C type sizes and plain `char`'s signedness;
  - the C calling convention and the exception model;
  - the debug-info format;
  - the linker and its flags, and the sysroot or SDK;
  - the standard library's platform directories.

  The target is part of the compile server's key, the C import cache's key and the object hashes.
- **In build.kpp:** `project.target = "linux";`, or `exe.target` for one executable, and on the
  command line `kpp --target linux`. The program sees it as `config::os` (`windows`, `linux`,
  `freebsd`, `macos`), `config::arch` (`x86_64`, `arm64`) and `config::posix`, the same way
  `config::debug` crosses over (section 12).
- **What differs, and what this proposes:**

| | Windows (today) | ELF, Mach-O, AArch64 | Proposal |
|---|---|---|---|
| Calls between kpp functions | kpp's convention: a class through a pointer to a copy, the result slot after `this` | the same | Unchanged everywhere. Scalars get the platform's convention from LLVM's `ccc` and the triple, with no codegen change. A class result keeps its slot after `this`. On AArch64, LLVM's `sret` attribute puts the slot in x8 for C functions only. |
| `extern "C"` calls with a struct by value | the pointer to a copy (wrong at 8 bytes or less) | SysV: classified into INTEGER/SSE registers up to 16 bytes, larger on the stack (`byval`). AAPCS64: up to 16 bytes in x0-x7, a homogeneous float aggregate of up to 4 members in v0-v3, larger through a pointer to a copy, a result in memory through x8. | A C ABI lowering per target in codegen (MS x64, SysV, AAPCS64), for calls to C and for kpp functions C calls back. It fixes Windows' small-struct gap too. Specs for each size class on every target. |
| Varargs | LLVM lowers the calls; `va_list` is `char*` | SysV: `__va_list_tag[1]`. Linux AArch64: a 32-byte struct. Apple arm64: every variadic argument on the stack, `va_list` is `char*`. | LLVM lowers the calls for each triple. `va_list` comes from the target's headers through the C import. kpp defines no C variadic functions. |
| `long` | 32 bits | 64 bits in C (LP64) | Q1 below (decided): C's size on each target. |
| `wchar_t` | 16 bits, UTF-16 | 32 bits | Q2 below. |
| plain `char` | signed | unsigned on Linux AArch64, signed on Apple arm64 and x86-64 | kpp's `char` stays signed on every target (kpp's meaning does not change with the target). An imported C `char` is the same type; a C function that relies on unsigned `char` sees the same bits. |
| `long double` | = double | x87 80-bit on x86-64 SysV, 128-bit on Linux AArch64, = double on Apple arm64 | kpp's stays double. An imported C function whose signature has `long double` (by value, through a pointer or a typedef) is an error where the C type differs, at each call and each use of its address (codegen's useFunction, 2026-11-05; the call was made as double(double) until then, silently). So is a C function that a kpp declaration names (`extern "C"`) and no kpp body defines (2026-11-09: such a call was silent until then, `fabsl(-2.5)` -2.5 on Linux). The library's strtold is kpp's own, strtod's value, on every target. |
| Bit fields | MSVC's rules | SysV's (Itanium): a unit straddles less, and types of different sizes may share one | The target's C rules for every class (a struct shared with C must match). Windows is unchanged. Specs on each target. Done 2026-11-05 (section 9's bit field paragraph); a polymorphic base's tail padding stays unused (Itanium reuses it for C++, which kpp does not share classes with). |
| Memory ordering | x86's: stores are seen in order | AArch64 is weakly ordered | The library's atomics are LLVM atomics with explicit orderings, so they hold. Lock-free code that relies on x86's ordering through plain loads and stores is a bug on AArch64. The library is audited for it in the AArch64 stage. |
| Symbol names | quoted IR names (`@"twice(int32)"`), C names unmangled | the same names are valid in ELF and Mach-O; Mach-O prefixes `_`, which LLVM adds through the data layout's `m:o` | Unchanged in the IR. No `dllimport` off Windows. |
| COMDAT | `linkonce_odr` with `comdat any` | ELF has COMDATs. Mach-O has none (LLVM rejects them) and coalesces weak definitions instead. | Emitted on Windows and ELF, left out on Mach-O. The shared definitions stay `linkonce_odr` or `weak_odr`. |
| TLS | `thread_local` globals, lazily built per thread | ELF TLS; Mach-O's TLV descriptors (`__tlv_bootstrap`); both through LLVM | Unchanged. |
| Global constructors | `llvm.global_ctors` | `.init_array` on ELF, `__mod_init_func` on Mach-O, from the same IR | Unchanged. Destructors at exit: kpp_atexit, on Mach-O `__cxa_atexit` as on ELF. |
| Entry | `main`, or `wmain` (the game) | `main`, with UTF-8 `argv` | `wmain` is Windows-only. Done 2026-11-06: on Windows a `main` that takes arguments is entered through a generated `wmain`, which makes the C runtime's wide `argv` UTF-8 (WideCharToMultiByte; the narrow one is the ANSI code page's), the link naming `wmainCRTStartup`; an argument holding a lone surrogate, which has no UTF-8 form, ends the program before `main` with a message naming it (as a path holding one throws: kovarex, 2026-11-06). `getenv` is UTF-8 too: on Windows it is the stdlib's, not the C runtime's (FilesWindows.kpp; an import of stdlib.h leaves it to the stdlib, as any library function of the same parameters), reading the wide environment (`_wgetenv`) into a UTF-8 copy kept until the variable changes; a value holding a lone surrogate throws SystemError, as a path does. C code linked in (`--lib`) keeps the C runtime's, and a program's own `extern "C"` declaration of getenv is a link error on Windows (the stdlib's definition takes its symbol, which the C runtime has). `kpp --run` passes the program's arguments on as UTF-8 too. A program with `main` and `wmain` is entered at `main`, as the C++ linkers choose. `main` is C++'s `int main()` or `int main(int argc, char** argv)` (`wmain`'s `wchar_t**`) and nothing else: another form (one parameter, the environment's third, a `void` result) is an error, as is an `extern "C"` main; a namespace's `main` is an ordinary function. |
| Debug info | CodeView | DWARF (on macOS, `dsymutil` collects it into a .dSYM) | One module flag per target. |
| Signing | none needed | macOS arm64 runs only signed code | ld64 and ld64.lld sign ad hoc on arm64 by default. A program shipped to others needs a Developer ID signature and notarization (packaging, thread E). |
| The runtime's stderr and errno | `__acrt_iob_func(2)`, `_errno()` | `write(2, ...)`; `__errno_location()` on glibc, `__error()` on FreeBSD and macOS | The runtime's few calls go through the platform layer (3). |

- **Q1, `long`: decided** (kovarex, 2026-11-01, "Distinct fixed types, deprecate long"; the kpp v0
  section's "Types" has the rule): `long` is a distinct type with C++'s overload behaviour, for C
  interop only, as wide as the target's C `long` (option (b) for its size), and deprecated in kpp
  code, which writes the fixed-size names. The options as they were put:

  On LP64 targets (every target but Windows), C's `long` is 64 bits:
  - **(a) kpp's `long` stays 32 bits everywhere.** C's `long` follows the target: the C import
    types an imported header's `long` as 64 bits on LP64, and hand-written bindings say `CLong` or
    `CULong` (stdlib/clib: int32 on Windows, int64 elsewhere). A kpp program means the same on
    every target, and the library's 93 lines and the game's 232 stay right. The cost is that kpp's
    `long` differs from C's off Windows, so code ported from POSIX C must use `CLong` for C's.
  - **(b) `long` follows the target's C.** This matches C and C++. Code that meant 32 bits (the
    library's atomics, MSVC code like the game's) changes meaning off Windows, so the converter
    would map MSVC's `long` to `int32_t`, and portable code must not say `long`.
  - **(c) No `long` in kpp.** It is an error, leaving int, int64_t and the rest, plus `CLong` for C.
    The converter maps it, and so must the 93 library lines.

  **Recommended: (a).** kpp's types mean one thing everywhere, and only the C boundary follows the
  platform, which is where the difference lives. It is needed before the first target's C import:
  `ssize_t`, `off_t` and `time_t` are `long` in glibc's, FreeBSD's and Apple's headers.
- **Q2, `wchar_t`: decided** (kovarex, 2026-11-01): (a), the target's C size; UTF-8 String is the portable text.
  - **(a)** `wchar_t` is the target's C `wchar_t` (16 or 32 bits), for interop only.
    The library's portable text is UTF-8 (String). The UTF-16 APIs (WString, conversions) exist on
    Windows only, and a path's native form is per platform, as in std::filesystem: UTF-16 on
    Windows, UTF-8 bytes elsewhere.
  - **(b)** `wchar_t` is 16 bits everywhere. C's `wchar_t` then differs off Windows, and kpp code
    that passes one to a C function there is wrong.

  The game's 394 lines are Windows code.

### 2. Exceptions: one front end, two lowerings

The semantics of section 9 hold on every target:
- The search is two-phase: the handler is found before anything unwinds.
- A handler matches through the thrown type's descriptor and its bases, with the base offset
  applied.
- The objects between the throw and the handler are destroyed in order.
- A destructor that throws while unwinding terminates.
- An uncaught exception prints its type and message from the search, the throw site on the stack,
  then calls the terminate handler and aborts.
- Descriptors, the record a handler owns, `throw;`, and the current-exception stack are the same.

- **Windows keeps SEH**, unchanged.
- **Every other target uses the Itanium model's zero-cost tables.** The unwind information is
  `.eh_frame` on ELF, and compact unwind with `__eh_frame` on Mach-O; LLVM writes both. A call
  that may throw is an `invoke` whose unwind edge goes to a `landingpad`. LLVM writes the LSDA from
  the landing pads' clauses: the call-site table, the action table, and a type table of pointers to
  kpp's type descriptors.
  - **The personality** is kpp's own, `__kpp_personality_v0`, a runtime function codegen emits as
    IR text (as it emits `kpp_throw` today). It works over the platform unwinder's `_Unwind_*`
    API: libgcc_s on Linux and in FreeBSD's base system, and libunwind in macOS's libSystem.
    - Phase 1 walks the LSDA. A catch clause matches when `kpp_eh_offset` (the base search SEH's
      filters use today) finds the clause's descriptor among the thrown type's bases.
    - Phase 2 installs the landing pad with the record and the clause's selector.
    - **Two phases, the types matched in phase 1** (kovarex, 2026-11-03, against a proposal to match
      in the landing pads and raise again from each one): "we need to be able to report crashes
      like that". An exception nobody catches, and one leaving a noexcept function, end the program
      through the terminate handler from the search itself, with the throw site still on the stack
      (for a crash report, and later a dump), and a debugger sees one throw. C++ leaves both to the
      implementation ([except.terminate]); kpp unwinds nothing before terminate. (Windows today
      unwinds first: its `@main` catches everything and reports in the handler. Reporting from the
      SEH filter instead, during the search, is the same change there, and changes Windows' IR.)
    - **noexcept** (2026-11-03): an exception leaving a noexcept function terminates, on every
      target. Before this, it passed through, its frames unwound and the caller's handler took it,
      Windows included (spec language/noexcept_violation_terminates_at_the_throw, pending).
  - **`throw`** allocates `{_Unwind_Exception header; object; descriptor; destructor}` and calls
    `_Unwind_RaiseException`.
  - **Not `__gxx_personality_v0`:** that would need C++ `std::type_info` objects and a C++ runtime's
    `__cxa_*` (libcxxrt, libsupc++, libc++abi). kpp's personality keeps kpp's descriptors and links
    only the unwinder. On x86-64 and AArch64 the personality is the same code.
- **The lowering is split along one interface.** codegen's EH has three parts:
  1. Which calls become invokes, and what each unwinds through (the destruction state and the
     innermost `try`). This part is model-neutral and stays as it is.
  2. The pads:
     - SEH: `catchswitch`/`catchpad`/`cleanuppad` funclets, filter functions, and destructors
       through `kpp_destroy_unwinding` (a funclet holds no invoke).
     - Itanium: one landing pad per (destruction state, `try`). It runs
       `landingpad {ptr, i32} cleanup catch ptr @desc...`, then the cleanups inline (a destructor
       that may throw is an invoke to a terminate pad), then dispatches on the selector
       (`llvm.eh.typeid.for`) to the handlers, the enclosing `try`'s dispatch, or `resume`.
  3. Entering and leaving a handler: `kpp_eh_begin`, `kpp_eh_end` and `kpp_eh_rethrow` on the
     same record, with the unwind header in front of it.

  The plan is an `EHLowering` interface in the Emitter (the pad for a state, begin and end a
  handler, rethrow, the throw call), with SEH's as today's code moved and Itanium's new. The
  Windows IR is checked byte-identical by the spec suite's IR and the game's module hashes before
  and after.

### 3. The standard library's platform layer (with thread C)

- **The portable files keep the kpp API.** Thread, Time, Filesystem, Error, the streams, and the
  new sockets call a `platform::` namespace of thin functions:
  - threads (create, join, id, sleep, yield, the processor count);
  - a mutex and a condition variable as storage of the target's size;
  - the monotonic and system clocks;
  - files: open, read, write, seek, close, stat, directory iteration, rename, remove;
  - the environment;
  - processes: spawn, wait, exit;
  - sockets: socket, bind, listen, accept, connect, send, recv, poll, close, getaddrinfo;
  - the last error and its message.
- **Implementations:**
  - `stdlib/platform/windows/` holds today's Win32 and UCRT code, moved.
  - `stdlib/platform/posix/` uses pthreads, `clock_gettime`, open/read/opendir, `environ`,
    `posix_spawn` and BSD sockets.
  - `stdlib/platform/linux/`, `freebsd/` and `macos/` hold the differences: the errno accessor,
    and the layouts and constants of `struct stat`, `dirent`, `sockaddr`, `pthread_mutex_t` (40
    bytes on glibc x86-64, 8 on FreeBSD, 64 on macOS), `O_*`, `SOCK_*` and the errno values. These
    come from the C import of the target's own headers where the importer manages, and are
    hand-written where it does not.
- **How a file picks its platform:** by directory. The driver adds `stdlib/platform/<os>` and its
  parents (`posix` for linux, freebsd and macos) for the target. There are no file-level
  conditionals. A small difference inside portable code is `if constexpr (config::os ==
  Os::windows)`, and an API that exists on one platform only (UTF-16 paths) is
  `conditional(config::os == Os::windows)`.
- **Order:** sockets and threads first, which the web server needs (thread I collects the
  website's needs). Then files, time, the environment and processes.
- **As built (thread C, 2026-11-03):** the platform files are picked by the file-level
  `requires config::platform == ...` kpp already had (stage 1's split, below), not by
  directories the driver adds: `stdlib/platform/PlatformWindows.kpp` (`== Platform::Windows`),
  `PlatformPosix.kpp` (`!= Platform::Windows`), `PlatformLinux.kpp` and `PlatformFreeBSD.kpp`.
  `platform::` so far:
  - `steadyNanoseconds`, `systemTicks` (100 ns since 1970), `fastCounter` (RandomDevice's seed),
    `sleepFor`, `yield`: QueryPerformanceCounter, GetSystemTimePreciseAsFileTime, Sleep and
    SwitchToThread on Windows; `clock_gettime`, `nanosleep` and `sched_yield` elsewhere
    (CLOCK_MONOTONIC is 1 on Linux, 4 on FreeBSD).
  - `errnoLocation`: `_errno()`, `__errno_location()`, `__error()`.
  - `setTerminate` is kpp's own on every target since 2026-11-04 (core/Error.kpp, an Atomic, one for
    the program; it was the UCRT's per-thread `set_terminate` on Windows). The exception runtime
    calls it through `kpp_terminate_handler`.
  - Threads and locks (Thread.kpp, SharedPtr.kpp): `Lock`, `SharedLock` and `Condition`, and
    `startThread`, `joinThread`, `detachThread`, `currentThreadId`, `processorCount`. On Windows
    Lock and SharedLock are one SRW lock (one pointer, as before), Condition a
    CONDITION_VARIABLE. Elsewhere they are a `pthread_mutex_t`, a `pthread_rwlock_t` and a
    `pthread_cond_t` of the C library's size (glibc 40, 56 and 48 bytes; FreeBSD a pointer each),
    zeroed, which both C libraries take for the static initializers, and destroyed with their
    owner (FreeBSD allocates them on first use). A timed wait's deadline is on the realtime clock,
    pthread_cond_t's default. A thread's id is its `pthread_t` (`pthread_self`), the processor
    count `sysconf(_SC_NPROCESSORS_ONLN)`.
  - Files (Filesystem.kpp; `stdlib/platform/FilesWindows.kpp`, `FilesPosix.kpp`): a file's
    information (`FileInfo`: type, permissions, size, write time), a directory reader, the
    absolute and canonical paths, the current and temporary directories, space, and creating,
    removing, renaming, copying and the permissions. Windows' calls are as before. Elsewhere they are
    stat and lstat, opendir and readdir (each entry lstat'ed), realpath, getcwd and chdir,
    statvfs, mkdir, rmdir and unlink, C's rename, a copy through fopen with the source's mode, and
    chmod, with the path in UTF-8. struct stat, statvfs and dirent are read at their C library's
    offsets (glibc's and FreeBSD's differ). A path stays wide text (UTF-32 there), with `/` the
    one separator and no root name.
  - Stage 4 is done: no library snippet fails to build for FreeBSD for the library's Win32 or
    UCRT calls any more.

### 4. Toolchains: what links for each target, from each host

- **Linux and FreeBSD targets.** clang with `--target` and ld.lld from any host:
  `clang --target=x86_64-unknown-freebsd14 --sysroot=<root> -fuse-ld=lld`.
  - **The sysroot** is the target's headers and libraries:
    - FreeBSD: the base system's `base.txz` for the version NFS runs (`usr/include`, `lib`, and
      `usr/lib` with crt1.o, libc, libthr, libm and libgcc_s), about 180 MB unpacked.
    - Linux: the host's own root on a Linux host. On Windows, WSL's Ubuntu
      (`\\wsl.localhost\Ubuntu`) is a glibc x86-64 sysroot, or the link runs inside WSL. For arm64,
      Ubuntu's `arm64` multiarch packages or a Debian arm64 sysroot.
  - **The C import** runs `clang -E --target=... --sysroot=...` on the target's headers. glibc's,
    FreeBSD's and Apple's headers use GNU extensions the importer must accept or drop
    (`__attribute__`, `__extension__`, `__restrict`, inline functions, `__builtin_va_list`, Apple's
    availability attributes and `__asm` renames). They are measured in the Linux stage.
    - FreeBSD's add more: clang's nullability qualifiers (`_Nonnull`, `_Nullable`), C99's
      `restrict` and C11's `_Noreturn`, which the importer drops. They also put
      `__attribute__((aligned(N)))` after a member's declarator, which the member takes.
    - glibc's are read with `_GNU_SOURCE`, as g++ and clang++ read them for C++ (libstdc++ needs
      it; 2026-11-05): the declarations C++ code on Linux is written against, GNU's own functions
      (`pthread_setname_np`, which the game's Linux code calls) and GNU's variants where POSIX has
      another (`strerror_r` returning the text, a `char*`).
    - What the importer still cannot read is never a silent wrong declaration:
      - a declaration it skips whole is not declared;
      - a struct or union whose body it could not read in full is an error where a use needs its
        layout (a pointer to it needs none);
      - an enum with an enumerator whose value it could not read is not declared, none of its
        enumerators.
    - Before this, FreeBSD 14.3's ucontext_t imported as 360 bytes, not 880 (2026-11-03).
- **macOS targets.** Built on Windows and Linux in this stage (kovarex, 2026-11-06), as "any host
  builds any target" has it (2026-11-02): ld64.lld and the SDK's headers and stubs, the SDK supplied by
  each user (Q7). Running them takes CI's macOS runners (Q5).
- **Windows target from another host:** lld-link and the Windows SDK's import libraries and UCRT
  headers. That is not needed for the minimum: Windows builds on Windows.
- **Finding the toolchain:**
  - Windows: today's vswhere search.
  - Linux and FreeBSD: `clang` and `ld.lld` on PATH, or an LLVM release directory.
  - macOS: `xcrun --find clang`.
  - Everywhere: `KPP_CLANG`, and a `--sysroot` or build.kpp's `project.sysroot`; without one, the
    sysroot beside kpp, `sysroots\freebsd-x86_64` or `linux-x86_64` (a release brings FreeBSD's:
    docs/build.md, "Installing"; 2026-11-04).

  One clang major version is supported per release (19 today).
- **Testing per target:**
  - Linux x86-64: WSL here, and CI's ubuntu runners.
  - Linux arm64: CI's ubuntu arm64 runners, or qemu-user in WSL with an arm64 sysroot.
  - macOS: CI's macOS runners, arm64 and x86-64 (Q5).
  - FreeBSD: kovarex's NFS site over SSH (a test bundle and a script that runs it and reports), a
    local FreeBSD VM (Hyper-V is in Windows 10 Pro), or a FreeBSD VM in CI (cross-platform-actions
    runs one on Linux runners) (Q3).

  The spec runner takes `--target` and a runner command (`wsl`, `qemu-aarch64`, `ssh host`) for
  targets that do not run on the host. The Windows-only snippets (Win32, UTF-16, SEH-specific) are
  marked.

### 5. The host: kpp on Linux and macOS

kpp is C++20, built with clang. Its Win32 uses come in four kinds:
- threads with large stacks and a guard against stack overflow;
- processes and pipes;
- the compile server's channel;
- paths and finding tools.

The proposal is a small host layer, `compiler/src/host.hpp/.cpp`, with one implementation for
Win32 and one for POSIX (Linux, macOS; FreeBSD for free):
- `runWithStack(size, fn)`:
  - Win32: CreateThread with a reserved stack.
  - POSIX: `pthread_attr_setstacksize`.
- `guardStackOverflow(fn)`, the codegen's guard:
  - Win32: `__try` on `EXCEPTION_STACK_OVERFLOW`.
  - POSIX: a SIGSEGV handler on `sigaltstack` that checks the fault against the thread's guard
    page, reports, and `_exit`s, as the Windows guard does.
- `spawn(command, arguments, stdout and stderr files or pipes)`, `wait`, `kill`:
  - Win32: CreateProcess.
  - POSIX: `posix_spawn`.

  This replaces `system()` and `_popen` with argument vectors, so quoting stops being per-shell.
- `executablePath()`:
  - Win32: GetModuleFileName.
  - Linux and FreeBSD: `/proc/self/exe`, or sysctl `KERN_PROC_PATHNAME` on FreeBSD.
  - macOS: `_NSGetExecutablePath`.
- `processId()`, `temporaryDirectory()`, `environment(name)`, `cpuCount()`.
- The server's channel:
  - Win32: a named pipe.
  - POSIX: a Unix domain socket in the user's runtime directory.

  Both carry the same protocol (server.cpp).

Builds and tests:
- CMakeLists.txt gets non-MSVC flags (`-Wall`, an 8 MB main stack through `ulimit` or a thread of
  its own). It is configured with clang and Ninja on every host.
- The test entry points become cross-platform: `run_tests` and `spec` as PowerShell 7 (`pwsh`,
  which runs on all three) or Python scripts, keeping the `.cmd` files as one-line wrappers on
  Windows.
- CI adds ubuntu and macOS jobs that build kpp and run the portable tests.

**Who does what (proposed):**
- **Thread B (this thread):** the targets, the driver and the toolchain (main.cpp's link lines and
  target record, source.cpp's tool discovery), codegen including its host uses (threads with big
  stacks, the stack guard), and `host.hpp` itself, which codegen and the driver need first.
- **Thread A:** the server's channel and processes (server.cpp) on host.hpp.
- **Thread E:** build.kpp's target settings, `kpp`, run.cpp, project.cpp and generate.cpp, the test
  scripts, CI, and packaging (signing and notarization on macOS).
- **Thread F:** lsp.cpp's processes and pipes.
- **Thread C:** the standard library's platform layer, with B for the runtime's few calls.
- **The converter (convert.cpp)** reads the Factorio sources and can stay Windows-only until
  needed.

All of this goes through the coordinator.

### Stages (each landed with specs, the Windows IR checked unchanged)

1. **The target model and Linux x86-64 hello world:**
   - `--target`, the triple and data layout, no `dllimport`, the entry, the runtime's stderr,
     DWARF;
   - the ELF link (clang and ld.lld with a sysroot, or WSL's).

   A spec builds, links and runs hello world under WSL.
2. **The portable subset of the spec suite on Linux x86-64:**
   - the runner's `--target` and runner command, and the Windows-only snippets marked;
   - the C ABI lowering for structs by value (SysV, and MS x64's small structs);
   - `long` and `wchar_t` as Q1 and Q2 decide;
   - the C import with `--target` and a sysroot.
3. **Exceptions on Itanium tables:** the personality and the lowering. Every exception spec runs
   on Linux, and the Windows IR is unchanged.
4. **The platform layer on POSIX:** the Win32 code moves behind `platform::`. POSIX threads,
   mutexes, condition variables, time and sockets come first, then files, the environment and
   processes.
5. **The host layer:** kpp builds and runs on Linux (WSL here, CI ubuntu), with A, E and F.
6. **FreeBSD x86-64:** its sysroot, its headers' constants and structs, its errno, and a test
   bundle run on FreeBSD. kpp.net's server can be built from here on.
7. **AArch64:** the AAPCS64 and Apple C lowerings, Linux arm64 (CI or qemu), and the memory
   ordering audit.
8. **macOS arm64, then x86-64:** Mach-O (no COMDATs, ld64.lld, ad-hoc signing, compact unwind), built
   on Windows and Linux, and CI macOS jobs that run the result. kpp hosted on macOS is not part of
   it (under consideration, 2026-11-06).

### Stage 1 as built (2026-11-01)

- **The target model.**
  - `--target windows|linux|freebsd`, or a triple (target.hpp). The other names say which stage
    brings them.
  - `--sysroot DIR`.
  - The program sees the target through the `config::platform` that already existed (`Platform`
    gained `FreeBSD`), not a new `config::os`. `--config platform=` must agree with the target.
- **What codegen emits for the target:**
  - its triple and data layout;
  - DWARF instead of CodeView;
  - the runtime's stderr: glibc's `stderr`, FreeBSD's `__stderrp`;
  - no `wmain` entry (a program with only `wmain` is an error off Windows).

  The driver compiles with `--target` and links with ld.lld, the sysroot, `--gc-sections`, `-lm`
  and `-pthread`. A release build is ThinLTO through ld.lld (`--thinlto-jobs`,
  `--thinlto-cache-dir`).
- **Until stage 3:** off Windows no call unwinds, so a `try`'s handlers are never reached. A
  `throw` writes out the program's output, then `exception of type T thrown: message (exceptions
  do not unwind on this target yet ...)`, then aborts.
- **Refused loudly, until stage 2:**
  - an `extern "C"` function with a struct by value;
  - a C header import.
- **The standard library's first platform split:**
  - `clib/CStdioWindows.kpp`, `CStdioLinux.kpp` and `CStdioFreeBSD.kpp` define stdin, stdout and
    stderr. Each is chosen by the file-level `requires config::platform == ...` kpp already had,
    which may serve the platform layer instead of directories.
  - Library functions that call Win32 or the UCRT (`_errno` in `stod`, Filesystem, Thread) link
    only while unused: a program for Linux that calls one fails to link, by name.
- **A sysroot for Linux** came from WSL's Ubuntu (`tools/sysroot/linux-from-wsl.sh`, glibc
  2.39); since 2026-11-04 it is Debian 11's, glibc 2.31 (`tools/sysroot/linux.ps1`, Q6), which
  targets.ps1 makes, or makes again over a copy of WSL's, with no WSL needed. Its link takes 0.03 s. The WSL root itself is no sysroot: ld.lld cannot follow
  `\\wsl.localhost`'s symbolic links, and the attempt took 16 s.
- **Test:** `tests/targets.ps1` in run_tests:
  - Windows: hello world, and an uncaught exception's report;
  - FreeBSD: the IR, and an ELF object for FreeBSD (OS ABI 9);
  - Linux: debug, release and `--debug-info` builds run under WSL, and the uncaught throw.

  The Linux part is skipped without WSL.
- **Windows' IR** over the 906 spec snippets, with one standard library on both sides: 29
  snippets are byte-identical. The other 869 differ only in the uncaught-exception report, a fix
  found on the way:
  - It read Exception's message (a String) as a heap pointer. A message of up to 22 characters is
    stored in the object itself, so it crashed (0xC0000005 after the type).
  - It now flushes the program's output first (abort wrote no buffer out, so a line printed before
    the throw was lost when stdout was a file).

  The new standard library files renumber the lambdas after them (`__lambda_<file>_<n>`), which
  changes names only.
- **Seen on the way, Q1's:** `clib/CStdio.kpp` declares `fseek` and `ftell` with `long`, which is
  64 bits in glibc's C.
- **The spec suite on Linux** (`spec.cmd --target linux`, built here and run under WSL, 2026-11-01):
  819 pass, 3 pending, 72 failing, 14 skipped.
  - Skipped: the `run:` and `test:` snippets (kpp runs those on this machine), and 4 marked
    `// target: windows`:
    - MSVC's `%I64u` and LLP64's `%ld` (glibc reads `%I64u` as its `I` flag and a width of 64);
    - a file that requires the Windows platform;
    - the UCRT's declarations;
    - `__ImageBase`.
  - Two snippets print the platform and have a `.linux.expected`.
  - Every failure is a later stage's:
    - 39 import C headers (stage 2);
    - 16 link library functions that call Win32 (stage 4);
    - 17 throw, which aborts (stage 3).

### Stage 2, the C calling convention for structs by value (2026-11-01)

- **Calls of C functions** (`extern "C"`, or an imported header's) pass and return a struct by value
  as the target's C does (codegen's `cPassing`):
  - **MS x64:** a struct of 1, 2, 4 or 8 bytes is one integer, both ways. Others are a pointer to a
    copy and a result slot, as before.
  - **SysV x86-64:** a struct of up to 16 bytes goes in one or two registers by its eightbytes,
    INTEGER where an integer or pointer lies, SSE where only floats do (`float`, `<2 x float>`,
    `double`). That holds while enough registers are left; otherwise the whole struct goes on the
    stack (`byval`), as clang does. Larger, packed or unaligned ones go on the stack, and their
    results through the slot.

  The `declare` lines carry the same types. A kpp *definition* of a C-callable function that takes
  or returns a struct where C passes it differently is an error until it is needed.
- **Spec:** interop/c_structs_by_value, against clang's C for every size class both ways,
  including registers running out. It passes on Windows and on Linux.
- **Before, on Windows**, every struct went as a pointer to a copy, which is MS x64's rule only
  above 8 bytes:
  - Windows IR over 908 spec snippets: only the new one differs.
  - The game: one call differs, `SetConsoleCursorPosition(HANDLE, COORD)`. COORD is 4 bytes, so
    the console cursor went to a pointer's bits; it is now passed in a register.

- **Narrow integers (2026-11-07, thread I; a silent wrong answer until then):** where the target's C
  reads a char, short or bool from its whole 32-bit register (SysV x86-64 as clang reads it, and
  Apple's arm64 as its ABI says; not MS x64, not Linux's AAPCS64), the caller extends such an argument
  and the callee such a result (LLVM's `signext`/`zeroext`, by the type's sign; bool `zeroext`):
  - a C function's `declare` line has them on its narrow parameters and result, so every direct call
    extends;
  - a call through a function pointer extends its narrow arguments at the call (the callee may be C's;
    a kpp callee ignores the upper bits);
  - every definition extends its narrow result, so that a C caller of a kpp function, through a
    pointer too, reads it right; no definition counts on its narrow parameters being extended.

  Before, kpp passed them with garbage in the upper bits: on Linux a clang-compiled
  `widen(short, signed char, unsigned char)` gave 655355400 in a debug build and -4600 in a release
  one, not -30200. Spec interop/c_narrow_integers_extended (direct, through a pointer, and kpp's
  functions' results that C reads through pointers). The Windows IR is unchanged.

### Stage 2, the target's C headers (2026-11-01)

Done before Q1 and Q2 were decided; what changed with them is in the next section.
- **The C import** preprocesses with `--target` and `--sysroot` (both in the cache's key), and
  reads glibc's own `stdio.h`, `stdlib.h` and `string.h`. They give 20 parse errors (GNU
  extensions), ignored as Windows' are.
  - **`long`:** on an LP64 target each C `long` became `long long` in the imported text (`widenCLong`,
    gone with the next section: kpp's `long` is C's now).
  - **`va_list`:** clang's `__builtin_va_list` becomes SysV's `__va_list_tag[1]`, 24 bytes of
    register save area (`vaListForSysV`). Codegen's `va_start`, `va_copy` and `va_end` take its
    address, or the pointer a parameter holds, and `va_arg` is LLVM's `va_arg` there.
  - **`intrin.h`:** MSVC's header becomes clang's `immintrin.h` and `x86intrin.h` off Windows: the
    same SSE intrinsics.
- **`CLong` and `CULong`** (gone with the next section): C's long and unsigned long, int32 on Windows and int64
  elsewhere, in `clib/CWindows.kpp`, `CLinux.kpp` and `CFreeBSD.kpp` (the files once named
  `CStdio<platform>`, which also hold the standard streams). The library's C declarations with
  `long` now say `CLong`: `fseek`, `ftell`, `strtol`, `strtoul`, `lround`.
  - The import found the need itself: glibc's real declarations clashed with the library's
    32-bit ones ("the C function strtol is declared with two types").
  - Still `long`, which is Q1's: Win32's declarations (Windows-only, where long is 32 bits) and the
    library's own 32-bit atomics (`kpp_atomic_*32`).
- **printf's format check follows the target** (resolver.cpp's `setLP64`). On LP64, `%ld` takes
  64 bits, and MSVC's lengths (`%I64d`, `%I32d`, `%Id`, `%w`) are errors: glibc reads `I` as a
  flag and the digits as a width. A 32-bit `long` given to `%ld` printed garbage silently on Linux
  before.
- **Test:** `tests/targets.ps1` gained `imports.kpp`: `strtol("9000000000")` into a `CLong`,
  printed with `%ld`. It gives `2147483647 4` on Windows (32 bits, saturated) and `9000000000 8` on
  Linux. An `int` given to `%ld` is refused for Linux.
- **The spec suite on Linux:** 850 pass, 4 pending, 36 failing, 23 skipped.
  - Every failure is a later stage's: 16 links of library functions that call Win32 (stage 4),
    and 20 throws (stage 3).
  - Skipped as Windows' own: 9 more. Six import `windows.h`; the others use MSVC's
    `_byteswap_*`, `_BitScanForward`, `__cpuid` and `_alloca`.

### Stage 2, `long` and `wchar_t` as the target's C has them (2026-11-01, after Q1 and Q2)

- **Sizes:** `long` (D's distinct type) is as wide as the target's C long: `TypeTable::longBits`,
  64 on LP64 targets. `wchar_t` (the type, the keyword, `L'x'` and `L"..."`, in the resolver,
  the constant evaluator and codegen's literals) is the target's C wchar_t: `TypeTable::wcharBits`,
  a signed 32-bit int (UTF-32) off Windows, as glibc, FreeBSD and macOS have it; unsigned 16 bits on
  Windows. `GlobalScope::setLP64` sets both, and printf's `%ld`, for the target.
- **What follows from it:**
  - An imported header's `long` is kpp's `long` again (no `widenCLong`).
  - The library's C declarations say `long` again (`fseek`, `ftell`, `strtol`, `strtoul`,
    `lround`).
  - `CLong` and `CULong` are gone.
  - The library's `kpp_atomic_*32` are declared with `long`, but codegen sizes them by their
    names, so they stay 32 bits. Their declarations go with D's deprecation of `long`.
- **Test:** `imports.kpp` in tests/targets.ps1 prints `%ld` of `strtol("9000000000")`,
  `sizeof(long)`, `sizeof(wchar_t)`, and the C library's `wcslen` of `L"h\u00e9llo"` and of an
  emoji. Windows gives `2147483647 4 2 5 2` (the emoji is a UTF-16 surrogate pair); Linux gives
  `9000000000 8 4 5 1`.
- **Specs on Linux:** C++'s LP64 answers have a `.linux.expected`:
  - `integer_types_are_distinct`: 2147483648 is a `long`, `0xFFFFFFFFL` and `1u + 1L` are `long`,
    `1UL + 1LL` is `unsigned long long`, and `sizeof(long)` is 8.
  - `numeric_limits`: `Limits<wchar_t>::max()` is 2147483647.

  `pointer_bitwise_operator_is_an_error`'s expected error no longer names wchar_t's type. The suite
  on Linux: 855 pass, 3 pending, 23 skipped, and 37 failing, every one stage 3's (20 throws) or
  stage 4's (17 Win32 links).
- **Seen on the way:** `sizeof(L"ab")` was a pointer's 8 on every target; since 77aaaa1a it is C++'s
  array size, its code units and the null (spec language/sizeof_string_literal).
- **An imported typedef of a name kpp builds in is kpp's type (thread B, 2026-11-05):** glibc and
  FreeBSD declare `int64_t`, `ptrdiff_t`, `intptr_t` and `ssize_t` as `long` and `uint64_t`,
  `size_t` and `uintptr_t` as `unsigned long` on LP64 targets (MSVC as `long long`). With `long` a
  type of its own, a C function taking `int64_t*` or `size_t*` refused kpp's `i64*` and `usize*`
  ("a pointer (int64*) does not convert to long*"), and kpp's own `int64_t` meant `long` wherever
  `stdint.h` was imported. Found building the game for Linux: SDL_StepUTF8's `size_t*`,
  glGetQueryObjecti64v's `GLint64*` (khrplatform's `int64_t`), 13 of Lua's calls. Now an imported
  header's typedef whose name kpp builds in (resolver's builtin names) is kpp's type when the two
  agree in width and signedness: the same C type on the target, so no call changes. One that
  differs in width stays the header's (glibc's 64-bit `int_fast32_t`), and C's `long` itself stays
  `long`. Windows' typedefs were kpp's types already. Spec
  interop/c_import_fixed_width_typedefs_are_kpps (the same output on every target).
- **GCC's builtins and cpuid.h (thread B, 2026-11-05):** clang declares GCC's builtins itself, so
  no header does, and C code calls them (29 calls in the game's `#ifdef __GNUC__` branches, glibc's
  inline functions). The import declares the ones C code calls: `__builtin_clz`, `__builtin_ctz` and
  `__builtin_popcount` with their `l` (the target's C long) and `ll` forms, `__builtin_bswap16`,
  `32` and `64`, `__builtin_expect`, `__builtin_prefetch` and `__builtin_alloca`; the code generator
  emits each as clang does, beside MSVC's intrinsics (gccBuiltin). cpuid.h's `__get_cpuid_max`,
  `__get_cpuid` and `__get_cpuid_count`, inline functions whose bodies are inline asm (which codegen
  refuses), are its cpuid instruction, as MSVC's `__cpuid` is: the game's SystemUtil on Linux. Specs
  interop/c_gcc_builtins and interop/c_cpuid_header.
- **Function types of C's calling convention (thread B, 2026-11-05, after thread D's 088b6d2e).** A
  kpp function passes a struct by value as a pointer to a copy; a C function as its target's C does
  (MS x64: 1, 2, 4 and 8 bytes in a register; SysV: up to 16 bytes in registers by class, larger on
  the stack). A direct call knows which the callee is; a call through a function pointer did not, and
  passed the struct kpp's way to a C function: a silent wrong value (`-627068060` for 45 on
  Windows). Now a function type that takes or returns a class by value is of C's convention when it
  is a C function's (an imported header's, or extern "C" in kpp), or a C header spells it (a
  typedef's, a field's, a parameter's function pointer): TypeTable::function's cConvention,
  GlobalScope::Impl::withCConvention in declaredTypeOf. A call through a pointer to one is lowered as
  a direct C call is (codegen's callIndirectC), and it is another type than the function pointer type
  kpp code writes with the same signature: no conversion between the two, either way ("a pointer
  (extern C int32(S8)*) does not convert to int32(S8)*"), where the call would pass the struct as
  the function does not take it. kpp code holds such a pointer by `auto`, `decltype` or the header's
  typedef, and passes it to its own functions and templates (spec interop/c_function_pointer_parameter).
  A signature that passes no struct by value is one type for both conventions, so nothing else
  changes. No syntax is added: the type prints as `extern C int32(S8)`, without quotes, since a type's
  printed form is part of codegen's quoted LLVM names (with them, `@"callIt(extern "C" ...)"` was
  invalid IR). Specs
  interop/c_function_pointer_struct_by_value (8, 16 and 24 bytes, a header's typedef, pointers C
  filled in, on Windows and Linux), interop/c_callable_struct_by_value_through_pointer (D's, now
  passing) and interop/c_function_pointer_conventions_differ_is_error.

### Stage 6, FreeBSD: the sysroot, the static link and the C import (2026-11-03)

kovarex: "for 4, we first need to support freebsd compilation, so the web can be in kpp" (4 being
kpp.net's deploy). Taken by the registry thread, B reviewing B's files.
- **The version (Q3, Q4):** NearlyFreeSpeech.NET runs FreeBSD 14.3-RELEASE (kovarex's
  `freebsd-version` there, 2026-11-03). The triple stays `x86_64-unknown-freebsd14`; the sysroot is
  14.3's.
- **The sysroot:** `tools/sysroot/freebsd.ps1` downloads 14.3's `base.txz` (kovarex's OK,
  2026-11-03: "it is ok to download the FreeBSD's official base system"; 210,362,720 bytes, from
  FreeBSD's archive, since download.freebsd.org no longer has 14.3, over http, its archive's https
  certificate naming another host). Its SHA-256 is pinned from the release's MANIFEST. The script unpacks `lib`, `usr/include` and
  `usr/lib` into `compiler/build/sysroots/freebsd-x86_64` (265 MB), replaces every link with a copy
  of its target, and strips the objects' and archives' debug sections: FreeBSD compresses them
  with zlib, and neither Visual Studio's lld nor llvm-mingw's is built with zlib. Windows' tar has
  no xz, so Git for Windows' `xz` decompresses. It takes 47 s.
- **Static by default:** a FreeBSD program links with `-static`. kpp.net's programs then need none
  of the host's shared libraries, and survive NFS's realm upgrades. The GENERIC kernel runs older
  releases' programs (COMPAT_FREEBSD*), so a 14.3 program keeps running on 15.
- **Test:** `tests/targets.ps1` links hello world for FreeBSD (debug, release, `--debug-info`,
  about 0.8 MB each) when the sysroot is there, and checks the ELF itself, since nothing runs it
  here: a 64-bit x86-64 executable (ET_EXEC), OS ABI 9, no interpreter and no dynamic section, and
  FreeBSD's ABI note (NT_FREEBSD_ABI_TAG, from the sysroot's crt1.o) saying 1403000. It also
  links `targets/cgi.kpp`, a CGI program on the http and mariadb libraries (net and url under
  them), 1.4 MB, checked the same way.
- **The C import** reads FreeBSD's own headers through `--sysroot`: stdio, stdlib, string, errno,
  unistd, fcntl, sys/types, sys/socket, sys/time, netinet/in, netdb, pthread, time, signal and
  wchar give 5365 declarations and 21 parse errors (ignored, as glibc's 20). FreeBSD's values come
  through, not Linux's: EAGAIN 35, AF_INET6 28, SOL_SOCKET 0xffff, SO_REUSEADDR 4, O_NONBLOCK 4,
  `sizeof(sockaddr_in6)` 28, `sizeof(pthread_mutex_t)` 8.
  The 21 are not harmless:
  - Five constructs give them: `_Nullable` and `_Nonnull` (struct __sFILE's function pointers,
    `sigset`, `sigwait`), `_Noreturn` (`abort`, `exit`, `_Exit`), `restrict` in a parameter
    (`pthread_mutex_init`, `pthread_rwlock_init` and others), `__attribute__((__aligned__(16)))`
    after a member (`mc_fpstate`, `sc_fpstate`), and `_Thread_local`. A dropped function or
    variable is an error where it is used.
  - A struct with such a member is laid out smaller than C's, silently: against clang, mcontext_t
    is 288 bytes and 8-aligned instead of 800 and 16, ucontext_t 360 instead of 880, and struct
    sigcontext 304 instead of 816. The same constructs do it on Windows: pending spec
    interop/c_struct_member_attributes. glibc's mcontext_t and ucontext_t match C.
  - FILE is the standard library's own opaque `_iobuf` (8 bytes) on every target, not the C
    library's struct (312 bytes on FreeBSD, 216 with glibc).
- **Found on the way:** an `extern "C"` variable declared in a scoped library had the library's
  scope in its name (http's `environ` linked as `http::environ`, on every target). A variable of C
  linkage now has C's name (kpp/extern_c_variable_in_library).
- **Found running the libraries' tests on Linux:** an uncaught exception whose message is longer
  than a small String printed the heap pointer's bytes instead of the message, on every target.
  The report read String's mark at byte 23, where the 24-byte String had it; String is 32 bytes
  since container sizes became 64-bit (85e03a9b), its mark at byte 31. The byte now comes from
  String's laid-out size (targets.ps1's uncaught, with a long message, on Windows and Linux).
- **The libraries' tests for FreeBSD** (2026-11-03): base64, bcrypt, html, json, markdown, net,
  sha256 and url build and link. http's and mariadb's do not link (the standard library's Win32
  calls, stage 4), and httpclient has no client off Windows yet. Run on Linux under WSL as the
  nearest POSIX, all of them pass since stage 3 (below); before it, each stopped at its first throw.
- **The spec suite for FreeBSD** (`spec.cmd --target freebsd`, 2026-11-03): every snippet built and
  linked for FreeBSD into `build\spec-freebsd`, a bundle with each program's expected output and a
  `run.sh` that runs them on FreeBSD and compares. Nothing runs FreeBSD here, so a snippet that
  links is "built": 916 built, 95 pass (the snippets that must not compile), 9 pending (the
  Windows suite's), 23 skipped, 19 failing (389 s):
  - 18 are stage 4's: the standard library's Win32 and UCRT calls (threads, time, filesystem,
    `_errno` in `stod`, `set_terminate`) do not link.
  - `interop/c_math_classification_macros`: FreeBSD's math.h makes `fpclassify`, `isnan` and
    `signbit` type-generic through C11's `_Generic` (`__fp_type_select`), which the C import does
    not turn into a call ("'fpclassify' is not declared"). On Linux the same snippet stops on the
    standard library's own `isnan` and `isinf` against glibc's declarations.
- **Not yet:** running anything on FreeBSD (a VM in CI, then one run on NFS), stage 3's exceptions
  (a throw aborts there as on Linux) and stage 4's platform layer.

### Stage 3, exceptions on the Itanium model (2026-11-03)

Linux and FreeBSD, as decided above (two phases, the types matched in phase 1). Windows is unchanged:
its IR for every spec snippet (1073) is byte-identical before and after.
- **The personality**, `__kpp_personality_v0`, is C in main.cpp (`itaniumRuntime`), compiled once
  per target into the import cache and linked into every program off Windows, as windows-gnu's
  `kpp_long_double` is. In C rather than IR text: it reads the LSDA (uleb128 and sleb128, the
  call-site, action and type tables), which is about 150 lines of C and would be about 300 of IR.
  - **Phase 1** matches each catch clause's descriptor with `kpp_eh_offset`. `null` is catch (...),
    and a zero entry is null in every encoding, pc-relative too.
  - **The empty exception specification** of a noexcept function ends the program in phase 1
    through `kpp_eh_terminate`, with the throw site on the stack.
  - **A call its function's table does not list** ends the program in phase 1 too, as C++ does:
    continuing would skip that frame's cleanups, silently (B's review).
  - **Phase 2** installs the handler's landing pad with the clause's index, and the other frames'
    pads for their cleanups with 0.
  - **Encodings:** the type table is udata4 in FreeBSD's static non-PIC link and indirect pcrel
    sdata4 with glibc, and the call sites are uleb128. Anything else aborts with a message.
  - **Exception class:** only kpp's exceptions ("KPP\0KPP\0") are caught. Another language's pass
    through the cleanups.
  - **Linking:** `kpp_eh_offset` and `kpp_eh_terminate` are weak there, and a program without
    exceptions drops the object (--gc-sections). Without `kpp_eh_terminate` (no throw of its own:
    another language's exception at a noexcept frame), the personality writes a line and aborts.
- **codegen** (`itaniumUnwindDestination`, `itaniumTry`, `finishExceptionRuntime`):
  - **Landing pads:** a call that may throw is an invoke whose landing pad has `cleanup`, the catch
    clauses of every try around it in the function (innermost first), and in a noexcept function's
    own body the empty filter. A lambda written there is not noexcept itself (B's review:
    language/lambda_in_noexcept_function_may_throw), nor is a helper emitted while it is. The pad keeps the exception and the selector in two slots and goes on to a
    chain of plain blocks destroying what the call leaves, shared as SEH's cleanup pads are.
  - **Try:** its dispatch compares the selector with each handler's `llvm.eh.typeid.for` in order.
    The handler it finds takes the exception into its record (`kpp_eh_enter`); with none, the
    chain goes on outward, to the enclosing try or to `resume`.
  - **Handlers** are plain code: `kpp_eh_end` frees the unwinder's header too, and leaving a
    handler is a branch.
  - **A destructor that throws** during a cleanup is invoked with a landing pad of the empty filter,
    so the program terminates in phase 1.
  - **Throw:** `kpp_throw` puts the object, its descriptor and its destructor after the unwinder's
    32-byte header and calls `_Unwind_RaiseException`. When that returns (no handler),
    `kpp_eh_terminate` prints "unhandled exception of type T: message" from the throw site, then
    calls the terminate handler `setTerminate` installed, if any, and aborts if it returns
    (2026-11-04). The handler is read through the standard library's `extern "C"
    kpp_terminate_handler` (core/Error.kpp), as the runtime is IR text and names no kpp
    function; without the standard library (--no-std) there is none. With no memory for the
    header, it says so and aborts.
- **The unwinder:** FreeBSD's static link takes it from libgcc_eh (LLVM's libunwind in 14.3), and
  glibc's from libgcc_s. kpp's functions have unwind tables by default (they are not `nounwind`).
- **Found on the way:** the first personality took a pc-relative zero, catch (...)'s, for an
  offset. So catch (...) never matched with glibc: the inner `throw;` specs aborted.
- **Spec** (`spec.cmd --target linux`, under WSL): every exception snippet passes in debug,
  release and debug-info. The 21 failing are the known ones: stage 4's Win32 links, the long double
  and `_Generic` math headers, and LLVM's -O0 va_arg. The two release-only failures,
  `kpp/asserts_and_config` and `config_values`, are the runner's: its `.linux.expected` wins over
  `.release.expected`.
  - FreeBSD: every snippet builds (928, 19 known not to), and the 25 stage-3 lines of
    freebsd-known-failures.txt are gone. CI's VM confirmed it (run 41835608420, at 44e4f1f9):
    `spec freebsd: ALL PASSED (956 pass, 3 known)`.
  - A snippet's `// pending on windows: why` is a gap of the targets it names alone (run_spec.py,
    2026-11-04).
  - On Windows, an exception nobody catches ends the program from the search too (2026-11-04, B's
    review): `@main`'s filter (`kpp.eh.filter.any`), which the search reaches only when no handler
    of the program takes a kpp exception, calls `kpp_eh_unhandled`: the report, the terminate
    handler, abort. Nothing unwinds first, as off Windows, so a handler that logs the stack sees the
    throw site. Before, `@main`'s handler reported after the frames had unwound
    (language/uncaught_exception_calls_the_terminate_handler).
  - noexcept on Windows (2026-11-05): a noexcept function's body unwinds, through its cleanups and
    its trys' handlers, to an outermost catchpad of its own whose filter (`kpp.eh.filter.noexcept`)
    ends the program in the search with the throw site on the stack ("exception of type T thrown
    through a noexcept function", the terminate handler, abort), as the empty filter does off
    Windows. Every call of such a body that may throw is an invoke now. Another exception (an access
    violation) passes the filter. language/noexcept_violation_terminates_at_the_throw passes on
    every target.
  - A thread's function (Thread's start, ThreadCall::start) is noexcept, as C++'s std::thread
    calls std::terminate for an exception that leaves it: on Windows that exception met no kpp
    filter and the system ended the process with 0xE0000001, no report and no terminate handler
    (language/exception_leaving_a_thread_terminates, every target).
- **The libraries' tests on Linux**: base64 (3), bcrypt (5), html (6), json (13), markdown (10),
  net (3), sha256 (6) and url (6) all pass. Each stopped at its first throw before.
- **A noexcept function's pad in a release build (2026-11-07, thread I; a silent wrong answer until
  then):** its landing pad with nothing to destroy was `landingpad cleanup filter [0 x ptr]` and a
  resume, which LLVM's optimizer deletes as a pad that only resumes, the filter with it: inlined into a
  caller's try, the caller's catch took the exception and noexcept said nothing (10 spec snippets,
  release builds for Linux and FreeBSD, x86-64 and arm64). The pad now goes to abort on the filter's
  selector, as clang's goes to `__cxa_call_unexpected`: the personality still ends the program in the
  search, and the pad stays.
- **Not yet:**
  - A thread a program starts itself through CreateThread with a function of its own (the
    converted game's threads that do not go through Thread) has no noexcept frame: an exception
    leaving it on Windows still ends the process with no report, unless the function is noexcept.
  - (`uncaught_exceptions()` off Windows, listed here until 2026-11-05: it counts kpp's own
    exceptions on every target since b55e5442, library/uncaught_exceptions_during_unwinding.)

### Stage 5, kpp on a Linux host (2026-11-06, thread E with A, B, D and F's files)

- **The host layer** is `compiler/src/host.hpp` and `host.cpp`, not the proposal's `runWithStack`
  set: what kpp did through Win32 or the C runtime's Windows names, behind one interface,
  its Windows half the code it replaced, moved unchanged. It covers:
  - the platform's name, the executable's path, the process id;
  - files by any path, pipes to a shell command, a shell command run to its end
    (`host::shell`: cmd's whole-line quoting on Windows, `/bin/sh` on POSIX), a program run with
    its arguments (`host::run`: CreateProcess, posix_spawn);
  - whether stdin is a person, Ctrl+C, the machine's description and load, a lock file held
    shared or alone, the language server's streams;
  - PATH lookup and case rules for paths.
- **No #ifdef outside host.cpp**, but for three Windows-only parts that compile to an explicit
  "not yet" on POSIX: source.cpp's watched listing (ReadDirectoryChangesW) and Visual Studio's clang
  (vswhere), and server.cpp, the compile server (named pipes). `host::compileServer` is false
  there: kpp builds in its own process, `kpp --run` too, `--server` is an error that says so, and
  the language server's in-process builds say the same.
- **On a Linux host**:
  - the default target is the host's (B's `hostTarget()`, c68df005), and the sysroot the host's
    own system unless one sits beside kpp;
  - the toolchain is the bundled one, KPP_CLANG's, else the clang on PATH;
  - every non-MSVC link needs lld, and a missing ld.lld is an error naming the package (B's
    rule: never GNU ld in its place);
  - clang 19 is needed: kpp's IR names LLVM 19's intrinsics, which clang 18 links as undefined
    symbols (`llvm.eh.typeid.for.p0`).
- **Built with CMake** on Linux (Threads, `-std=c++20`, not gnu++20, whose `linux` macro breaks
  the converter). `run_spec.py` on a Linux host runs build/kpp and the snippets natively.
- **Tested** by `.github/workflows/linux-host.yml` on every push: ubuntu-24.04 with clang 19, lld 19
  and Ninja builds kpp, `kpp --run` builds and runs hello world, and the spec runs natively. First
  green on master at b644530d: spec ALL PASSED (1375 pass, 2 pending, 22 skipped for linux, the
  Windows-only snippets and the compile server's steps).
- **Windows programs from a Linux host** (kovarex 2026-11-07, stage 1: Factorio's Windows exe built
  on Linux, "as long as it is not extremely difficult"): `--target windows-gnu` with llvm-mingw's
  Linux build of the bundled toolchain's release (`tools/sysroot/llvm-mingw.sh`, pinned and SHA-256
  checked, through KPP_CLANG or as an installation's bin/). What it took:
  - the UCRT import library's llvm-dlltool and the C libraries' llvm-ar named with the host's
    executable suffix, not `.exe`;
  - the C libraries' dialect chosen by the target (windows-gnu: clang's GNU driver with
    `--target=x86_64-w64-windows-gnu`), not by whether the toolchain is the bundled one: KPP_CLANG
    naming llvm-mingw's clang built them with clang-cl's flags;
  - Windows' headers found without case, as on Windows: MinGW-w64's are lower case, and Windows code
    includes `<Windows.h>`, `<WinSock2.h>` (25 of Factorio's files, the converted game's C imports
    and its C libraries). On a host whose file names have case, kpp writes a clang VFS overlay of
    the toolchain's system include directories, every file listed with `'case-sensitive': 'false'`
    (one that only remaps a directory looks inside it as it is), made once per toolchain and target
    in the imports' cache, and passes `-ivfsoverlay` to the C import and the C libraries' compiles
    (source.cpp's headerCaseOverlay);
  - the code generator's threads on 1 GB stacks on every host (`host::startOnLargeStack`), as on
    Windows: a Linux thread's default 8 MB is less than the game's deepest code needs.
  CI's linux_host job builds hello world and tests/targets/clibrary (whose Windows library and C
  import include `<WinSock2.h>` and `<Windows.h>`) for windows-gnu, in debug and release.
- **Not yet:** the compile server and the language server on POSIX (a Unix socket, A's; F's), macOS as a
  host (host.cpp has its executable path and machine description; nothing tested), FreeBSD as a
  host (the same), and a release for Linux (packaging, E's).

### Stage 7, AArch64: Linux arm64 (2026-11-07, thread I)

- `--target linux-arm64` is `aarch64-unknown-linux-gnu` (`config::arch` `Arch::Arm64`), linked as
  linux is against a glibc 2.31 sysroot: Debian 11's same eight packages for arm64
  (`tools\sysroot\linux.ps1 -Arch arm64`, into sysroots\linux-arm64; pinned from bullseye's signed
  index as amd64's are). A Linux host on AArch64 builds for itself.
- **The C calling convention (AAPCS64, codegen's cPassing):** a homogeneous floating-point aggregate
  (1 to 4 floats or doubles of one type, a union's members sharing places too) is `[N x float]` in that
  many vector registers (Linux's with `alignstack(8)`, as clang has it); any other struct of up to 16
  bytes `i64`, `[2 x i64]`, or `i128` when 16-aligned, its bytes read and written within the struct
  (a 3-byte struct is loaded as 3 bytes and widened, stored narrowed); a larger one through a pointer
  to a copy, a larger result through the caller's slot, `sret`, which AAPCS64 passes in x8 (a C
  function's slot is `sret` off Windows at the call and the declaration as at the definition). LLVM
  keeps an array's registers together, all of them or all on the stack, as AAPCS64 says. A struct
  holding a vector is refused.
- **Varargs:** `va_arg` on Linux's AAPCS64 is lowered as clang lowers it (the general registers' save
  area while `__gr_offs` is negative, the vector registers' by `__vr_offs`, else the stack); on Apple's
  arm64 every variadic argument is in an 8-byte stack slot and va_list a pointer. The C import's
  va_list is the target's: SysV's 24-byte array, AAPCS64's 32-byte struct (it was SysV's on every
  target off Windows), Apple's char* (the import cache's version is 12).
- **x86 alone:** an SSE intrinsic (`_mm_...`) or cpuid for an arm64 target is a compile error, never
  a stand-in (the game turns HAS_SSE2 off there itself); x86's headers (intrin.h, cpuid.h) refuse
  another processor already. The spin of a static local's guard is AArch64's `yield`, not x86's
  `pause`. The spec snippets about x86 alone say `// arch: x86_64` (skipped for linux-arm64).
- **Exceptions:** the personality reads the type table in any DWARF pointer encoding (2, 4 or 8 bytes,
  pc-relative, indirect): AArch64's PIC table is indirect pcrel sdata8, where it knew x86-64's three.
- **wchar_t** is unsigned 32 bits in glibc on AArch64 (signed on x86-64 and Apple's arm64), so it
  promotes to unsigned int there; plain `char` stays signed (kpp's meaning, the type table above), so
  a `char` bit field kpp writes -1 into is 7 to glibc's C on AArch64, the same bits; C's long double
  (128 bits there) refused as off Windows everywhere, but on Apple's arm64, where it is double.
- **The library:** glibc's arm64 layouts where they differ (PlatformLinux.kpp by `config::arch`):
  pthread_mutex_t 48 bytes, struct stat asm-generic's (st_mode at 16, a 32-bit st_nlink at 20).
- **Tests:** `run_spec.py --target linux-arm64` builds every snippet and runs it under WSL through
  qemu's user mode (KPP_QEMU_AARCH64, Ubuntu's qemu-user-static), the C ABI snippets against clang's C
  for the target: 1722 pass, 5 pending, 114 skipped (the `run:`, `test:` and x86 ones) on 2026-11-07.
  New: interop/c_structs_by_value_float_aggregates (the classes only AAPCS64 tells apart, on every
  target). A CI job on GitHub's ubuntu arm64 runners is E's (Q5).
- **Apple's variant**, written with it and not run until Mach-O links (M2, M3): no `alignstack` on a
  float aggregate, varargs on the stack, va_list a char*, narrow arguments extended by the caller
  (Target::extendsNarrowIntegers), long double = double.

### Stage 8, step M0: the macOS targets and the SDK check (2026-11-07, thread I)

- `--target macos` is `arm64-apple-macosx11.0.0` (data layout `e-m:o-...-Fn32`), `macos-x64`
  `x86_64-apple-macosx10.15.0` (Q6; Factorio's own x86-64 release says 10.13, which is for kovarex to
  decide), and a triple `arm64-apple-macos...`, `-darwin...` or `x86_64-apple-...` names one of them by its
  processor: the oldest system is kpp's, whatever version the triple says. `config::platform` is
  `Platform::Mac`, and `config::arch` is new on every target: `Arch::X86_64` or `Arch::Arm64` (an
  enumeration beside Platform; `--config arch=` must agree with the target).
- **The SDK (Q7):** `--sysroot DIR`, else `SDKROOT`; nothing is looked for beside kpp. Before any
  work kpp checks that the directory's SDKSettings.json says version 11.0 or newer (the first with
  arm64), that it has usr/include/stdio.h, and that usr/lib/libSystem.tbd lists the target's processor
  (a text stub's `targets:` or `archs:`). Otherwise one message: what was wrong, what a macOS SDK is,
  where one comes from (`xcrun --show-sdk-path` on a Mac, Xcode's or the Command Line Tools'; copied to
  this machine) and the two ways to give it. Specs kpp/macos_without_sdk_is_explained,
  macos_sdk_too_old_is_explained, macos_sdk_lacks_the_architecture_is_explained,
  macos_not_an_sdk_is_explained (fake SDKs beside them: the spec runner's `{here}`), and
  macos_sdk_checked_before_the_build: with a good SDK the build goes on (M2, below).
- **build.kpp (2026-11-09, thread E):** projectlib's kppTargetPlatform reads the names and triples as
  targetNamed does: `macos`, `macos-x64` and the apple triples are `Platform::Mac`, `linux-arm64` and
  the aarch64 Linux triples `Platform::Linux`. Until then a project's cell.platform said Windows for
  them, and kpp refused the build (its check that projectlib reads a name as kpp does). run_tests'
  project test builds a cell for each.
- **The spec for macOS, until kpp links (2026-11-09, thread E):** `run_spec.py --target macos` and
  `--target macos-x64` build every snippet against a macOS SDK (KPP_SDK_MACOS, else the stand-in
  beside kpp/macos_sdk_checked_before_the_build.kpp, with which a snippet that imports C headers is
  skipped); a snippet is "built" when its modules compile to Mach-O objects and its only error is
  where the link would be. compiler/tests/spec/macos-known-failures.txt lists those that do not build
  (none: 1430 built for each target, 289 skipped). CI's macos_build job runs both on every push, so
  M2's and M7's objects cannot regress unseen before M3. With M3 and M10 it becomes a bundle run on
  GitHub's macOS runners, as the FreeBSD jobs do.
- **M3, the link (2026-11-09, thread E, before a real SDK was here):** a build for macos or macos-x64
  links with ld64.lld through clang's Darwin driver: `--target=<triple> -fuse-ld=lld -isysroot SDK
  -Wl,-dead_strip`, and the driver adds `-arch`, `-platform_version macos <oldest> <the SDK's version
  from SDKSettings.json>`, `-syslibroot` and `-lSystem` (libc, libm, pthreads and the unwinder are
  libSystem's). The SDK goes as -isysroot: given --sysroot the driver reads no version from it.
  ld64.lld signs arm64 output ad hoc itself (an unsigned arm64 program does not run); x86-64 output is
  unsigned, as ld64.lld's default is. The personality's weak references to kpp_eh_offset and
  kpp_eh_terminate, which a program without a throw does not define, are allowed undefined
  (`-Wl,-U,_NAME`: ld64.lld wants every undefined symbol found, and dyld binds a missing weak import
  to null, as ELF does). A kept symbol (`--keep-symbol`) gets Mach-O's underscore. A release build's
  ThinLTO cache is `-cache_path_lto`, and a full LTO link has no partitions (ld64.lld has none). An
  optimized build's DWARF stays in its module objects, which the executable's debug map names, until
  dsymutil (M5) gathers it into a .dSYM. A C library with C++ files links the SDK's libc++ (`-lc++`).
  Checked against the stand-in SDK's libSystem.tbd, which lists the 54 libSystem functions the four
  kpp/macos_* programs link, debug and release: specs macos_sdk_checked_before_the_build and
  macos_stdlib_compiles_to_mach_o_objects (arm64), macos_x64_compiles_to_mach_o_objects and
  macos_x64_stdlib_compiles_to_mach_o_objects, now `// target: macos` and `macos-x64` snippets that
  link into Mach-O executables (LC_MAIN, libSystem.B.dylib, LC_BUILD_VERSION 11.0 or 10.15 with SDK
  11.3, arm64's LC_CODE_SIGNATURE), built by run_spec.py --target macos and macos-x64. The other
  snippets there defer their link with the stand-in (`--defer-link`), whose stub lists only those
  names. Not checked until a real SDK: the names against its .tbd stubs, a run (M10), C libraries'
  `.m` and C++ files.
- **M2, Mach-O objects (2026-11-07):** a build for macos or macos-x64 compiles its modules into Mach-O
  objects and stops at the link with an error that says so (ld64.lld is M3), never another target's
  link. Mach-O has no COMDATs and LLVM refuses a module that names one: each module's text loses its
  `$NAME = comdat any` lines and its definitions' `comdat` (ModuleCompile's withoutComdats, outside
  quotes), and the shared definitions stay linkonce_odr or weak_odr, weak external symbols the linker
  coalesces by name. The rest comes from the triple through LLVM, checked with llvm-readobj and
  llvm-nm on objects for both: LC_BUILD_VERSION macOS 11.0 (10.15 for x86-64), thread_locals as TLV
  descriptors (`__tlv_bootstrap`), `__compact_unwind` and `__eh_frame` with `__gcc_except_tab`, DWARF
  with --debug-info, ThinLTO bitcode in a release build. Specs kpp/macos_sdk_checked_before_the_build
  (arm64) and macos_x64_compiles_to_mach_o_objects: a template instance, a vtable, a thread_local, a
  static local and a throw, with --no-std, since the library has no macOS platform layer yet (M7: a
  program with it fails on Lock's size and the errno values, each an error). Not checked without an
  SDK: the C import of Apple's headers (M6) and anything run.
- **M7, the library's macOS layer (2026-11-07, thread I for C):** stdlib/platform/PlatformMac.kpp and
  stdlib/clib/CMac.kpp, from the SDK's headers as written before an SDK was here: pthread_t a pointer,
  the locks 64, 48 and 200 bytes with their initializers' signatures (0x32AAABA7, 0x3CB0B1BB,
  0x2DA8B3B4), CLOCK_MONOTONIC 6, _SC_NPROCESSORS_ONLN 58, `__error()`, struct stat with 64-bit inodes
  (st_dev 32 bits at 0, st_mode 16 at 4, st_nlink 16 at 6, st_mtimespec at 48, st_size at 96), struct
  statvfs (its counts 32 bits: FilesPosix reads them by statvfsCountBytes, st_dev by statDeviceBytes,
  8 on glibc and FreeBSD), dirent's d_name at 21, AT_FDCWD and UTIME_OMIT -2, macOS's errno values,
  FILE `struct __sFILE` and its __stdinp/__stdoutp/__stderrp, fpclassify by __fpclassifyd. The code
  generator: stderr through `__stderrp`, a thread_local's destructor through libSystem's `_tlv_atexit`,
  and the symbols macOS's headers give C functions (Emitter::cSymbol): on x86-64 the struct stat and
  dirent functions' `$INODE64` ones (stat, lstat, fstat, opendir, readdir ...: the plain ones fill the
  old 32-bit-inode structs, a silent wrong layout), on both `fopen$DARWIN_EXTSN`, `realpath$DARWIN_EXTSN`
  (and fdopen, popen); a wrong name there is an undefined symbol at M3's link, never another function.
  On arm64 the modules compile without GlobalISel (`-fno-global-isel`): LLVM 19's -O0 selector crashed
  on a float's bits read as an int and passed through printf's `...`. Checked: specs
  kpp/macos_stdlib_compiles_to_mach_o_objects and macos_x64_stdlib_compiles_to_mach_o_objects (threads,
  locks, condition variables, files, directories, space, clocks, a map, exceptions, and the test runner
  with --tests by hand), and every spec snippet built for both: 1354 of 1460 compile to Mach-O objects,
  the other 106 import C headers the stand-in SDK has not got. Waiting for the SDK: the link, a run,
  and checking these values and symbols against its headers and .tbd stubs.
- **Locks ahead of M7:** a zeroed pthread_mutex_t, pthread_cond_t or pthread_rwlock_t is no lock on
  macOS (its initializers start with a signature: 0x32AAABA7, 0x3CB0B1BB, 0x2DA8B3B4; 64, 48 and
  200 bytes). The library's Lock, Condition and SharedLock now set their first word to the platform
  file's `mutexSignature`, `conditionSignature` and `rwlockSignature` (0 on glibc and FreeBSD), which
  each platform file must declare.

### Questions for kovarex

- **Q1:** `long` on LP64 targets: decided 2026-11-01, "Distinct fixed types, deprecate long" (above).
- **Q2:** `wchar_t`: decided 2026-11-01, the target's C size (above).
- **Q3:** FreeBSD: the version NFS runs is 14.3-RELEASE, and `base.txz` is OK'd as the sysroot
  (both 2026-11-03, stage 6 above). How tests run there: a FreeBSD VM in CI and one smoke run on
  NFS, proposed and agreed with the coordinator; the CI job is still to be set up.
- **Q4:** decided 2026-11-03: FreeBSD 14, the one NFS runs, linked statically.
- **Q5:** decided 2026-11-07 (kovarex): the macOS programs kpp builds run on GitHub's macOS runners,
  arm64 and x86-64; building stays on Windows and Linux, never on macOS: a Windows or Linux job builds
  the specs and tests, a macOS job runs them. Linux arm64 works the same way later (ubuntu arm64
  runners).
- **Q6:** decided 2026-11-04 (kovarex: "the targets are ok as they are"; accepted again 2026-11-06): the oldest systems a
  kpp program runs on are Windows 10 x64, FreeBSD 14, glibc 2.31 (Debian 11, Ubuntu 20.04; Steam's
  Linux runtime "sniper" is Debian 11) and, when macOS work starts, macOS 11.0 arm64 and 10.15
  x86-64 (the target table above). Factorio states Windows 10, OS X 10.10 and "Linux (tarball
  installation)" with no glibc (factorio.com/buy, Steam, 2026-11-04). The Linux sysroot, a copy of
  WSL's Ubuntu 24.04 until then (programs needed glibc 2.39), is Debian 11's since 2026-11-04
  (tools/sysroot/linux.ps1, as freebsd.ps1 is: pinned packages checked by their SHA-256, from
  bullseye's signed index); a hello world and an uncaught exception linked with it need no glibc
  symbol newer than GLIBC_2.14.
- **Q7:** decided 2026-11-07 (kovarex): each user supplies the macOS SDK (a release brings none); when
  it is missing or wrong, kpp says what to have, where it comes from and how to give it (stage 8's M0,
  below).

## kpp format (decided 2026-11-08, kovarex's answers to thread F's design)

`kpp format` lays out kpp source in kpp's style, built on the compiler's lexer and parser (the parser's
format mode, ParsedFile::formatRoles). Its rules, each numbered with its count on the converted game and
on our own code, are in [format.md](format.md). Decided:
- Only whitespace changes: the output is lexed again and must have the input's tokens and comments,
  else the file is left as it is and reported. A file with a parse error is left untouched, whole (Q6).
- The project's style is a `.kppstyle` file beside build.kpp, `key = value` lines (Q1); without one,
  kpp's default. 13 settings (indent, columns, braces, case-indent, namespace-indent, access-label,
  wrap-operator, continuation, pointer, braced-list-spaces, template-space, initializer-commas,
  blank-lines-between-functions), each with a closed set of values; an unknown name or value is an error.
- The default writes braced lists `{1, 2}` (Q2) and `template<typename T>` (Q3).
- One-liner or expanded is the writer's choice, kept by every style (Q4).
- The formatter wraps long lines itself; the writer's breaks inside a statement are not kept (Q5). A
  function's parameters are all on one line or one per line, never in between, and call arguments
  likewise (no greedy filling); each nested list decides on its own, an argument that is a call staying
  whole on its line or wrapping its own arguments one per line, aligned after its `(`.
- The git filter lets a file that does not parse through as written (Q7).
- The formatter changes only what its rules cover, starting with the obvious ones and extended as
  nuances are found (Q8).
- Tests: not every combination of the 13 settings. Each setting has its own golden files (one input,
  an expected file per value), plus the few combinations that interact (indent tab with continuation,
  braces with one-liners, columns with wrap-operator); the round-trip, idempotence and perturbation
  checks run per configuration tested.

Decided 2026-11-09 (kovarex, as thread F recommended):
- Q9: our code and the converted game are reformatted after wrapping (step 3), the counts shown to him
  first (format.md has them; he checks them before either is reformatted).
- Q10: `kpp format --check` replaces kppstyle.py's layout rules in run_tests.
- Q11: line ends are kept per file.
- Q13: `columns` takes only the widths the tests cover (130, 100, 80, 160).

The 13 settings are built (2026-11-09; format.md, "Settings", has what each value does). Details the
design left open: a constructor's initializer list or base clause is one item to a line exactly when
its `:` starts a line (that is the kept fact); `namespace-indent` is about `namespace` only; `braces =
end-of-line` leaves `else` and `catch` on the line after the `}`; with a numeric `continuation` the `(`
of a control statement stays aligned.

## Open questions
- **Syntax must be parseable without name lookup.** C++ needs to know whether a name is a type
  or a template to parse `a * b;` or `f < a > (b)`. In the index-then-resolve model the parser
  runs before any name is known, so kpp should make these cases unambiguous by grammar
  (candidates: a different template-argument bracket or a required disambiguation, no
  most-vexing-parse, declaration statements recognisable from their first tokens). The prototype
  parser uses heuristics that work on 99.2% of Factorio files; the resolver can flag misparses
  later, but a real compiler should not have to guess. Decided 2026-11-04 (section 22).

- **Library names and program names share one global scope.** The first clash happened on
  2026-10-24: a test's own `struct Pair` against the library's `Pair`. Options: the library in a
  namespace (`kpp::`) that programs see through an implicit `using`; or program declarations
  shadowing library ones; or simply distinct names. Answered by "Code packages" (decided
  2026-11-04) with distinct names checked by the compiler, and by section 16's scoped libraries.

- **An own IDE** (proposed 2026-10-30, [ide.md](ide.md)): a query layer in the compile server and an
  LSP adapter first, then an editor written in kpp with a DbgEng debugger and hot reload. Its eight
  decisions are listed at the end of ide.md. **Decided 2026-10-31 (kovarex): integrate with the
  Language Server Protocol**, so that IntelliSense works in most editors: `kpp --lsp` (ide.md, "The
  language server as built"). **Decided 2026-11-06 (kovarex): an own editor comes later, only when kpp
  is fully mature with all features implemented.** No editor work until then; ide.md's decisions 3 to
  8 that depend on the editor are parked with it.

- **Running and testing kpp code** (the section above, 2026-10-30): Q1 an implicit `--run` (decided
  2026-11-06: none). Q2 (`--filter`), Q3 (the runner's isolation and parallelism) and Q4 (native
  tests) are settled by 8.7.

- **Targets and hosts** (the section above, 2026-10-31): Q1 `long` on LP64 targets (decided 2026-11-01), Q2 `wchar_t`, Q3 a FreeBSD sysroot (14.3, OK'd 2026-11-03) and a way to run tests there, Q4 the FreeBSD version (14, decided 2026-11-03), Q5 CI for macOS and Linux arm64 (decided 2026-11-07), Q6 the minimum OS versions (decided 2026-11-04), Q7 the macOS SDK off a Mac (decided 2026-11-07).

- **How a build is described** ([build.md](build.md), "Questions for kovarex"): none open. Settled:
  the command's name (kovarex 2026-11-07, "kpp everywhere": the repository builds compiler\build\kpp.exe,
  as a release installs it; it was kppc.exe), debug information in release builds
  (kovarex 2026-11-07: on by default, a file beside the executable, for crash reports as Factorio's);
  project files are generated (kovarex 2026-10-31, 54e30458); `kpp` builds the game's C libraries
  from source (7074036f) and the scripts link FASTBuild's, rebuilt at Factorio's HEAD on 2026-10-31.

- **Two-phase lookup of a template's non-dependent calls** (a known deviation, logged 2026-11-06, A;
  **decided 2026-11-06, kovarex: kept, as order matters nowhere else in kpp**). C++ binds a call in a template that depends on no parameter where
  the template is defined, with the declarations before it: `int helper(double); template<class
  T> int call(T) { return helper(1); } int helper(int);` makes `call(0)` 1. kpp's lookup is
  order-independent (symbols are, by design), so it sees `helper(int)` too and gives 2. Converted
  C++ could differ silently where a better overload is declared after a template that calls a name
  without its parameters. Matching C++ would take, for such a call, only the declarations before
  the template's definition (in its file, and the files before it). kpp's rule: a template's calls see
  every declaration of the name, as all of kpp's lookup does; a converted C++ program that relies on
  the earlier overload is written with it explicitly (a cast of the argument, or a qualified name).

- **No text mode** (a known deviation, **decided 2026-11-06, kovarex: CRLF is to be eliminated**).
  kpp's file streams are binary on every platform: `"\n"` is written as LF on Windows too
  (std::ofstream's text mode writes CRLF there) and nothing is translated when reading. getline
  with the newline as delimiter drops a `\r` right before it, on every platform, so CRLF files
  still read as lines. Spec library/file_streams_have_no_text_mode; stdlib/io/README.md.

- **noexcept in the function type** (decided by kovarex, 2026-11-06: full C++17 behaviour, "the
  noexcept is there for a reason"; logged as a deviation 2026-11-05). `void() noexcept` and `void()`
  are two types (TypeTable::function's noexcept, printed in the type, so two instances or overloads
  never meet in one LLVM name). A function's name has the type it declares, noexcept as the instance
  says (`noexcept(cond)` under the template's arguments); declaredTypeOf stays the signature without
  it, which calls, overrides and redeclarations compare. noexcept goes by a conversion, as a
  qualification does (rank 1, worse than the identity, so overloads on it choose) for a pointer, a
  member pointer and a reference, and never comes: an assignment, an argument, a return, a
  static_cast or a non-noexcept closure are errors, a C cast and reinterpret_cast are not. A template
  deduces it (T as the whole type, `R (*)(A...) noexcept(N)` binds N; a plain `R (*)(A...)` parameter
  takes a noexcept function, as a call converts it), a partial specialization matches it exactly, a
  conditional of the two is the plain type, `+closure` and a closure's conversion carry its call
  operator's. The noexcept operator reads it through a pointer, a member pointer and a closure. An
  overrider of a noexcept function must be noexcept, keyword `override` or not, and every declaration
  of a function says the same. A noexcept lambda's body ends the program as a noexcept function's
  does: the specifier was read and dropped until now, so its caller's catch took the exception. A
  handler of `void (*)()` takes a thrown noexcept pointer ([except.handle] 3.3, 1dd08930). Specs
  language/noexcept_*, clang's output.

- **A destructor's exception specification** (D, 2026-11-06, [except.spec] 8, the rest of kovarex's
  "full C++17 behaviour"). A noexcept-specifier says, defaulted or not; with none, written or implicit,
  a destructor may throw exactly when a base's or a member's may, the variant members of a union and
  of an anonymous union among them, pointers, references and statics not. One answer, the resolver's
  destructorMayThrow, cached as a trait; a closure's parts are the code generator's, so the checker
  does not know a class holding a closure by value (its override is not refused, the operator says it
  does not decide) and Emitter::destructionMayThrow walks such a class itself. Three things read it:
  the noexcept operator (`x.~X()`, `p->Base::~Base()`, `delete p` with a deallocation function that
  says otherwise, a call returning a class by value; always false until now, a silent wrong answer);
  the body, as a noexcept function's (emittingNoexcept): a throw leaving a destructor with no specifier
  ended up in the first try above it, silently, until now, as a call of one is no invoke; and the
  override check: a destructor that may throw over a noexcept virtual one is an error, also an implicit
  one, by a member's noexcept(false) ([except.spec] 4; clang's MSVC mode only warns). A class of no
  template is checked where it is written, an instance when it is laid out (collectVtables), as naming
  it does not instantiate it. `S::~S() noexcept(false) {}` of `~S();` says otherwise than its
  declaration, with no specifier what the implicit one would. `~S() noexcept = default;` over a member's
  that may throw has no body to be noexcept: its members go in the destructor helper, which is.
  A debug build's noexcept destructor holding a call that may throw has a frame 8 bytes larger (the
  funclets' UnwindHelp on Windows). Specs language/destructor_*, language/noexcept_*destructor*,
  templates/destructor_*.

- **No implicit fallthrough** (built: kpp's default since C's flip of 2026-11-09; rules decided by kovarex 2026-11-07 on
  thread D's questions, Q5 and Q6 changed on his rule "no keyword that does nothing"; the steps are C's plan of
  2026-11-06). Every group of statements after a label ends with an implicit `break`:
  1. Each label begins a group of its own (kovarex, 2026-11-08, replacing 2026-11-07's "labels side by side share one
     group"): `case A: case B: build();` runs build() for B alone, and an empty `case A:` is a case that does nothing.
     A body several values share is one label, `case A, B, C: build();` (kovarex, 2026-11-08 21:15, as Go's and C#'s:
     C++'s case takes a conditional-expression, so the comma is no comma operator), with the flag and without, each
     value a case of the label's block, a kpp enum's exhaustive switch counting every value; C's converter writes the
     game's runs of labels side by side as such labels (903 of them, joining 3,631 labels). `case A: fallthrough; case B: build();`
     (rule 2) stays legal, and `fallthrough;` stays for a group with statements of its own running on. A value that
     two labels of a switch share, or one label lists twice, is an error, as C++'s duplicate case value ([stmt.switch]
     2; Walker::checkCaseValues: until then codegen gave LLVM a switch of two cases of one value, which it refused).
     The C++ a conversion reads keeps C++'s meaning. Specs kpp/case_label_list, _under_no_implicit_fallthrough and
     _duplicates_is_error.
  2. `fallthrough;` is the last statement of a group with another group after it (Go's rule), a group of `fallthrough;`
     alone too; anywhere else, inside an `if`, in the last group or outside a switch, it is an error. `case A: break;`
     loses its break (rule 5) and is an empty case.
  3. A label nested in another statement (Duff's device) is an error: it means nothing with implicit breaks (J's held item
     closes as refused).
  4. Each group is a scope of its own: `case 1: Foo f; ...` needs no braces, and two groups may declare one name.
  5. `break;` keeps only its early-exit meaning (inside an `if`, in the middle of a group); a `break;` that is a group's
     last statement is an error naming the fix ("every case ends with a break, remove it").
  6. `[[fallthrough]];` is an error: `fallthrough;` alone says it.
  Step 1, built: a case that can reach the next label without saying so is an error, `switch` keeping C++'s meaning
  (Walker::checkCaseEnds, by the statements alone: `break`, `return`, `continue`, `goto`, `throw`, `fallthrough;`, a block
  or both branches of an `if` ending so), so what every case does is written before the meaning changes. Then: the count
  of trailing `break;`s in the game and in kpp's own code, the converter's removal of them (C; it writes `fallthrough;`
  alone), kpp's stdlib, libraries and programs cleaned, and step 2, the flip with the errors above, the game's IR
  byte-identical across it, the evaluator flipped by the specs. Since the flip step 1 runs only on C++ text (the
  C++ a conversion reads, a C++ corpus given to kpp); its spec language/switch_case_running_on_is_error is gone, and
  language/switch_case_ends_written shows kpp's rule.
  Step 2 built behind `--no-implicit-fallthrough` (D, 2026-11-07), every rule as an error exactly as it will be, so
  that the converter and the stdlib are fixed against it. In the code generator and the evaluator a group whose end
  is reached breaks; `fallthrough;` as its last statement runs on into the next; each label begins a group, an empty
  one doing nothing (labels side by side shared one group until 2026-11-08; specs
  kpp/no_implicit_fallthrough_labels_side_by_side_are_two_cases and kpp/no_implicit_fallthrough_shares_a_body_by_fallthrough).
  Each group is a scope of its own: the walker's names, and codegen's objects, destroyed at the group's end; a
  `for constexpr`'s element is its group. The errors: a group's last `break;` (also as the last statement of a block
  that ends it) ends "remove `break;`"; `[[fallthrough]];` ends "write `fallthrough;`, not `[[fallthrough]];`";
  `fallthrough;` elsewhere (inside another statement, in the last group, outside a switch) and a label nested in
  another statement are errors; step 1's "runs on into it" is off. Specs kpp/no_implicit_fallthrough and
  kpp/no_implicit_fallthrough_errors_is_error, built with `--no-std` until the stdlib's trailing breaks are gone.
  While the flag was opt-in, a trailing `break;` in the standard library's files was no error under it (the
  coordinator, 2026-11-08, for C's converter step): its 7 (Time.kpp's parseTime; the switches of Error.kpp, Variant.kpp
  and FilesPosix.kpp end every case with a return) mean the same with the flag and without, and can go only with the
  flip, as step 1 refuses a case that runs on without the flag; so the
  converted game builds with the flag before the flip, as it did for explicit by default (7ab51b62). The program's
  own trailing `break;` was still the error. Gone with the flip, with the 7 breaks (spec
  kpp/no_implicit_fallthrough_standard_library_switches, until then _keeps_the_standard_library_breaks).
  Labels side by side, which mean one case each under the flag, are written as one label of their values where code
  builds with it: the stdlib's 12 (Error.kpp's errcOfWindowsError, `case 2, 3, 15, 53, 67, 123, 161:`) by C, the same
  with the flag and without; the game's by C's converter.
  The converter's step (C, 2026-11-08; FileConverterBodies.inl's walkSwitchBody): a group's trailing `break;` goes, also
  as a block's last statement and as a group's only statement (`case A: break;` becomes the empty `case A:`), and so
  does one a macro's expansion writes where the text proves it ends a case (dropTrailingBreaksInExpansion); C++'s running
  on into the next case is written `fallthrough;`, moved after the brace of a block that ends the case, and none follows
  a macro's use whose expansion ends its case; a `case` label directly followed by another (comments between too, and
  C++'s `[[fallthrough]];` as its only statement) is one label with it (shareLabels, kovarex 2026-11-08 21:15): the text
  from its `:` to the next one's value is `, `, the comments in it go on lines of their own above the label, and in a
  macro's expansion `case a: case b:` is `case a, b:`. Two labels stay apart, the first given `fallthrough;`, beside a
  `default` (`case A: fallthrough; default:`, as `default: fallthrough; case B:`), where a macro's use writes the next,
  for a range (`case 1 ... 5:`), and where a preprocessor line is between them (an `#if` the conversion may drop with
  what follows). In the game's tree (converted from C:\Factorio 4c178a5b915): 903 labels listing 4,534 values, 3,631
  labels joined into them (Permissions.kpp's longest, 297 values on one line); 51 labels keep a `fallthrough;` before
  the next label (23 before a default, 24 after one, 4 across preprocessor lines, trio's and lstrlib's); 278 labels are
  left empty, each a `case A: break;`. One conversion's counts: 3,555 trailing breaks removed and 2,044 in macros'
  expansions, 277 `fallthrough;`s for C++'s running on (164 withdrawn after macros' uses), 43 comments moved above a
  joined label. convert.cmd builds every configuration with kpp's rule, and its switch test (tests\convert_switch)
  runs the converted cases against C++'s output.
  The flip (C, 2026-11-09; the game converted for it, kpp f6ca09d2 and factorio-in-kpp f6343cedb): no implicit fallthrough
  is on (source.cpp) except while converting (FrontEnd.cpp: the C++ a conversion reads is C++). `--no-implicit-fallthrough`
  is accepted and does nothing (Compile.cpp), as `--explicit-by-default` is. A switch of C or C++ text keeps C's meaning
  whatever the rule (SwitchStmt::cRules, set by the parser: an imported C header's inline function, whose switch got
  implicit breaks under the flag until 59e65d9c, a silent wrong answer the game's IR shows it never had; the walker's
  rules and scopes skip it too, so a C++ corpus keeps step 1). The standard library's exemption from the trailing-break
  error goes with its 7 breaks (Time.kpp's parseTime). kpp's own code cleaned with it: libraries/ (mariadb's 7 and
  markdown's 36 trailing breaks; markdown's 3 runs of labels side by side and json's 1 written `case A, B:`, where
  json's tests failed on the meaning they took), programs/kpplex (25 breaks; its 9 single-character punctuators, each
  a `case 'x': break;`, one empty label `case '{', '}', ...:`), 13 specs and 3 test inputs (a `break;` gone, run/control's labels joined; where
  a test reads line numbers, debuginfo/values.kpp, its line left blank; lsp/members' `break;`, its quick fix's
  subject, is a published error now).
  language/jump_past_initialization_is_error loses its case labels past a declaration, and
  language/static_local_constant_initialized_from_the_start its static read through a later case: each case is a
  scope of its own now (rule 4). Found by a temporary error for an empty group before a label over every library,
  program, spec and test (none kept: an empty case is the rule's, the game's 278 among them).

- **Explicit by default** (readme's Planned; decided by kovarex 2026-11-07 on thread D's questions, Q1 and Q3 reshaped;
  D's survey in the coordinator's notes). A constructor or a conversion operator converts implicitly only when marked
  `implicit`; an unmarked one is what C++'s `explicit` one is.
  1. Braced lists (settled by kovarex on the count, 2026-11-07 evening; D-braced-list-arguments-survey.md in the
     coordinator's notes). A function argument's outermost braces name their type, whatever they reach: a constructor,
     an aggregate, a container through its InitializerList constructor, or `{}`: `move(Point{1, 2})`,
     `fill(Vector<i32>{1, 2, 3})`, `reset(Color())`, an error naming the fix ("write `Point{1, 2}`, not `{1, 2}`"),
     not `draw({1, 0, 0})` (kovarex: unreadable, ambiguous beside `draw(Color(1, 0, 0))`, and a new overload
     `draw(const Vector3&)` would break the untouched line). The one exception is a parameter that is an
     InitializerList itself: `max({a, b, c})`. Inside a braced list whose element type is known (an InitializerList's
     elements, a struct's members, an array's elements) nested braces never name a type, at any depth (Go's rule):
     `Values values({{"hello", Color(5)}, {"foo", Color(7)}});`. In `return {x, y};` and a variable's
     `Color c = {1, 0, 0};` the braces stay unnamed: a list of two or more values takes a constructor with no mark,
     and `{a}` needs `implicit`, as `= a` does.
  2. A default constructor is never a conversion: `T x = {};` and `return {};` take an unmarked one.
  3. `explicit` is removed ("we remove explicit to remove a keyword that does nothing, the goal is clean language"):
     plain `explicit` is an error naming the fix ("constructors are explicit by default, remove explicit"), and
     `explicit(cond)` is written `implicit(cond)`, the positive form: `implicit(isConvertible<U, T>) Optional(U&& v)`.
     `implicit` twice, or `implicit(cond)` beside a plain `implicit`, is an error.
  4. A contextual conversion (`if`, `while`, `!`, `&&`, `||`, `?:`) takes an unmarked `operator bool`, as C++'s
     `explicit operator bool`; `switch` on a class needs a marked conversion, as C++ needs a non-explicit one. A copy
     (`bool b = x;`, an argument, a return, an assignment) takes only a marked one (an `implicit operator int()` beside
     it gives the bool; until 2026-11-08 codegen took the unmarked operator bool in every conversion to bool).
  5. Class template argument deduction by copy (`Box b = 5;`) takes marked constructors only, as C++'s explicit guide is
     no candidate there; written guides are as in C++.
  6. Inherited constructors keep their base's mark; a copy or move constructor is never a conversion.
  The order: the counts and the marks first (the converter marks the game, C; the stdlib's `implicit(cond)`s and
  Atomic's `operator T`), then the flip with its errors behind a flag, then the flag as the default, the headline
  staying 0. Built: `implicit(cond)` (kept as the explicit condition `!(cond)`), and `implicit` twice or beside
  `explicit` an error naming the word to remove (2026-11-07, specs kpp/implicit_condition,
  kpp/implicit_condition_errors_is_error).
  Built (D): behind `--explicit-by-default` from 2026-11-07, every rule as an error exactly as it is now, so that the
  converter and the stdlib were fixed against it; the default since 2026-11-08, after C's push of the game converted
  for it (kpp 7a1fab3a). `--explicit-by-default` is still accepted and does nothing, until convert.cmd and the game's
  build.kpp drop it; `--convert` reads its C++ as C++, an unmarked constructor converting. An unmarked constructor or conversion operator is explicit
  (isExplicit), a copy or move constructor (not a template, its one parameter a reference to the class itself) and
  one of no parameters never; plain `explicit` ("remove `explicit`") and `explicit(cond)` ("write `implicit(...)`",
  the condition turned round: a negation's operand, `a && b` for `!a || !b`, else `!cond`) at the word, an
  `explicit(cond)` keeping its condition meanwhile (the game's FixedPointNumberTemplate, unmarked, made 17,000 errors
  of every `MapPosition(1, 2)` where its seven say why); a function
  argument's outermost braces that name no type, of a call, a construction, `new` or a member initializer ("write
  `Point{1, 2}`, not `{1, 2}`", or for a long or multi-line list "write `Point{`, not `{`"). The type is one the
  call can write, for C's converter applies the fixes:
  - as the parameter writes it, a name declared in a namespace or a class qualified by them
    (`CircuitNetworkHelper::VectorOfSignalValues`, `LocaleStringReplacer::Parameters`), each name found from the
    parameter's own scope, so that the fix is the same whatever file is walked first (2026-11-08: a call in an
    out-of-class member definition got the bare name when its callee's class was walked after it);
  - else (a class template's member alias, a template parameter, a template argument written by a name its
    namespaces or classes qualify) the type in kpp's spelling (`Pair<BlueprintRecordID, f32>`, `Vector<i32>`,
    `Vector<agui::AreaAlign>` for a parameter's `Vector<AreaAlign>`; `Box<int>` stays as written);
  - a function's local class by its bare name in that function, where the name is visible (`UpgradeCount{...}`);
  - for an aggregate of one part that is an aggregate itself, given two or more values, the braces brace elision
    dropped (`Array<C, 5>({ {1, 2}, {3, 4} })`, C++20's parenthesized initialization of Array's member: write
    `Array<C, 5>{{ {1, 2}, {3, 4} }}`, as `Array<C, 5>{ {1, 2}, {3, 4} }` has more values than Array has members);
  - for a pointer `nullptr`, or the value of `{p}`;
  - in template code, as its instances decide: one error where they agree, and where the type differs by instance
    (`ScriptObjectHelper<T>::SelectAction({a, b})`, a specialization's parameter each) no fix, each instance naming its
    own, for a hand edit.
  A list of two or more values that no constructor takes, for want of its elements' conversions to the InitializerList's
  type, is that conversion's error (`Box<Strict> b = {5, 6};`); `{a}` in a copy-initialization needs `implicit`,
  `{a, b}` and `{}` do not. The explicit checks run in template code too (a template's own walk for what is not
  dependent, each instance for the rest), and a requirement's probe of an assignment the value reaches by no implicit
  conversion is not met (`requires { t = u; }`, which Vector's assign and insert ask). A switch on a class and class
  template argument deduction by copy take marked conversions only. The stdlib alone gave 87 errors under it (72 plain `explicit` on
  constructors, 11 on conversion operators, its 4 `explicit(cond)`): its `explicit(cond)`s became `implicit(...)`, and
  K's 7d9fd9ac removed its plain `explicit`s with the flip (until then those errors skipped the standard library's
  files: the coordinator's decision (a), 2026-11-08, so that the converted game was built with the flag before the
  flip, C's converter push and the flip staying separate steps). The specs, the libraries, kppsite and the end-to-end
  tests lost their `explicit`s with it (`explicit(cond)` written `implicit(...)`), and gained `implicit` where they
  convert on purpose. A conversion through a conversion operator without `implicit` is the checker's error too, as
  one through such a constructor (D, 2026-11-08, where codegen said it did "not support" the conversion, the
  constructor or the operator): a copy-initialization (`Target t = s;`, an argument, a return, `int n = s;`) and a
  non-const reference bound through `operator Base&` name the direct-initialization (`Target(...)`, `int32(...)`,
  `static_cast<Base&>(...)`); a built-in operator's class operand (`1.0 + value`, `kind = result`, `values[index]`,
  `index[values]`) "the built-in + takes Value only by its conversion operator to double"; a call of an object
  through its conversion to a function (`caller(4)`) likewise. Spec kpp/explicit_by_default_conversion_operators_is_error,
  kpp/explicit_by_default_conversion_operator_operands_is_error. An overloaded call that no candidate takes is still
  codegen's "a call that did not resolve", whatever the arguments (`k(&c)` with `k(int*)` and `k(double*)` too). A function argument's braces for a parameter that is a reference to an array name a type C++
  cannot write as `T{...}` (`const int[3]{...}`): the error has no fix clause, and an alias's braces
  (`using Three = const int[3]; sum(Three{1, 2, 3})`) are the one way left to pass a temporary array, which codegen
  does not make yet (pending language/brace_list_binds_const_array_reference; an open question for kovarex whether
  such a parameter's braces stay unnamed, as an InitializerList's do). Not refused, for kovarex: a braced list as an operator's operand (`x = {..}`, 301
  in the game; `m[{..}]`); `explicit` on a deduction guide stays, written guides being as in C++ (rule 5). Specs kpp/explicit_by_default, kpp/explicit_by_default_marks_is_error,
  kpp/explicit_by_default_braced_arguments_is_error, kpp/explicit_by_default_conversions_is_error. Two C++ rules it
  leans on were missing and hold without the flag too: class template argument deduction by copy takes no explicit
  constructor's guide ([over.match.class.deduct] 3; `Box b = 7;` took it), and a switch on a class takes no explicit
  conversion operator ([stmt.switch] 2; the checker's error, codegen's "does not support" before); specs
  templates/class_template_argument_deduction_by_copy_takes_no_explicit_constructor_is_error,
  language/switch_on_class_with_only_an_explicit_conversion_is_error.
  The converted game's unneeded marks go (kovarex, 2026-11-09: the converter keeps `implicit` only where it is used):
  `--implicit-uses FILE` lists each mark with its uses (the places codegen converted through it) and how often whether
  it converts implicitly was asked (isExplicit: every conversion, overload candidate, requires clause and trait, whatever
  the answer, so one asked about keeps its mark even when no conversion took it); one never asked about changes nothing
  when it goes. The asked counts vary with -j (cached answers); their 0 does not, nor the uses. Spec
  kpp/implicit_uses_lists_each_mark. The converter's step (C, 2026-11-09): programs/factorio/implicit-dropped.cmd builds
  the converted checkout with `--implicit-uses` in 15 builds (Windows, Linux and FreeBSD; debug, release and final; with
  and without the tests, final only without, as the C++ tests do not compile there), since code a build leaves out by a
  false `when` asks nothing in it, and implicit-dropped.py lists each mark no build asks about in
  overrides/implicit-dropped.txt, by file, class and parameter names (two marks sharing a key go only if both can);
  the converter then leaves those out. A new or changed declaration is not on the list and keeps its mark, so the list
  can only be stale in the safe direction. In the game (C:\Factorio 4c178a5b915): 1,490 listed, 1,550 marks gone, 954
  keys kept. Its IR in all 15 builds is the same before and after but for the names that carry a token number (a
  local class's, a static local's, a specialization's) and one assert's column.

- **The noexcept operator** (D, 2026-11-06, [except.spec] 6) answers for any operand: what it names and what it
  calls without naming it (Walker::canThrow): a class's operators, constructions, implicit and defaulted
  constructors by their parts and default member initializers, a by-value parameter's copy or conversion, a
  conversion operator, default arguments, a temporary's destructor, new (an allocation function that may throw,
  unless it is a placement one), a dynamic_cast to a reference that needs a check, typeid through a pointer. In a
  template's own walk it stays undecided; a noexcept-specifier's `noexcept(noexcept(a = b))` is answered under
  the instance's arguments, by a copy of the expression typed under them (the evaluator's NoexceptExpr, as
  typeOfSignatureExpression does for decltype). A lambda expression copies what it captures by copy where
  it is made (a variable's copy from it as an lvalue, an init-capture from its initializer) and its closure,
  a temporary, is destroyed (2026-11-07). It still refuses, as not decided, what it cannot see: a comparison
  C++ rewrites, a braced list as an argument, a lambda whose captures are not known, a fold. Specs
  language/noexcept_operator_*, clang's output.

- **A member function's qualifiers are part of its pointer's type** (D, 2026-11-06, as C++: a silent
  wrong answer fixed). `R (K::*)()`, `R (K::*)() const`, `volatile`, `&` and `&&` are five types
  (TypeTable::function's memberQualifiers, printed in the type); they were one, so a second partial
  specialization or overload on them was never chosen. As with noexcept, declaredTypeOf stays the
  signature and a function's name carries them (namedFunctionType); no conversion joins them. A
  pointer to a member function is a template argument too (`template<int (A::*M)()>`), by what
  `&A::f` is, a thunk for a virtual one. Specs templates/member_function_*.

- **Wube's utility code in kpp (parked)** (kovarex, 2026-11-06): once Factorio is officially in kpp, so
  that the game's code changes along with the stdlib, Wube's handwritten utility code moves, not a
  fresh implementation: PropertyTree and a generic versioned Serialiser/Deserialiser into the stdlib,
  decoupled from Factorio. No code or design write-up before then. Open until then: the licence label
  of the moved code, stdlib or libraries/ for each piece, PropertyTree's node size, JSON numbers (double
  or integer), which pieces come next.

## Roadmap

**Stages (kovarex, 2026-11-06).** These are the project's two stages, not the target work's numbered
ones under "Targets and hosts".
- **Stage 1, now:** everything planned that makes Factorio a proper example with the full features,
  meaning all the language features and good compile times, so that kpp can be integrated into
  Factorio's code to showcase its strong points.
- **Stage 2:** starts once kpp is a branch in Factorio: PropertyTree, kpp.net and other things.

The readme's "Planned" lists what is in each. Placed by this decision:
- Named arguments, `both_const` and `class_const` (their details not decided yet) and the localisation
  library are stage 2's.
- macOS as a host is under consideration, not decided; macOS as a target stays in stage 1, built on
  Windows and Linux ("Targets and hosts"). Linux as a host is done (2026-11-06).
- The library registry is under consideration, not decided, and not for stage 1.

The work so far, in order:

1. **Speed-test prototype** (done, see [prototype-results.md](prototype-results.md)): parallel
   lexer + parser + order-independent name resolution, benchmarked on Factorio. Whole front end
   in 0.32 s for 1.33 M lines on 32 threads. No code generation.
2. **Type checker on top of the resolver** (stages 1-4 done 2026-10-23, see
   [prototype-results.md](prototype-results.md)). Staged so that each stage is measurable on
   Factorio:
   1. Canonical types: interned, hash-consed in one thread-safe table, so that two threads that
      build the same type get the same pointer (the type-level form of "generate each
      instantiation once").
   2. Declared types: AST types of variables, parameters, members and functions converted to
      canonical types, with template parameters substituted from the enclosing instantiation
      (`Vector<int>::operator[]` returns `int&` without copying the template's AST).
   3. Expression typing in all function bodies: literals, names, `this`, calls with overload
      selection by arity, operators (built-in rules and overloaded operators by lookup), casts,
      `new`, `auto` deduction, range-for element types, lambdas (return type from the body; an
      `auto` parameter makes the lambda a template whose call operator is typed per call, as in C++).
   4. Member accesses through the object's type: this binds the accesses the resolver had to
      defer and is the first real use of class template instantiation.
   5. Checking template bodies per instantiation, exactly once, with the produce/wait protocol
      from the proposal (done 2026-10-23: 4,100 instantiations, 17,000 bodies, under 0.1 s of
      CPU on Factorio).
   6. Overload resolution with conversion ranking (done 2026-10-23; the library's `append`
      overloads depend on it). Later: members through templated bases, constexpr evaluation,
      per-instantiation diagnostics and expression types.
3. Compiler that runs real kpp programs via LLVM (started 2026-10-23: the v0 subset compiles
   through textual IR and clang, see prototype-results.md "Stage 4"; constructors, destructors,
   copies, operators, templates and the standard library seed `stdlib/core` with `String` and
   `Vector<T>`, virtual functions, lambdas, `HashMap`, `Optional` and `toString`/`println`
   and variadic templates with a type-safe `format` followed the same day, then the C header
   importer: `extern "C" import` of the UCRT and `windows.h`, see "C header import" in
   prototype-results.md; the first real program, `programs/kpplex`, the kpp lexer in kpp, runs
   as fast as its C++ transliteration on the Factorio corpus, 2026-10-24). Real programs live
   under `programs/<name>/`.
4. Macro replacements (section 7), the small ones first because real programs and the converter
   need them: `assert`/`release_assert`, caller location, `conditional` functions, `config`
   constants with `if constexpr` at every scope, `_` locals, attributes; then the ones that need
   design work: reflection, `for constexpr`, typed unions, real enums, tests.
5. Backend at Factorio scale (first measurement 2026-10-24, see prototype-results.md "Backend at
   scale": for 73 k functions LLVM needs 5 s at -O0 and 109 s at -O2 on one thread, kpp 0.84 s
   for everything before it). Done the same day: one IR module per group of files, compiled by
   clang in parallel, -O0 for debug and -O2 for release: 0.6 s and 3.8 s of LLVM for the 73 k
   functions on 32 threads, a debug build of 615 k lines in 1.7 s; unchanged modules keep their
   object files, so a one-line change rebuilds in 1.25 s (0.93 s of it single-threaded IR
   writing). Next: write the IR in parallel too; cross-module inlining; in-process LLVM instead
   of clang.exe when the textual round trip shows up in the numbers.
6. Persistent compiler + IDE integration: the one-line-change rebuild (16 s in Factorio today).
7. Converter from Factorio-style C++ to kpp (needs 4).
8. Precise const typing (scheduled 2026-10-29 for after the converted game's first run reaches
   the main menu): a const value is never assigned, as in C++ (spec language/const_value_assigned,
   pending; kpp accepts and stores `r = 5` through a `const int&` today). A checker error at every
   built-in assignment to a const value flagged about 60 valid lines of the game and the library,
   which is how imprecise the walk's constness is: Array<const char, N>::fill (instantiated,
   where C++ instantiates only the members used), DrawCommandBatch's and SpriteDrawOrder's vertex
   writes (about 50 lines), AsteroidDrawCommandBatch, EntityUsageBitMask's BitMaskStorage, and the
   imported C headers' ReadAcquire/WriteRelease bodies (`const volatile`). Those are to be typed
   right first, then the error turned on.
