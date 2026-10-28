# Kpp design

Kpp is a C++-like language with a new compiler built around one idea: **no includes at the
deepest level of how the compiler works**, so that builds are orders of magnitude faster.
The original proposal lives in this repository: [readme.md](../readme.md) (language changes)
and [compilation/index.md](../compilation/index.md) (compilation model).

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
- **Order-independent.** Declaration order does not matter (except runtime static
  initialization order within a file). Circular dependencies that cannot be resolved
  (e.g. type A contains B by value and B contains A) are reported as errors.
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
descriptor is a compile-time value the loop binds, like a template argument). Not yet: `m.type`,
`enumerators(E)`, `alternatives(U)`, comparing types with `==` in `if constexpr`.

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
of the alternatives; the compiler adds the tag-aware operations. Not yet: `alternatives(U)`, a
generated `str()`, constructing a union from a value in one expression.

Proposed 2026-10-28, for review (the converter maps `std::variant` to named typed unions, and
the game constructs them from values and saves and loads them generically):
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
Spec snippets: kpp/typed_union_from_value, kpp/typed_union_alternatives (pending).

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

### 9. Exceptions and RTTI (decided 2026-10-24)
- Kpp has exceptions, with C++'s `throw`/`try`/`catch` semantics, implemented on LLVM's
  unwinding (the Windows personality); the non-throwing path costs nothing. Factorio's 3,198
  `throw` sites convert as they are.
- `dynamic_cast` and `typeid` exist for single-inheritance hierarchies: a type descriptor behind
  the vtable, with the chain of bases, which is what Factorio's 647 casts need. No
  cross-casts or virtual bases.
- Implemented 2026-10-24 (throw, try/catch, rethrow; `dynamic_cast` not yet) on Windows SEH
  rather than the MSVC C++ ABI: `throw` builds the object on the heap and raises an SEH exception
  (code 0xE0000001) carrying the object, its type descriptor and its destructor; every `try`
  gets a filter function that walks the descriptor's base chain against the handlers in order
  (so `catch (const Base&)` works, with the base-subobject offset applied); destructors between
  the throw and the handler run in cleanup funclets; a handler owns the object and destroys it on
  exit unless it rethrows. Calls that may throw become `invoke`s only where something has to
  happen on the way out, so code without destructible locals and without `try` is unchanged.
  Uncaught exceptions print `unhandled exception of type T: message` and abort. Rules that follow
  from this: destructors do not throw; `extern "C"` functions do not throw (calls to them stay
  plain calls); an exception cannot pass through a C callback frame that was not compiled for
  unwinding; `catch` by pointer matches the pointee type's spelling only (no base conversion);
  `new T(args)` leaks the memory when the constructor throws (to fix with the allocator work).

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
- **Grammar parseable without name lookup.** Three rules replace C++'s dependence on knowing
  what a name is:
  1. A statement that starts with a (qualified) name followed by `*`, `&`, `&&` chains and then
     an identifier is a declaration (`Foo* p;`); as an expression it would be useless anyway.
  2. `Type name(args)` is a declaration; `Type(args)` is an expression (no most vexing parse).
  3. `name<...>` is a template-id in type positions always, and in expression positions only
     when the closing `>` is followed by `(`, `::`, `{`, `.` or `->`. `a < b > c` without
     parentheses is a compile error.
  4. `f(*p);` and `f(&x);` are calls. A parenthesised declarator is a declaration only when a
     parameter list or an array bound follows: `R (*f)(int)`, `T (&a)[3]`. Pointer and reference
     variables are written `T* p;`, never `T (*p);` (decided 2026-10-23, when `windows.h` showed
     that the C++ reading turns every `QueryPerformanceFrequency(&f);` into a declaration).
  No `typename` or `template` disambiguators (as in the proposal).
- **Types.** `bool`, `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `char` (UTF-8 code unit),
  pointers, references, arrays, `const`. `int` is an alias of `i32`, `size_t`/`usize` of `u64`
  (the short names exist in the compiler since 2026-10-24; `f32`/`f64` not yet).
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
