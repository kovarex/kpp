# Kpp design

Kpp is a C++-like language with a new compiler built around one idea: **no includes at the
deepest level of how the compiler works**, so that builds are orders of magnitude faster.
The original proposal's compilation model is [compilation/index.md](../compilation/index.md); its
language changes are in [readme.md](../readme.md), "The language", each with its state.

This file records decisions that have been made. When a decision changes, update it here.

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
  that order across files (as C++ across translation units). Every one is built, whether
  anything reads it or not (an initializer that registers something, as C++ runs it): globals
  of namespaces and static data members of classes that are no templates; a static member of
  a class template is built when an instance of it is used, and every one of an instance an
  explicit instantiation names (`template class LuaStruct<MapSettings>;`, as C++ instantiates
  all its members), built where the explicit instantiation is written, in the order of the
  members' definitions. A `thread_local` (a global, a
  static member or a local) is one object per thread, built by each thread at its first use
  (a local at its first pass), not at startup; none is destroyed at a thread's exit yet.
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

- **Inlining across chunks** (decided 2026-10-28, to be done later): kppc hands clang the program
  in chunks (IR modules) compiled in parallel, and a non-template function's body is in one chunk
  only, so calls from other chunks cannot be inlined (up to 35% on small String methods,
  docs/stdlib-performance.md). The options were (1) ThinLTO at link time, (2) available_externally
  copies of small functions in every chunk that calls them, (3) grouping callers with callees into
  chunks, (4) kppc inlining trivial functions itself when it emits IR. kovarex: (1) or (4); (2)
  rejected, since the copies grow with the number of chunks. One chunk (`--modules 1`) remains the
  manual extreme for a release build.
- **Floating point is strict IEEE** (the game builds with /fp:strict for its determinism): kppc
  emits no fast-math flags and no `contract`, and clang compiles the modules with
  `-ffp-contract=off` for the baseline x86-64 target (no FMA), so no multiply-add is fused and
  every operation rounds as written.

Expected limit: once headers and duplicate instantiations are gone, LLVM optimization
dominates release builds. The big wins are debug builds and incremental rebuilds; release
builds win less unless machine code is cached per function.

### 3. Simple templates
- Templates are for generic types and functions (`Vector<T>`, `max<T>`), not a
  metaprogramming language.
- Kept: type and value parameters, variadic parameters, full specialization,
  `if constexpr`, constraints written as plain conditions (`requires`-like).
- Dropped: SFINAE / `enable_if` tricks, template metaprogramming as computation,
  `typename` / `template` disambiguators.
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
- Variable templates (2026-11-02): a use is the instance's value, the initializer evaluated for
  the use's arguments. Its object (`&hasBegin<C>`, a reference or a Text piece bound to it) is a
  constant global per argument list holding that value; a builtin trait (`isSame<A, B>`) has none,
  and a reference binds a temporary. A variable template that is not a const value is a compile
  error until it gets its own storage per argument list (one global named by the template alone
  read `hasBegin<Vector<int>>` as false).
- Instantiation depth (implementation limit, 2026-10-28, open to change by kovarex): a function
  instantiation requested through a chain of more than 64 instantiations, each asked for by the
  previous one's body, is an error naming the last two. Without a limit, a template that
  instantiates itself with ever larger arguments never stops, and every step costs more because
  the names grow (the first case, from a wrong deduction, ran for over ten minutes). C++ allows
  1024 nested instantiations, but counts fewer of them (a function body's calls are instantiated
  later, not nested); kppc counts every link of the chain, and the whole Factorio source needs 12.
  Templates as computation are dropped (above), so deep recursion is not expected.

### 4. Our own standard library
- No C++ std. A smaller, saner library written in Kpp: one UTF-8 `String` with small-string
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
  _tuple_size_mismatch.)
- Each library type that a std type converts to keeps the guarantees the C++ standard gives that
  type (iterator, pointer and reference invalidation, complexity, the meaning of return values),
  since converted code relies on them. A faster type that breaks one is a separate, explicitly
  named type (FastMap next to Map). The rules are written out, self-contained, in docs/stdlib/
  (the specification); docs/stdlib-conformance.md lists where the code does not meet it yet.
  (kovarex 2026-10-28.)
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
  `kppc --config sizeBits` switch were removed (the option is now an error). String is 32 bytes,
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
- C constructs accepted for this: unions (one block of storage, every member at offset 0),
  anonymous struct/union members, `typedef struct { ... } Name;`, `(void)` parameter lists,
  MSVC's `__intN`, calling conventions and `__declspec`. Imported declarations never appear in
  parse statistics or diagnostics unless `--errors` is given.
- MS C's anonymous member of a named struct type (`MONITORINFO;` inside winuser.h's
  MONITORINFOEXW, whose C++ version derives from it): as a struct's first data member it becomes a
  base at offset 0, as C++ has it, so the layout is C's and its members are the struct's; anywhere
  else it is a member with a name of its own (the layout stays right; its members by their names
  are an error). (2026-10-29, spec interop/c_anonymous_member_of_named_type.)
- A C function a header declares or defines and Kpp declares `extern "C"` with its parameters is one
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
Real enum class with methods/str()/iteration, `this` as a reference, required `this->`,
required `override`, explicit by default (`implicit` keyword), no implicit switch
fallthrough, exhaustive enum switches (abort on invalid value), typed union, `?->`, `?:`,
`both_const` / `class_const`, named parameters, test tools in the compiler, `accumulate`
methods (the Factorio survey suggests generating save/load from the class may beat it).

### 7. No preprocessor; what replaces macros (decided 2026-10-23)
Based on [macro-survey.md](macro-survey.md): Factorio's 611 macros and 22,647 expansions fall
into eleven cases and none of them needs textual substitution, so Kpp has no preprocessor. The
replacements, in the order of how much code they remove:
1. Tests and `check` in the compiler: `test "name" { ... }` declarations the compiler collects
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
5. Typed unions (`union enum`, section 6).
6. Real enums with `str()`, `fromStr()`, `count` and iteration (section 6). Done 2026-10-24
   except `fromStr()`, see 8.4.
7. `_` as a name that may be declared any number of times in a scope, each an unnamed local that
   lives to the end of the scope. Done 2026-10-24.
8. Attributes for the real compiler modifiers: `[[noinline]]`, `[[forceinline]]`,
   `[[vectorcall]]`, `[[optimize]]`, `[[no_sanitize]]`. Done 2026-10-24 for `noinline` and
   `forceinline`; the others are accepted and ignored for now.
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
switch (d) { case North: ... }         // exhaustive, no default; an invalid value aborts
```
Inside the enum's methods `this` is the value and the enumerators are in scope unqualified.
Comparison, copying and `switch` work on the value; the underlying type is explicit or `i32`.
Done 2026-10-24: the brace block, methods with `this` as the value, `count`, `values`, the
generated or hand-written `str()`, `switch` without `default` that must name every value (a
compile error otherwise) and aborts on a value outside them; `nameof(x)` from 8.3. A plain C++
`enum class` without the inner block keeps the C++ rules. Done 2026-10-25: `fromStr(const char*
text)` returns `Optional<E>`; the compiler declares it on every Kpp enum and lowers it to the
library's `enumFromStr<E>(text)` (stdlib/core/Optional.kpp), which walks `E::values` comparing
`str()`; a `String` passes its `c_str()`, and a hand-written `fromStr` replaces the generated one.

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
Done 2026-10-25: `union enum` with methods; `p[Integer] = v` (destroys the alternative held, sets
the tag, constructs the value), `p[Integer]` (a release_assert on another tag), `p == Integer`,
`switch (p)` (exhaustive, as for a Kpp enum), copying, assignment and destruction following the
tag. Decided on the way: a default-constructed union holds its first alternative,
default-constructed; the tag type is the nested Kpp enum `Property::Tag` (so `tag.str()` and
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

**8.6 Tests in the compiler.**
```
test "Direction::isVertical"
{
  check(Direction::North.isVertical());
  check(Direction::East.str() == "East");
}
```
A `test "name" { ... }` declaration may appear at namespace scope or inside a class (where it
sees private members). `check(expr)` records a failure with the expression text, the location
and the operand values and continues; `require(expr)` ends the test. Tests are compiled only
with `kppc --tests`, which generates the runner (`--run-test <name or file>` selects); without
the switch they cost nothing. Fixtures are ordinary classes constructed in the test.
Done 2026-10-30 (thread G; the runner's details in "Running and testing Kpp code"): `kppc --test
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

### 9. Exceptions and RTTI (decided 2026-10-24)
- Kpp has exceptions, with C++'s `throw`/`try`/`catch` semantics, implemented on LLVM's
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
- The vtable is MSVC's and COM's (2026-10-29): the object's first word points at the first
  virtual function's slot, the slots follow in declaration order (an override takes its base's
  slot), `this` is the first argument and a class result goes through a hidden pointer after it.
  The type descriptor is the slot just before the first function (where MSVC keeps its RTTI
  locator), and the slot before it holds the offset of the subobject whose vtable pointer it is
  within the whole object (0 for the first one). So a COM interface (ID3D11Device) can be declared
  as a Kpp class with its virtual functions in the header's order, and a C table of functions
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
  that type (2026-10-29, spec classes/bitfields_of_one_size_share_a_unit).
- Implemented 2026-10-24 (throw, try/catch, rethrow; `dynamic_cast` not yet) on Windows SEH
  rather than the MSVC C++ ABI: `throw` builds the object on the heap and raises an SEH exception
  (code 0xE0000001) carrying the object, its type descriptor and its destructor; every `try`
  gets a filter function that searches the descriptor's bases for each handler's type in order
  (so `catch (const Base&)` works, with the base-subobject offset applied); destructors between
  the throw and the handler run in cleanup funclets; a handler owns the object and destroys it on
  exit unless it rethrows. Calls that may throw become `invoke`s only where something has to
  happen on the way out, so code without destructible locals and without `try` is unchanged.
  Uncaught exceptions print `unhandled exception of type T: message` and abort. Rules that follow
  from this: destructors do not throw; `extern "C"` functions do not throw (calls to them stay
  plain calls); an exception cannot pass through a C callback frame that was not compiled for
  unwinding; `catch` by pointer matches the pointee type's spelling only (no base conversion);
  `new T(args)` leaks the memory when the constructor throws (to fix with the allocator work).
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

Kpp evaluates operands in the order C++17 sequences them, so that converted code means what it
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
- `new` allocates before it evaluates the initializer's arguments ([expr.new] 19). (A class's own
  operator new or operator delete is not supported yet, and declaring one is an error:
  classes/class_operator_new_is_not_supported_yet.)
- Otherwise C++ leaves the order unspecified (a call's arguments among themselves, the operands
  of `+`), and so does Kpp: code must not depend on it.

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

### 12. The build description: build.kpp (decided 2026-10-31)
A project's builds are described in `build.kpp` at its root: Kpp, run by the compile-time
interpreter, so it can compute but only describes (no I/O, no processes, no C). The project's code
is in `src/`, its own libraries and forks in `libraries/`; registry libraries stay in a shared
cache, pinned in `build.lock`. One project may build several executables from shared sources, a
target leaving files out (Factorio's `Main`) rather than having its `main` replaced. With no
build.kpp, `src/` is one executable with debug and release. Writing `project.use("json")` adds a
library: the next build finds it (`libraries/`, cache, registry) and records it in build.lock. In
the project's directory the compiler alone builds it (`kpp`, `kpp --release`). A Kpp library is a
project without an executable: its build.kpp says `project.library(NAME)` with its version,
license, minimum kpp, source roots and test roots, its sources join the program of whatever uses
it, and `kpp --test` runs its tests (build.md, "Kpp libraries"). Details, the research behind it
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
- **Kpp's integer and floating types are the built-in names** `u8 u16 u32 u64 i8 i16 i32 i64` and
  `f32 f64`. They are the standard way to write a number in Kpp. Each is one of C++'s types under
  another name (`u32` is `unsigned int`, `i64` is `long long`, `u8` is `unsigned char`, `f64` is
  `double`), so C interop, layouts and overloads are C++'s: a converted `f(int)` beside
  `f(int32_t)` is one function.
- **C's names are for C interop.** These are `int`, `unsigned`, `short`, `long`, `long long`,
  `signed char` and `unsigned char` as numbers, and `int8_t` ... `uint64_t`, `size_t`,
  `ptrdiff_t`, `intptr_t` and `uintptr_t`. They are kept, with C++'s meanings, for the imported C
  headers and `extern "C"` declarations and blocks (a binding's aliases and C struct mirrors go
  there too). Elsewhere they are deprecated: `kppc --c-type-warnings` warns at each one, naming
  Kpp's type, and is never an error. It is opt-in until the converter and the standard library
  have moved, then on by default; whether it later becomes an error is kovarex's call.
- **The types are C++'s, distinct** ("Distinct fixed types, deprecate long"). `char`, `signed char`
  and `unsigned char` are three types, and `int`, `long` and `long long` are three (the Kpp v0
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
  kppc would add to the link line. No Windows C API takes one, so calls between Kpp functions are
  Kpp's own. On Windows x64 clang passes an i128 argument by pointer and returns it in xmm0, and
  LLVM's own library calls followed another convention until recent fixes (LLVM issue 136428).
  So kppc calls compiler-rt only through clang's lowering, never declares such calls itself, and a
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

Each rename changes no type, so the IR is unchanged. C's checks are the ones in
C-integer-names-plan: map the new names back and get the old conversion byte for byte. C names
stay in the imported headers, the kpp-native shims (C) and `extern "C"` declarations. The standard
library (C's) writes `int` 983 times, `size_t` 99, `long long` 98, `ptrdiff_t` 92, `unsigned long`
84 (Win32 bindings, which belong in extern "C"), `uint64_t` 62, `unsigned long long` 48 and
`uint8_t` 41.

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
  - Microcontrollers (Cortex-M) and old 32-bit ARM Linux, which Kpp does not target.
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
- **Unsigned traps, and how Kpp can help.**
  - `for (u32 i = n - 1; i >= 0; --i)` never ends.
  - `size() - 1` of an empty container is the largest value.
  - `a.size() - b.size() < 0` is never true.
  - A signed/unsigned comparison converts the signed side.

  Kpp can warn of each: a comparison that is always true or false on an unsigned (`x >= 0`,
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
- **The measurement** (H, kppc fe933d5b on Factorio 4c178a5b915; method and tables in
  [test-matrix.md, "Container sizes: u32 against u64"](test-matrix.md#container-sizes-u32-against-u64-kppc-fe933d5b-factorio-4c178a5b915-2026-11-01)).
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
streams are not ideal either. Kpp takes the form most languages chose (Pascal's `writeln`,
Python's and Lua's `print`, Go's `fmt.Print`, Nim's `echo`, Abseil's `StrCat`): the pieces as
arguments, one after another, with nothing parsed.

- **Calls:** `print("Hello ", name, " you are ", age, " years old");`. `print(someText)` prints the
  text as it is, always. `println` is `print` with a line end. There is no `printf` in Kpp's
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
- **Converted code is Kpp too** (kovarex, 2026-11-01: "converting code to kpp is getting kpp code"):
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
  `%E` `%G` `%a`, precision on integers and strings, wide strings: none in these calls). Next: the
  game's own format functions (Logging, TagWriter, addDebugProperty, makeProperty, AbortException::make)
  taking a `Text`, std::to_string of a floating value, and the C libraries' printf calls.

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
- In the IR a private entity's symbols and types carry its file (`inflate.kpp@<hash of the
  path>::helper`), so two files' private functions, globals, classes, vtables and template
  instances of one name stay apart.
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
  library's scope, section 16, once the converted libraries are scoped).
- C++ (the game): a source file's `static` functions and variables at the global namespace's scope
  and its anonymous namespaces at that scope, unwrapped, their declarations each `private` (Atlas.cpp's
  helpers, AppManager.cpp's ConfirmationBoxConnectToServer): 93 declarations, the 7 anonymous
  namespaces. Not in a named namespace (private is no Kpp there) nor in a header (other files
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
description (done: kpp passes each used library's roots as kppc's `--library-root NAME DIR` or
`--unscoped-library-root NAME DIR`, build.md "Library scopes"); thread C the stdlib marked unscoped and the converter's qualification; thread I the
registry's docs. This supersedes the "distinct names" answer below for libraries; the package tree
for browsing stays as proposed.

As built (thread A, 2026-11-02; specs `kpp/library_*`, `kpp/two_libraries_*`, `kpp/unscoped_*`,
`kpp/using_namespace_of_a_library_is_error`):
- kppc takes `--library-root NAME DIR` and `--unscoped-library-root NAME DIR`, both repeatable (one
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
- **`format` with a literal format:** through C++20 `consteval`, which kppc now enforces. A call
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
self contained package". A Kpp release compiles and links a program on a Windows machine with
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
  base, macOS later), so `--target` works from Windows, Linux or macOS alike; tests still run on the
  target.
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
- The build works it out (compiler/version.cmake) and puts it in kppc, with the commit. A build
  with uncommitted changes adds "+dirty". A build outside git (a source zip) says "unknown" rather
  than guessing. A shallow clone that lacks VERSION's last change is an error, so CI fetches the
  whole history.
- `kpp --version` prints it. A release is named by it (tag v<version>): package.ps1 takes it from
  the kppc it packages and never from its command line (kovarex: "take the version from the
  versioning system always, not from command line").
- VERSION starts at 0.2: CI had already published v0.1.141 to v0.1.146 under the run-number
  scheme, and 0.2.0 sorts above them.

docs/build.md, "Versions", has the details.

## Code packages (proposed 2026-10-27, not yet decided by kovarex)

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
- **An optional `package.kpp` in the directory names and describes it**, for the tree's
  display. Without it the directory name is used.
  ```cpp
  // stdlib/containers/package.kpp
  package "Containers"
  {
      description = "Owning sequences and maps: Vector, HashMap, HashSet, UniqueArray.";
      order = 1;   // position among its siblings in the tree; alphabetical when absent
  }
  ```
  `package.kpp` may contain only this declaration. It has no effect on compilation other than
  appearing in reflection and diagnostics.
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
- `kppc --packages` prints the tree with the declarations in each package (for the command line
  and for scripts), and diagnostics name the package along with the file.
- Reflection: `packageof(T)` gives a constant descriptor (`name`, `description`, `parent`), so
  for example the test runner can run all tests of one package (`--run-test "Factorio > Train"`).
- Cost to the compiler: the package is a field of the file, found once when the file list is
  built. Name lookup does not look at it, so it changes nothing in the front-end numbers.

### Packages for the converted Factorio and the standard library
- The converter keeps the source directory layout, so Factorio's own directories become its
  packages with no extra input: `src/Entity/Inserter.cpp` goes to *Factorio > Entity*, the 66
  directories under `src/` (AI, Belt, Blueprint, CircuitNetwork, Entity, Gui, Train...) are the
  second level, and the 340 files directly in `src/` are in *Factorio* itself until someone
  sorts them. CommonUtil and Agui are top-level packages of their own. The converter writes a
  `package.kpp` only when it has a description to give. Factorio's namespaces (`agui`,
  `ActionData`, `MTL`...) stay namespaces, because the code refers to them qualified.
- `stdlib/core` (23 files) would split into directories: *Containers* (Vector, HashMap,
  HashSet, UniqueArray, InitializerList), *Text* (String, StringView, WString, Format),
  *Memory* (Memory, UniquePtr, SharedPtr), *Functional* (Function, Algorithm, Hash, Utility,
  Optional), *Math*, *Threads* (Thread, Time) and *Language* (TypeInfo, Location). The move
  changes no code, since no name changes.

### Compared with C++ namespaces and Java packages

| | C++ nested namespaces | Java packages | Kpp code packages |
|---|---|---|---|
| Part of a name | yes, `std::chrono::seconds` | yes, `java.util.List` | no, only `Vector` |
| Needs context to use a short name | `using namespace` / `using` | `import` per file | never |
| Same name in two places | allowed, disambiguated by qualification | allowed, disambiguated by import | compile error |
| Declared by | `namespace x { }` blocks, anywhere | `package a.b;` in each file, must match the directory | the directory, plus an optional `package.kpp` |
| What it is for | avoiding clashes, ADL | avoiding clashes, access control (package-private) | browsing and navigation only |
| A snippet means the same in any file | no (depends on usings and the enclosing namespace) | no (depends on imports) | yes |

C++ namespaces and Java packages solve name clashes by making names longer and then let each
file shorten them again, so the meaning of a short name depends on the file it is in. Kpp
packages keep the hierarchy those systems give for browsing and drop the part that makes names
contextual; clashes are prevented by the compiler refusing them instead.

## struct and class (decided 2026-11-01: option 3, with friend)

**Decided** (kovarex, 2026-11-01): option 3, two keywords with a checked meaning ("I like 3, so we
keep struct and class, and just means more than before"), and `friend` stays ("for now lets keep
friend"). A struct deriving from a polymorphic class is no value type: it must be a class. Order:
the struct rule first (the converter turns the 31 game and test structs into classes), then access
checks as warnings over the converted game, errors once it is clean; the converter keeps the
game's 438 friend declarations. The analysis follows.

**As built (thread A, 2026-11-02): the struct rule, behind `kppc --struct-rule` until the converter
writes the game's breaking structs as classes.** The parser records a `private:` or `protected:`
section in a struct ("a struct's members are all public"), a private or protected base ("a struct's
bases are all public") and a virtual function, pure function, override or virtual destructor ("a
struct has no virtual functions"); the resolver adds a struct deriving from a polymorphic class
directly or through a base ("struct Label derives from Widget, a polymorphic class"), each "(a struct
is a public value type, design.md "struct and class"): make it a class". Under the flag they are
errors; without it nothing changes (`--access-warnings` still warns of them). Imported C structs,
unions and typed unions are exempt. Constructors, a destructor, methods, operators, static members,
nested types, a `public:` label, friend declarations and public bases that are not polymorphic stay
allowed, in templates and local structs too. The converted game breaks it 37 times (31 in src, 6 in
its tests: 26 + 5 sections, 1 + 1 virtual functions, 4 polymorphic bases); the standard library's
ExceptionHolderBase, ExceptionHolder, OutStream and OutFileStream are classes now. When the flag
becomes the default, 40 spec snippets and 8 of run_tests' programs that use structs with virtual
functions become classes too. Specs classes/struct_private_member_is_error,
struct_virtual_function_is_error, struct_base_not_public_is_error,
struct_deriving_from_polymorphic_class_is_error and struct_is_a_public_value_type. Friend
declarations of classes and functions work (classes/friends, friend_declarations); a friend function
defined in its class (a hidden friend) is not found yet (pending classes/hidden_friend_functions).

**The question** (kovarex): why are the standard library's types structs (`struct String`), and
what do two keywords buy when the only difference is that a struct starts public? He uses
`struct` as a hint to the reader that a type is small, mostly data, and `class` for a real object.

**Today** the two differ only as in C++: a class's members and bases start private, a struct's
public. Kpp records access but checks it nowhere (its one use is class argument deduction's
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

**Measured with the checker (2026-10-30).** `kppc --access-warnings` runs option 3's rules as
warnings only (off by default, no other change: the game's IR is the same byte for byte; on, it
adds 0.06 s to the 1.4 s resolve of the converted game and its tests). It reports every use of a
private or protected member outside its class and the classes around the use, the classes derived
from it (protected) and its friends, and every struct with a private or protected member, a base
that is not public or a virtual function. Constructors are not checked yet: the checker does not
choose them, codegen does. Run over the converted game with its tests (scratch conversion of
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
  MinHeap's items by the heap 24. The converter removes every friend declaration today ("Kpp has
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
  `friend` in Kpp (friend classes; friend functions are used twice) the converter keeps the 438
  declarations as written and the program checks clean but for the 2 operators, which become
  members. Without it, each of the 259 relationships becomes something else (public members, an
  accessor, the partner nested in the class), in 177 classes, by hand.

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

*What it evaluates.* Ordinary Kpp, on the typed tree (an instantiation's own typed copy, so
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
- Not evaluated: what has no Kpp body (C functions, intrinsics, the C library's math), `new` and
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
Kpp body", "reads the global gameTick, which is not a constant", "divides by zero (Math.kpp:12)",
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

*Step 1, done 2026-10-29.* compiler/src/consteval.cpp holds the values (ConstValue: an integer
normalised to its type's width and signedness, an enumeration with the underlying type it was
declared with, bool, char, float or double) and C++'s arithmetic on them (ConstArith: literals
with every suffix and escape, integral promotion and the usual arithmetic conversions, modular
conversions between integer types, shifts in the promoted left type, IEEE `+ - * /` and
comparisons), in two modes: *Required* for the checker, where a signed overflow, a division by
zero, a shift by the width or more, or a float out of an integer's range fails the constant with
its reason (static_assert reports it: `static assertion whose condition is no constant: signed
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
constructor that is not constant, Debug tools' values); `sqrt` (the library's has no Kpp body
yet); float template arguments. The library's gcd and lcm can go back to loops. A namespace-scope
`Pair<int, Mode> five(a, m);` with variables as arguments is parsed as a function declaration
(the parser's guess there; the walk and the code generator see no variable either). The game's 9
static_asserts left need: VariantAlternative over a variant
(SegmentedUnitAI, 5), `sizeof` of a member through a reference (`maskUniform.uvTransform`) and of
another class's member (`GraphicsInterface::orthoProjectionMatrix`), and `typeID > 0` in
RendererContainer's instances (2), which the code generator evaluates.

*Objects of the run, 2026-10-31 (for build.kpp, section 12).* The interpreter runs the program's
object lifetimes, so that build.kpp can compute with Vector and String. Copies and moves run the
class's own copy and move constructors with C++'s value categories: `T b = a;` copies,
`static_cast<T&&>(b)`, `move(b)` and `forward<T>(x)` of an rvalue move (the source's place given to
the constructor, so a move empties it), a by-value parameter is copied or moved from its argument,
`return local;` moves (a constant evaluation elides no named object, [class.copy.elision]/1; the
program does not either), a prvalue initializes its object directly, and an implicit copy or move
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
constant.

*A constexpr variable needs its constant, 2026-10-31.* A `constexpr` (or `constinit`) variable,
global, static member or local, whose initializer the interpreter cannot evaluate is an error
("the constexpr variable x is no constant", with the reason when there is one), never a variable
initialized at run time (C++ [dcl.constexpr] 10; spec
language/constexpr_variable_that_is_no_constant_is_error, found by thread E for build.kpp). It is
checked where its type is known, as a static_assert is: a template's instances, not the template;
one whose value needs the code generator's exact layouts is the code generator's. The game had 21
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
body to run at compile time"). `kppc --run-build CONFIGURATION files...` prints it (run_tests'
runbuild case); `--run-build debug,release` prints each in turn, which is how kpp describes a
project (project.cpp, through the compile server; projectlib's `describe`), with no describe.exe.

*Its cost, measured 2026-10-31 (stage 7).* A description at Factorio's scale, written with Vector and
String as projectlib will be (15 C libraries of 20 sources, 5 include directories and 5 defines
each, two executables; 481 lines of description; `C:\kpp-sandbox\scratch\templates\rb7`), with
`C:\kpp-sandbox\scratch\templates\runbuild-time.ps1` (10 interleaved rounds, `kppc --std stdlib
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
Where Kpp stands (see [prototype-results.md](prototype-results.md)): the whole front end over
Factorio's own code runs in 0.32 s on 32 threads, and for a generated program with Factorio's
number of function bodies a debug build is 1.7 s from source to executable and 1.25 s after a
one-line change, release 3.8 s of LLVM; the converter will show what the real code costs.

## Kpp v0: the first runnable subset (proposed 2026-10-23, not yet reviewed by kovarex)

The smallest language that is still Kpp, as the target of the first code generator:

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
  templates/deduction_guide_target. A pack of base classes (`struct Overload : Ts...` with `using
  Ts::operator()...`, the overload-set idiom) is an error where such a class is laid out, until
  something needs it (2026-10-31, templates/pack_of_base_classes_is_not_supported_yet).
- **The literal 0 is a null pointer constant** (C++'s rule; for kovarex's review, who may prefer
  Kpp to require `nullptr`): an integer literal 0 (`0`, `0u`, `0L`, ..., and only a literal, not a
  constant that is 0) converts to any pointer and member pointer type, ranked as a standard
  conversion: `CoInitializeEx(0, COINIT_MULTITHREADED)`, `Entity* e = 0`, `pick(0)` choosing
  `pick(Entity*)` over `pick(const String&)`, while `take(0)` stays `take(int)`. Converted C and C++
  pass 0 for pointers throughout; requiring nullptr would be a converter rewrite instead (2026-10-29,
  spec language/literal_zero_null_pointer).
- **A pointer converts without a cast only as in C++** (2026-10-31): to the same type with added
  qualifiers (at every level as [conv.qual] allows: `T* const*` to `const T* const*`), to `void*`
  from an object pointer, from a derived class to its base; a function or array to a pointer to it.
  Any other (`float*` to `int*`, `const int*` to `int*`, one function pointer type to another) is
  an error, in an initialisation, assignment, argument or return alike; imported C also converts
  to and from `void*`. Two pointers compare as their composite pointer type (a `const Derived*`
  against a `Base*` as a `const Base*`, moved to the base part). Placement new takes any object
  pointer as its place, a const one too (the library's containers of const elements construct
  into their storage so, where C++'s construct_at casts). Specs:
  expressions/pointer_of_other_type_is_error, expressions/pointer_conversions_without_cast,
  expressions/function_pointer_of_other_type_is_error.
- **A C-variadic call is never format-checked** (2026-11-01, kovarex, section 14; it replaced
  "a printf-like call's format matches its arguments", decided 2026-10-31): Kpp code prints through
  `print`/`toString`, the converter rewrites printf-family calls into them, and what stays variadic
  is a call at the C boundary or of a converted library's own variadic interface (trio, zlib's
  gzprintf, stb's writef), passed as written. Specs: language/variadic_with_own_format,
  language/variadic_calls_are_unchecked, language/printf_format_matching_arguments.
- **Grammar parseable without name lookup.** Three rules replace C++'s dependence on knowing
  what a name is:
  1. A statement that starts with a (qualified) name followed by `*`, `&`, `&&` chains and then
     an identifier is a declaration (`Foo* p;`); as an expression it would be useless anyway.
  2. `Type name(args)` is a declaration; `Type(args)` is an expression (no most vexing parse).
     At namespace scope, `T name(a, m);` with only names in the parentheses is an object
     constructed from them when they are variables (enumerators, functions), and a function with
     unnamed parameters when they are types: the resolver decides once every name is indexed
     (2026-10-29, spec language/object_declaration_from_variables).
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
  pointers, references, arrays, `const`. The fixed-size names are what Kpp code writes; each is
  one of C++'s types under another name: `i8` is `signed char` (`int8_t`), `u8` `unsigned char`
  (`uint8_t`), `i16`/`u16` `short`, `i32`/`u32` `int` (`int32_t`), `i64`/`u64` `long long`
  (`int64_t`), `f32` `float`, `f64` `double`; `size_t`/`usize` is `u64` (the integer names since
  2026-10-24, `f32`/`f64` since 2026-11-01). So a converted `f(int)` beside `f(int32_t)` is one
  function, as in C++.
- **Distinct fixed types, deprecate long** (kovarex, 2026-11-01: "Distinct fixed types, deprecate
  long"; it decides Q1 of "Targets and hosts" below). The integer types are C++'s, with C++'s
  identities and overloads:
  - `char`, `signed char` and `unsigned char` are three types (`char` stays signed on every
    target); `__int8` is `char`, as MSVC has it. `char8_t`, `char16_t`, `char32_t` are types of
    their own. (`wchar_t` is still `char16_t`: Q2.)
  - `int`, `long` and `long long` are three types, `unsigned long` and its siblings likewise. So
    `f(int)` beside `f(long)` is two overloads, `X<int>` and `X<long>` two instances, `1L` a
    `long`, `int` to `long` an integral conversion (no promotion: a `short` promotes to `int`
    only), and the usual arithmetic conversions rank `long` between `int` and `long long` (on
    Windows `1u + 1L` is `unsigned long`, since `long` cannot hold every `unsigned int`).
  - `long` is C's type, for C interop only: imported headers, `extern "C"` declarations and blocks
    (a binding's aliases and C struct mirrors go there too), and the standard library's overloads
    that cover it (`toString(long)`, as `std::to_string` has). Its size follows the target's C ABI:
    32 bits on Windows, 64 on LP64 targets (`TypeTable::longBits`, set from the target).
    Elsewhere it is deprecated, as all of C's integer names are (section 13: `kppc
    --c-type-warnings`). Kpp code writes `i32`/`i64` and `f64` for `long double`.

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
- **Statements and expressions.** The C++ set, with the proposal's rules: required `this->`,
  explicit by default, no implicit fallthrough, exhaustive `switch`, `?->` and `?:`.
- **C interop.** `extern "C"` declarations of functions and structs; those are the only
  externally linked names (nothing is mangled across Kpp code, since everything is one program).
- **Not in v0:** exceptions, RTTI, virtual inheritance, `both_const`/`class_const`, typed
  unions (plain C unions exist, for interop), `accumulate`, coroutines.

**Backend plan.** The compiler writes textual LLVM IR for the whole program and runs the
bundled clang on it (`clang -O2 program.ll`). This needs no LLVM development libraries on the
machine (only the clang binaries exist here) and keeps every codegen decision in Kpp's own
code; moving to the LLVM C++ API in-process is a later optimisation. Names in IR are the
qualified Kpp names with parameter types, quoted, so there is no mangling scheme to design.

## Running and testing Kpp code (proposed 2026-10-30, thread G; the open choices are kovarex's)
The goal: write some Kpp and see it run, and write tests for Kpp code, with no ceremony. The test
syntax is already decided (8.6: `test "name" { }`, `check`, `require`); what follows is the rest
of the mechanism, what is done, and the choices left to kovarex (marked **Q**).

### How to use it (works on master since 2026-10-30)
Run a program: `kppc --run` builds it (only what changed since the last run) and runs it with
the arguments after the file; the exit code is the program's.
```
// hello.kpp
int main(int argc, char** argv)
{
  println("hello, ", argc - 1, " arguments");
  return 0;
}
```
`compiler\build\kppc.exe --run hello.kpp a b` prints `hello, 2 arguments`.

Test code: write `test` blocks next to the code (or in a file of their own) and run them with
`kppc --test`, with the files or directories to build. A normal build skips the tests, and the
file's `main` is left out of a test build.
```
// square.kpp
int square(int x) { return x * x; }

test "square"
{
  check(square(3) == 9);
  check(square(-2) == 5);    // fails: square.kpp:7:3: check failed: square(-2) == 5 (4 == 5)
  require(square(0) == 0);   // a failed require ends the test
}
```
`compiler\build\kppc.exe --test square.kpp` prints each failure as above, `failed: square.kpp >
square` after a test that failed, and `1 test, 1 failed`. The exit code is 0 when every test
passed and 1 otherwise. Arguments after `--` go to the runner: `kppc --test src -- --filter
square` (a part of "file > name"; `*` matches anything), `-- --list`, and `-- --report -` (a JSON
line per test, for tools). Compiler options go before `--run` or `--test`: `--release`,
`--msvc-diagnostics`, `--no-server` (build without the compile server, which both use by
default). When the build reports an error, nothing runs. A small program or test takes about
0.15-0.17 s from the edit to the result, under 0.02 s when nothing changed.

### What exists (surveyed 2026-10-30 on 7f3e55c)
- **Running a snippet** is two commands: `kppc --exe out.exe file.kpp`, then `out.exe`. The
  program is built with the whole standard library (59 files; the front end takes 0.04 s of it).
- **The spec suite** (`compiler/tests/spec.cmd`, `run_spec.py`): one program per construct,
  compiled, run, stdout compared with `.expected`; headers `// spec:`, `// pending:`, `// error:`,
  and now `// run:` / `// exit:` (built and run in one step through `kppc --run`). It checks
  output, not assertions: it is the compiler's specification, not a test framework for Kpp code.
- **`compiler/tests/run_tests.cmd`**: the same shape for `tests/run/*.kpp`, plus the access,
  determinism and incremental checks.
- **The converted game's unit tests** (`programs/factorio/tests.cmd`): UnitTest++ converted to Kpp
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

| From the edit to the output | `kppc --exe`, then the program | `kppc --run` | `kppc --server --run` |
|---|---|---|---|
| file edited | 0.33 s (0.29-0.38) + 0.07 s | 0.47 s (0.44-0.78) | 0.61 s (0.53-0.81) |
| nothing changed | 0.30 s (0.28-0.33) + 0.07 s | 0.31 s (0.25-0.63) | 0.07 s (0.03-0.16) |

Where an edited build's time goes (`kppc --exe` without `--quiet`): the front end 0.03-0.05 s over
the 59 files, codegen 0.02 s, clang -O0 of the one changed module 0.12-0.27 s, the link
0.17-0.39 s: clang and the link are 80-90% of it. Two findings for the owners of the server
(thread A) and the link (thread B): the compile server's rebuild after an edit is slower than a
new process's (0.42-0.60 s against 0.29-0.38 s for `--exe`; clang and the link both slower in the
server), and a freshly linked 10-line executable takes 0.06-0.09 s to run (likely the virus
scanner reading the new file). A small test costs the same build: until 8.6 exists a test is a
snippet with asserts, 0.3-0.5 s from the edit to the result.

### (a) `kppc --run` (done 2026-10-30)
`kppc [options] --run SOURCE [program arguments]` builds SOURCE (a file or a directory) with the
options before `--run`, runs it with the arguments after SOURCE, and returns the program's exit
code; when the build fails, the compiler's errors and exit code. The executable and its module
objects live in `%TEMP%\kpp-run\<name>-<hash of the source's path and the options>\`, so the next
run rebuilds only the changed module and nothing is written next to the source. The build is a
kppc process of its own (a build ends its process when done), a client of the compile server: it
starts the server when none runs, and the server stops after an hour idle (one per kppc binary,
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
**Q1**: should `kppc file.kpp args` with no output option mean `--run`, or a file starting with
`#!` run as a script? Proposed: no; one explicit spelling.

### (b) Native tests: the rest of 8.6
- **Declaration** as 8.6: `test "name" { ... }` at namespace scope or in a class. The name is a
  string; two tests of one name in one program are a compile error (not yet). A test's full name
  is its file's path as the build names it (later its package, "Code packages") and its name:
  `core/Vector.kpp > push_back grows`.
- **Checks.** `check(expr)` records a failure and continues; `require(expr)` records it and ends
  the test. The message is in the compiler's error format, so editors jump to it:
  `file:line:column: check failed: v.size() == 3 (2 == 3)`: the expression text and, for a
  comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`), both operands' values when `toString` or a
  `str()` method can say them (the text alone otherwise). The same values then go into `assert` messages (7.2's "operand
  values: later"). Inside a test, a failing `assert` or `release_assert` fails the test (the
  runner's `kpp_assert_failed` ends the test by throwing, section 9).
- **One command.** `kppc [options] --test ROOTS... [-- runner arguments]` builds the tests under
  ROOTS into one test executable (in `%TEMP%\kpp-run\`, as `--run` does) and runs it; `--tests`
  (8.6) stays the build-only switch, with `--exe`, for a binary to keep or debug. In a test build
  the program's `main` is left out and the runner is the entry point. The standard library's tests
  run only when a root names it. Without `--tests` or `--test`, the parser skips test declarations:
  they cost nothing.
- **The runner** (its arguments after `--`, or on the test executable): `--filter TEXT` (a
  substring of the full name, `*` matching anything; repeatable), `--list` (the tests, no run),
  `--report FILE` (below), `--jobs N` (below). One line per failure and `failed: <full name>`
  after a failed test, then a summary: `12 tests, 1 failed`. The exit code is 0 when all pass, 1 when one failed, the compiler's when the
  build failed. **Q2**: `--filter` as above instead of 8.6's `--run-test <name or file>`? (Built
  as `--filter` on 2026-10-30, the coordinator's call, reversible.)
- **Isolation and parallelism.** **Q3**, proposed: by default the tests run in one process, one
  after another (the fastest; a crash ends the run and is reported as that test's); `--jobs N` runs
  them in N processes of the same executable, each a share, and a crash fails only its test (the
  process is started again for the rest of its share), which is what `suites.py` does by hand for
  the converted UnitTest++. No threads inside one process: tests may use globals.
- **The converted UnitTest++.** The game's tests stay UnitTest++, converted as they are: they are
  the game's code, compared with the C++ build's run (9006 passing). **Q4**: should the converter
  (thread C) later turn `SUITE`/`TEST`/`CHECK`/`CHECK_EQUAL` into `test` and `check`, so the game's
  tests run under the native runner (a suite becomes a filter)? Proposed: not now; the native
  runner first serves new Kpp code and the standard library.
- **Spec snippets stay** output comparisons: they specify the compiler (the pass/pending table);
  tests check Kpp code. Each test construct (declaration, check values, require, the failure
  format, filtering) gets its spec snippet first, as every feature does.

### (c) Visual Studio (thread E) and the IDE (thread F)
Both drive the same command line and read the same machine-readable report; neither parses the
human output.
- **Discovery**: `kppc --test ROOTS -- --list --report -` writes one JSON object per line,
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
  Debugging" is `kppc --run`; debugging builds with `--exe` (or `--tests --exe`) and starts the
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
4. The runner is Kpp source the driver adds as one more file, `<tests>.kpp`
   (`compiler/src/tests.cpp`), as it adds `<config>`: its `main`, `kpp_check_failed`, a
   `kpp_assert_failed` that fails the running test, and the comparison templates. The standard
   library is untouched.
5. Not yet: `--jobs N` (the processes are kppc's, so the test executable stays simple), seconds
   in the report, a compile error for two tests of one name, leaving the standard library's
   tests out of a program's run (it has none yet).

Latency of a small test (one `test` with a loop and two checks, `kppc --test fib.kpp`, six rounds
on 2026-10-30, other threads building): 0.34 s median from the edit to the summary (0.32-0.69 s),
0.27 s with nothing changed (0.25-0.45 s). Again after `--test` began reading the build's output
for errors, the machine quieter: 0.22 s median after an edit (0.20-0.40 s), 0.19 s with nothing
changed (0.16-0.26 s). Cold (the cached objects deleted before each round), on a heavily loaded
machine: 2.3 s median (1.5-2.8 s); in the same rounds, warm after an edit 0.91 s (0.46-1.69 s) and
warm unchanged 0.87 s (0.31-1.36 s). The load decides more than the build does; a cold run builds
the standard library's modules once.

## Targets and hosts (decided 2026-10-31; the design proposed by thread B, Q1 to Q6 kovarex's)

**Decision (kovarex, 2026-10-31):** "kpp will have to work obviously in all reasonable platforms, so
windows/mac/linux is minimum." kpp.net, the website and library registry written in Kpp, is hosted
on NearlyFreeSpeech.NET, which runs FreeBSD. That decision has two axes:

- **Targets:** what kppc emits and links for. The minimum is Windows x64 (the default), Linux x86-64
  and arm64, macOS arm64 (Apple Silicon) and x86-64, and FreeBSD x86-64 for kpp.net.
- **Hosts:** where kppc itself (C++20) builds and runs. That is Windows, Linux and macOS.

On both axes Windows stays the default, with its output unchanged: the game, the tests, and the IR
byte for byte unless a difference is shown to be a fix.

**Order (proposed):**
1. Linux x86-64: the most tooling, CI runners, and the closest to FreeBSD. It is also testable on
   this machine (WSL).
2. FreeBSD x86-64.
3. macOS arm64, with Linux arm64 alongside: one AArch64 lowering serves both.
4. macOS x86-64.

| Target | Triple | Object, linker | C calling convention | Exceptions | Tested on |
|---|---|---|---|---|---|
| Windows x64 (default) | `x86_64-pc-windows-msvc` | COFF, lld-link | MS x64 | SEH (today's) | this machine, CI |
| Linux x86-64 | `x86_64-unknown-linux-gnu` | ELF, ld.lld | SysV AMD64 | Itanium tables, Kpp's personality | WSL here, CI ubuntu |
| FreeBSD x86-64 | `x86_64-unknown-freebsd14` | ELF, ld.lld | SysV AMD64 | the same | NFS, or a FreeBSD VM (Q3) |
| Linux arm64 | `aarch64-unknown-linux-gnu` | ELF, ld.lld | AAPCS64 | the same | CI ubuntu arm64, qemu-user in WSL |
| macOS arm64 | `arm64-apple-macosx11.0.0` | Mach-O, ld64 (or ld64.lld) | Apple's AAPCS64 | the same, compact unwind | CI macOS arm64 (Q5) |
| macOS x86-64 | `x86_64-apple-macosx10.15.0` | Mach-O, ld64 | SysV AMD64 | the same | CI macOS x86-64 (Q5) |

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
- **kppc as a host program** uses Win32 in its sources (lines matching Win32 APIs):
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
| Calls between Kpp functions | Kpp's convention: a class through a pointer to a copy, the result slot after `this` | the same | Unchanged everywhere. Scalars get the platform's convention from LLVM's `ccc` and the triple, with no codegen change. A class result keeps its slot after `this`. On AArch64, LLVM's `sret` attribute puts the slot in x8 for C functions only. |
| `extern "C"` calls with a struct by value | the pointer to a copy (wrong at 8 bytes or less) | SysV: classified into INTEGER/SSE registers up to 16 bytes, larger on the stack (`byval`). AAPCS64: up to 16 bytes in x0-x7, a homogeneous float aggregate of up to 4 members in v0-v3, larger through a pointer to a copy, a result in memory through x8. | A C ABI lowering per target in codegen (MS x64, SysV, AAPCS64), for calls to C and for Kpp functions C calls back. It fixes Windows' small-struct gap too. Specs for each size class on every target. |
| Varargs | LLVM lowers the calls; `va_list` is `char*` | SysV: `__va_list_tag[1]`. Linux AArch64: a 32-byte struct. Apple arm64: every variadic argument on the stack, `va_list` is `char*`. | LLVM lowers the calls for each triple. `va_list` comes from the target's headers through the C import. Kpp defines no C variadic functions. |
| `long` | 32 bits | 64 bits in C (LP64) | Q1 below (decided): C's size on each target. |
| `wchar_t` | 16 bits, UTF-16 | 32 bits | Q2 below. |
| plain `char` | signed | unsigned on Linux AArch64, signed on Apple arm64 and x86-64 | Kpp's `char` stays signed on every target (Kpp's meaning does not change with the target). An imported C `char` is the same type; a C function that relies on unsigned `char` sees the same bits. |
| `long double` | = double | x87 80-bit on x86-64 SysV, 128-bit on Linux AArch64, = double on Apple arm64 | Kpp's stays double. An imported C function that takes or returns `long double` is an error where the C type differs. |
| Bit fields | MSVC's rules | SysV's (Itanium): a unit straddles less, and types of different sizes may share one | The target's C rules for every class (a struct shared with C must match). Windows is unchanged. Specs on each target. |
| Memory ordering | x86's: stores are seen in order | AArch64 is weakly ordered | The library's atomics are LLVM atomics with explicit orderings, so they hold. Lock-free code that relies on x86's ordering through plain loads and stores is a bug on AArch64. The library is audited for it in the AArch64 stage. |
| Symbol names | quoted IR names (`@"twice(int32)"`), C names unmangled | the same names are valid in ELF and Mach-O; Mach-O prefixes `_`, which LLVM adds through the data layout's `m:o` | Unchanged in the IR. No `dllimport` off Windows. |
| COMDAT | `linkonce_odr` with `comdat any` | ELF has COMDATs. Mach-O has none (LLVM rejects them) and coalesces weak definitions instead. | Emitted on Windows and ELF, left out on Mach-O. The shared definitions stay `linkonce_odr` or `weak_odr`. |
| TLS | `thread_local` globals, lazily built per thread | ELF TLS; Mach-O's TLV descriptors (`__tlv_bootstrap`); both through LLVM | Unchanged. |
| Global constructors | `llvm.global_ctors` | `.init_array` on ELF, `__mod_init_func` on Mach-O, from the same IR | Unchanged. No target runs global destructors at exit yet. |
| Entry | `main`, or `wmain` (the game) | `main`, with UTF-8 `argv` | `wmain` is Windows-only. |
| Debug info | CodeView | DWARF (on macOS, `dsymutil` collects it into a .dSYM) | One module flag per target. |
| Signing | none needed | macOS arm64 runs only signed code | ld64 and ld64.lld sign ad hoc on arm64 by default. A program shipped to others needs a Developer ID signature and notarization (packaging, thread E). |
| The runtime's stderr and errno | `__acrt_iob_func(2)`, `_errno()` | `write(2, ...)`; `__errno_location()` on glibc, `__error()` on FreeBSD and macOS | The runtime's few calls go through the platform layer (3). |

- **Q1, `long`: decided** (kovarex, 2026-11-01, "Distinct fixed types, deprecate long"; the Kpp v0
  section's "Types" has the rule): `long` is a distinct type with C++'s overload behaviour, for C
  interop only, as wide as the target's C `long` (option (b) for its size), and deprecated in Kpp
  code, which writes the fixed-size names. The options as they were put:

  On LP64 targets (every target but Windows), C's `long` is 64 bits:
  - **(a) Kpp's `long` stays 32 bits everywhere.** C's `long` follows the target: the C import
    types an imported header's `long` as 64 bits on LP64, and hand-written bindings say `CLong` or
    `CULong` (stdlib/clib: int32 on Windows, int64 elsewhere). A Kpp program means the same on
    every target, and the library's 93 lines and the game's 232 stay right. The cost is that Kpp's
    `long` differs from C's off Windows, so code ported from POSIX C must use `CLong` for C's.
  - **(b) `long` follows the target's C.** This matches C and C++. Code that meant 32 bits (the
    library's atomics, MSVC code like the game's) changes meaning off Windows, so the converter
    would map MSVC's `long` to `int32_t`, and portable code must not say `long`.
  - **(c) No `long` in Kpp.** It is an error, leaving int, int64_t and the rest, plus `CLong` for C.
    The converter maps it, and so must the 93 library lines.

  **Recommended: (a).** Kpp's types mean one thing everywhere, and only the C boundary follows the
  platform, which is where the difference lives. It is needed before the first target's C import:
  `ssize_t`, `off_t` and `time_t` are `long` in glibc's, FreeBSD's and Apple's headers.
- **Q2, `wchar_t`: decided** (kovarex, 2026-11-01): (a), the target's C size; UTF-8 String is the portable text.
  - **(a)** `wchar_t` is the target's C `wchar_t` (16 or 32 bits), for interop only.
    The library's portable text is UTF-8 (String). The UTF-16 APIs (WString, conversions) exist on
    Windows only, and a path's native form is per platform, as in std::filesystem: UTF-16 on
    Windows, UTF-8 bytes elsewhere.
  - **(b)** `wchar_t` is 16 bits everywhere. C's `wchar_t` then differs off Windows, and Kpp code
    that passes one to a C function there is wrong.

  The game's 394 lines are Windows code.

### 2. Exceptions: one front end, two lowerings

The semantics of section 9 hold on every target:
- The search is two-phase: the handler is found before anything unwinds.
- A handler matches through the thrown type's descriptor and its bases, with the base offset
  applied.
- The objects between the throw and the handler are destroyed in order.
- A destructor that throws while unwinding terminates.
- An uncaught exception prints its type and message, then aborts.
- Descriptors, the record a handler owns, `throw;`, and the current-exception stack are the same.

- **Windows keeps SEH**, unchanged.
- **Every other target uses the Itanium model's zero-cost tables.** The unwind information is
  `.eh_frame` on ELF, and compact unwind with `__eh_frame` on Mach-O; LLVM writes both. A call
  that may throw is an `invoke` whose unwind edge goes to a `landingpad`. LLVM writes the LSDA from
  the landing pads' clauses: the call-site table, the action table, and a type table of pointers to
  Kpp's type descriptors.
  - **The personality** is Kpp's own, `__kpp_personality_v0`, a runtime function codegen emits as
    IR text (as it emits `kpp_throw` today). It works over the platform unwinder's `_Unwind_*`
    API: libgcc_s on Linux and in FreeBSD's base system, and libunwind in macOS's libSystem.
    - Phase 1 walks the LSDA. A catch clause matches when `kpp_eh_offset` (the base search SEH's
      filters use today) finds the clause's descriptor among the thrown type's bases.
    - Phase 2 installs the landing pad with the record and the clause's selector.
  - **`throw`** allocates `{_Unwind_Exception header; object; descriptor; destructor}` and calls
    `_Unwind_RaiseException`.
  - **Not `__gxx_personality_v0`:** that would need C++ `std::type_info` objects and a C++ runtime's
    `__cxa_*` (libcxxrt, libsupc++, libc++abi). Kpp's personality keeps Kpp's descriptors and links
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

- **The portable files keep the Kpp API.** Thread, Time, Filesystem, Error, the streams, and the
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
- **macOS targets.** Built on a Mac: Xcode's command-line tools' clang, ld64 and the SDK. Apple's
  SDK licence ties its use to Apple hardware, so cross-linking for macOS from Windows or Linux is
  not proposed, though ld64.lld exists. A Mac host, or CI's macOS runners, builds and tests the
  macOS targets.
- **Windows target from another host:** lld-link and the Windows SDK's import libraries and UCRT
  headers. That is not needed for the minimum: Windows builds on Windows.
- **Finding the toolchain:**
  - Windows: today's vswhere search.
  - Linux and FreeBSD: `clang` and `ld.lld` on PATH, or an LLVM release directory.
  - macOS: `xcrun --find clang`.
  - Everywhere: `KPP_CLANG`, and a `--sysroot` or build.kpp's `project.sysroot`.

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

### 5. The host: kppc on Linux and macOS

kppc is C++20, built with clang. Its Win32 uses come in four kinds:
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
- CI adds ubuntu and macOS jobs that build kppc and run the portable tests.

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
5. **The host layer:** kppc builds and runs on Linux (WSL here, CI ubuntu), with A, E and F.
6. **FreeBSD x86-64:** its sysroot, its headers' constants and structs, its errno, and a test
   bundle run on FreeBSD. kpp.net's server can be built from here on.
7. **AArch64:** the AAPCS64 and Apple C lowerings, Linux arm64 (CI or qemu), and the memory
   ordering audit.
8. **macOS arm64, then x86-64:** Mach-O (no COMDATs, ld64, ad-hoc signing, compact unwind),
   kppc hosted on macOS, and CI macOS jobs.

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
    stderr. Each is chosen by the file-level `requires config::platform == ...` Kpp already had,
    which may serve the platform layer instead of directories.
  - Library functions that call Win32 or the UCRT (`_errno` in `stod`, Filesystem, Thread) link
    only while unused: a program for Linux that calls one fails to link, by name.
- **A sysroot for Linux** comes from WSL's Ubuntu (`tools/sysroot/linux-from-wsl.sh`, glibc
  2.39). Its link takes 0.03 s. The WSL root itself is no sysroot: ld.lld cannot follow
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
  - Skipped: the `run:` and `test:` snippets (kppc runs those on this machine), and 4 marked
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

  The `declare` lines carry the same types. A Kpp *definition* of a C-callable function that takes
  or returns a struct where C passes it differently is an error until it is needed.
- **Spec:** interop/c_structs_by_value, against clang's C for every size class both ways,
  including registers running out. It passes on Windows and on Linux.
- **Before, on Windows**, every struct went as a pointer to a copy, which is MS x64's rule only
  above 8 bytes:
  - Windows IR over 908 spec snippets: only the new one differs.
  - The game: one call differs, `SetConsoleCursorPosition(HANDLE, COORD)`. COORD is 4 bytes, so
    the console cursor went to a pointer's bits; it is now passed in a register.

### Stage 2, the target's C headers (2026-11-01)

Done before Q1 and Q2 were decided; what changed with them is in the next section.
- **The C import** preprocesses with `--target` and `--sysroot` (both in the cache's key), and
  reads glibc's own `stdio.h`, `stdlib.h` and `string.h`. They give 20 parse errors (GNU
  extensions), ignored as Windows' are.
  - **`long`:** on an LP64 target each C `long` became `long long` in the imported text (`widenCLong`,
    gone with the next section: Kpp's `long` is C's now).
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
  - An imported header's `long` is Kpp's `long` again (no `widenCLong`).
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
- **Seen on the way:** `sizeof(L"ab")` is a pointer's 8 on every target, not C++'s array size: Kpp
  types a string literal as a pointer.
### Questions for kovarex

- **Q1:** `long` on LP64 targets: decided 2026-11-01, "Distinct fixed types, deprecate long" (above).
- **Q2:** `wchar_t`: decided 2026-11-01, the target's C size (above).
- **Q3:** FreeBSD: the version NFS runs (`freebsd-version` there), an OK to download its
  `base.txz` as the sysroot, and how tests run there: a script he runs, SSH access for this
  machine, a local VM, or a FreeBSD VM in CI.
- **Q4:** The FreeBSD major version to target. The default is the one NFS runs.
- **Q5:** (to decide when macOS comes, kovarex 2026-11-01) CI for macOS (arm64 and x86-64) and Linux arm64 on GitHub's runners. macOS minutes cost
  10 times Linux's on a private repository. The alternative is a Mac he has, as a self-hosted
  runner.
- **Q6:** The minimum macOS (11.0 for arm64 and 10.15 for x86-64 proposed) and glibc (2.31,
  Ubuntu 20.04, proposed) versions to support.

## Open questions
- **Syntax must be parseable without name lookup.** C++ needs to know whether a name is a type
  or a template to parse `a * b;` or `f < a > (b)`. In the index-then-resolve model the parser
  runs before any name is known, so Kpp should make these cases unambiguous by grammar
  (candidates: a different template-argument bracket or a required disambiguation, no
  most-vexing-parse, declaration statements recognisable from their first tokens). The prototype
  parser uses heuristics that work on 99.2% of Factorio files; the resolver can flag misparses
  later, but a real compiler should not have to guess.

- **Library names and program names share one global scope.** The first clash happened on
  2026-10-24: a test's own `struct Pair` against the library's `Pair`. Options: the library in a
  namespace (`kpp::`) that programs see through an implicit `using`; or program declarations
  shadowing library ones; or simply distinct names. Not decided; the test was renamed.
  The proposal in "Code packages" above answers it with distinct names checked by the compiler.

- **An own IDE** (proposed 2026-10-30, [ide.md](ide.md)): a query layer in the compile server and an
  LSP adapter first, then an editor written in Kpp with a DbgEng debugger and hot reload. Its eight
  decisions are listed at the end of ide.md. **Decided 2026-10-31 (kovarex): integrate with the
  Language Server Protocol**, so that IntelliSense works in most editors: `kppc --lsp` (ide.md, "The
  language server as built"). Whether to build an own editor on top stays open.

- **Running and testing Kpp code** (the section above, 2026-10-30): Q1 an implicit `--run`, Q2
  `--filter` instead of `--run-test`, Q3 the test runner's isolation and parallelism, Q4 the
  converted UnitTest++ as native tests.

- **Targets and hosts** (the section above, 2026-10-31): Q1 `long` on LP64 targets (decided 2026-11-01), Q2 `wchar_t`, Q3 a FreeBSD sysroot and a way to run tests there, Q4 the FreeBSD version, Q5 CI for macOS and Linux arm64, Q6 the minimum OS versions.

- **How a build is described** ([build.md](build.md)): debug information in release builds, the
  command's name (`kpp`), who builds the C libraries the game links (FASTBuild's April build is 54
  `libraries/` commits behind HEAD), and hand-written or generated `.vcxproj`.

## Roadmap
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
3. Compiler that runs real Kpp programs via LLVM (started 2026-10-23: the v0 subset compiles
   through textual IR and clang, see prototype-results.md "Stage 4"; constructors, destructors,
   copies, operators, templates and the standard library seed `stdlib/core` with `String` and
   `Vector<T>`, virtual functions, lambdas, `HashMap`, `Optional` and `toString`/`println`
   and variadic templates with a type-safe `format` followed the same day, then the C header
   importer: `extern "C" import` of the UCRT and `windows.h`, see "C header import" in
   prototype-results.md; the first real program, `programs/kpplex`, the Kpp lexer in Kpp, runs
   as fast as its C++ transliteration on the Factorio corpus, 2026-10-24). Real programs live
   under `programs/<name>/`.
4. Macro replacements (section 7), the small ones first because real programs and the converter
   need them: `assert`/`release_assert`, caller location, `conditional` functions, `config`
   constants with `if constexpr` at every scope, `_` locals, attributes; then the ones that need
   design work: reflection, `for constexpr`, typed unions, real enums, tests.
5. Backend at Factorio scale (first measurement 2026-10-24, see prototype-results.md "Backend at
   scale": for 73 k functions LLVM needs 5 s at -O0 and 109 s at -O2 on one thread, kppc 0.84 s
   for everything before it). Done the same day: one IR module per group of files, compiled by
   clang in parallel, -O0 for debug and -O2 for release: 0.6 s and 3.8 s of LLVM for the 73 k
   functions on 32 threads, a debug build of 615 k lines in 1.7 s; unchanged modules keep their
   object files, so a one-line change rebuilds in 1.25 s (0.93 s of it single-threaded IR
   writing). Next: write the IR in parallel too; cross-module inlining; in-process LLVM instead
   of clang.exe when the textual round trip shows up in the numbers.
6. Persistent compiler + IDE integration: the one-line-change rebuild (16 s in Factorio today).
7. Converter from Factorio-style C++ to Kpp (needs 4).
8. Precise const typing (scheduled 2026-10-29 for after the converted game's first run reaches
   the main menu): a const value is never assigned, as in C++ (spec language/const_value_assigned,
   pending; Kpp accepts and stores `r = 5` through a `const int&` today). A checker error at every
   built-in assignment to a const value flagged about 60 valid lines of the game and the library,
   which is how imprecise the walk's constness is: Array<const char, N>::fill (instantiated,
   where C++ instantiates only the members used), DrawCommandBatch's and SpriteDrawOrder's vertex
   writes (about 50 lines), AsteroidDrawCommandBatch, EntityUsageBitMask's BitMaskStorage, and the
   imported C headers' ReadAcquire/WriteRelease bodies (`const volatile`). Those are to be typed
   right first, then the error turned on.
