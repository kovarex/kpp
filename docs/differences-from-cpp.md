# kpp's differences from C++

For a reader who knows C++. kpp is C++ with a few rules changed and a few things added: what this
page does not list is meant to work as in C++. Each entry gives C++'s rule, kpp's rule, an example
where one helps, the reason and where it is decided or tested.

Each entry has a status:

- **built**: works today. Every built entry was tried on kppc at 7c4a7e0a (2026-11-06), or has a
  passing snippet in `compiler/tests/spec/`.
- **this stage**: planned for the stage that ends with kpp integrated into Factorio's code.
- **later**: decided, for the stage after that.
- **undecided**: proposed or under consideration, with no decision yet.

Rules marked *open* belong to a planned feature whose details are not decided yet. The readme's
[Planned](../readme.md#planned) has the stages.

Not here: bugs and missing C++ features, unless the refusal is deliberate; platform ABI choices that
match what a C++ compiler does on that target. Where the standard library falls short of its own
specification is [stdlib-conformance.md](stdlib-conformance.md). A spec named below is a file under
`compiler/tests/spec/`. A change that adds or removes a deliberate difference from C++ updates this
page in the same commit, with its status.

- [Programs and files](#programs-and-files)
- [Configurations instead of macros](#configurations-instead-of-macros)
- [Grammar](#grammar)
- [Types and literals](#types-and-literals)
- [Classes](#classes)
- [Enums, switch and typed unions](#enums-switch-and-typed-unions)
- [Expressions](#expressions)
- [Templates and compile time](#templates-and-compile-time)
- [Exceptions](#exceptions)
- [ABI and layout](#abi-and-layout)
- [Text and the standard library](#text-and-the-standard-library)
- [Tests](#tests)
- [Often expected, but as in C++](#often-expected-but-as-in-c)

## Programs and files

### No includes, and order does not matter (built)

- **C++:** a name must be declared before its use, through headers and forward declarations.
- **kpp:** the program is a set of files, and every top-level name is visible in every file, in any
  order. A forward declaration is allowed and never needed. A class declared and never defined is
  opaque: pointers to it work, an object of it is an error.
- **Why:** it is what lets one multithreaded compiler parse each file once.
- **See:** [design 2][d2]; spec `classes/pointer_to_declared_only_class_is_opaque`.

```cpp
int main() { Late late; return later(late); }
int later(Late& late) { return late.x; }
struct Late { int x = 0; };
```

### No preprocessor (built)

- **C++:** `#include`, `#define` and `#if` run before the language.
- **kpp:** there are no macros. Constants, `config::` values, `if constexpr`, `when`, `assert` and
  `for constexpr` do their work (next section). The converter turns a C++ program's macros into these.
- **Why:** 22,647 macro expansions in Factorio fell into eleven cases, none needing text substitution.
- **Rule:** a `#` line in kpp source, `%:` too, is a compile error, each one reported. Spec
  `language/preprocessor_directive_is_error`.
- **See:** [design 7][d7]; [macro-survey.md](macro-survey.md).

### One global scope: a name clash is an error (built)

- **C++:** two files can each define a `const` or `static` of one name, which have internal linkage;
  two definitions with external linkage are an ODR violation, often undiagnosed.
- **kpp:** all top-level names of the program, the standard library and C imports share one scope.
  Two declarations of one name in different files are a compile error, `const` and `static` ones
  too (a constant written alike in two files is one constant). Overloads are no clash.
- **Why:** a name means one thing in every file, and a clash is found at build time.
- **See:** [One global scope][dglobal]; spec `language/function_defined_twice_is_error`.

```cpp
// a.kpp
const int limit = 1;
// b.kpp
const int limit = 2;   // error: top-level names are global
```

### `private` makes a name local to its file (built)

- **C++:** file-level `static` and anonymous namespaces.
- **kpp:** `private` before a top-level declaration. Neither `static` nor an anonymous namespace makes
  a name local: two files' `static int helper()` are "defined twice". `private` works in the global
  namespace only; inside a namespace it is an error.
- **Why:** one word for every kind of declaration, and C's many reused static names convert directly.
- **See:** [design 15][d15]; specs `kpp/private_names_of_two_files_do_not_clash`,
  `kpp/private_name_from_another_file_is_error`.

```cpp
private int helper() { return 1; }   // another file may have its own helper
```

### A template's calls see every declaration (built)

- **C++:** a call in a template that depends on no parameter binds where the template is defined, to
  the declarations before it.
- **kpp:** it sees every declaration of the name, as all of kpp's lookup does.
- **Why:** order matters nowhere else in kpp. Converted code that relied on the earlier overload
  writes it explicitly (a cast or a qualified name).
- **See:** [Open questions][dopen] (decided by kovarex, 2026-11-06).

```cpp
int helper(double) { return 1; }
template<class T> int call(T) { return helper(1); }
int helper(int) { return 2; }
// call(0) is 2; in C++ it is 1
```

### Libraries are qualified by their name (built)

- **C++:** a library chooses a namespace, or none, and users can write `using namespace`.
- **kpp:** a library's names are `NAME::x` by default, whatever its sources say, and it may declare
  itself unscoped, as the standard library is. `using namespace zlib;` and `namespace z = zlib;` of a
  library are errors. Namespaces written in code work as in C++.
- **Why:** adding a library never changes or breaks existing code, and `zlib::close` says what it is.
- **See:** [design 16][d16]; spec `kpp/using_namespace_of_a_library_is_error`.

### C is the only foreign language (built)

- **C++:** C++ libraries are linked, and C headers are `#include`d.
- **kpp:** `extern "C" import "header.h";` anywhere imports a C header for the whole program, its
  object-like constant macros as `const auto` constants. Function-like macros are not imported, and a
  header's inline functions are declared, not compiled. A C++ library needs a thin C wrapper or a port.
  Only `extern "C"` names link externally.
- **Why:** the C ABI is the one stable boundary, and no C++ ABI has to be followed (see
  [No C++ ABI](#no-c-abi-built)).
- **See:** [design 5][d5]; specs `interop/c_import_stdlib`, `interop/c_macro_casts`.

```cpp
extern "C" import "math.h";
int main() { println(sqrt(2.0)); return 0; }
```

## Configurations instead of macros

### `config` constants and `requires` at a file's top (built)

- **C++:** `-DNAME` and `#ifdef`.
- **kpp:** typed `config::` constants, set by `build.kpp` or `--config name=value`. A program or
  library declares its own at top level with a default: `config i32 level = 6;`. A file may start
  with `requires config::platform == Platform::Windows;`.
- **Why:** a typed value the compiler checks, listed by `kpp options`.
- **See:** [design 7][d7], [design 12][d12]; specs `kpp/config_values`, `kpp/config_default_declared`.

### `if constexpr` at every scope, its false branch unchecked (built)

- **C++:** only inside functions, and outside a template a discarded statement is still checked.
- **kpp:** also at namespace and class scope. In any function the branch a constant condition discards
  is not checked, so it may name what only the other configuration declares. It must still parse.
- **Why:** one source holds debug and release code, as the converted game's `#ifdef DEBUG` does.
- **See:** [design 7][d7]; specs `language/discarded_branch_in_plain_function`,
  `kpp/config_debug_branch_names_debug_only_members`.

```cpp
struct Line
{
  int length = 0;
  if constexpr (config::debug) { int checks = 0; }
};
```

### `when`: a declaration that exists under a condition (built)

- **C++:** `#ifdef` around a declaration; `if constexpr` opens a scope of its own.
- **kpp:** `when (condition) declaration;` in a block, a class or a namespace. When the condition holds
  it is an ordinary declaration of that scope, with that scope's lifetime; when it does not, it has
  no storage and its initializer is unchecked, and the name still hides outer names, so a use is an
  error in that build. `when` is a keyword.
- **Why:** a debug-only local or an RAII timer must live in the enclosing scope.
- **See:** [design 19][d19]; specs `kpp/when_object_lives_to_the_end_of_its_scope`,
  `kpp/when_false_name_hides_outer_names`, `kpp/when_member_exists_per_configuration`.

```cpp
when (config::timeMeasuring) TimeMeasurer timer("buildPhase");
```

### `assert`, `conditional` and `CallerLocation` (built)

- **C++:** macros: `assert`, logging macros that vanish, `__FILE__` and `__LINE__`, or
  `std::source_location`.
- **kpp:** `assert(e);` and `release_assert(e);` are statements. A `conditional(constant)` function's
  calls compile to nothing when it is false, arguments unevaluated. `CallerLocation here = caller` as a
  parameter is filled at the call site.
- **Why:** these were the macros no function could replace.
- **See:** [design 7][d7]; spec `kpp/asserts_and_config`.

```cpp
conditional(config::debug) void trace(Text text);
trace("cost ", expensive());   // nothing is evaluated in a release build
```

### `_` is a name you can repeat (built)

- **C++:** every local needs its own name (C++26 adds this).
- **kpp:** `_` may be declared any number of times in a scope, each an unnamed local that lives to the
  end of the scope.
- **Why:** it replaces macros that made a unique name for a scope guard.
- **See:** [design 7][d7]; spec `kpp/underscore_locals`.

## Grammar

kppc parses every file before it knows any name, so where C++ asks whether a name is a type, kpp
decides by syntax alone, and the resolver rejects what does not fit. These rules hold in kpp's own
sources; the converter's C++ input keeps C++'s reading.

### `a * b;` is a declaration (built)

- **C++:** a product or a declaration, depending on what `a` is.
- **kpp:** always a declaration of `b`; with `a` a variable it is an error. So are `a & b;`, `a && b;`
  and `a ^ b;`.
- **Why:** parsing needs no name lookup, and a dropped product is useless anyway.
- **See:** [design 22][d22]; specs `language/statement_type_star_name_is_declaration`,
  `language/statement_variable_star_name_is_error`.

### `T(x);` is an expression (built)

- **C++:** declares a variable `x` of type `T`.
- **kpp:** a temporary `T` made from `x`. Parentheses in a declarator only group, so `int (x);` is
  the expression `int(x)`.
- **Why:** the same, and it is what the line looks like.
- **See:** [design 22][d22]; spec `language/functional_cast_statement_is_expression`.

### The most vexing parse goes by scope (built)

- **C++:** `T x();` declares a function, anywhere.
- **kpp:** in a body `T x(...)` is always a variable, `T x();` too (value-initialized); a body declares
  no function. At namespace and class scope `T f(...)` is always a function, so a variable there is
  written with `=` or `{}`: `int x(3);` at namespace scope is an error.
- **Why:** one rule per scope instead of a guess from the parentheses.
- **See:** [design 22][d22]; specs `language/most_vexing_parse_by_scope`,
  `language/namespace_scope_parentheses_declare_a_function`.

```cpp
Widget global{1, 2};   // namespace scope: braces or =
int main()
{
  Widget local();      // a variable
  return 0;
}
```

### No chained comparisons (built)

- **C++:** `a < b < c` is `(a < b) < c`.
- **kpp:** a relational operator with a relational operand, or an equality operator with an equality
  operand, is an error without parentheses. `a < b == c < d` stays as in C++.
- **Why:** it almost never means what it says, and `a < b > c` looks like a template-id.
- **See:** [design 22][d22]; spec `language/chained_comparison_is_error`.

### `a < b >> c` needs parentheses (built)

- **C++:** the comparison of `a` with `b >> c`, unless `a` is a template.
- **kpp:** an error naming both readings: `x < (y >> 2)` or `x<y> > 2`. `>>` still closes nested
  template arguments, as in `Vector<Vector<int>>`.
- **Why:** without knowing what `a` is, either reading is a guess. This one silently broke a mipmap loop.
- **See:** [design 22][d22]; spec `language/less_than_then_shift_needs_parentheses`.

### Template arguments by the token after `>` (built)

- **C++:** `name<` opens template arguments when `name` is a template.
- **kpp:** in an expression, as in C#, by the token after the matching `>`: `f(a<b, c>(d))` calls the
  template `a`, and `f(a < b, c > d)` passes two comparisons. An argument that can only be a type
  decides it whatever follows.
- **Why:** no name lookup, and the two errors above remove the cases where this could misread silently.
- **See:** [design 22][d22]; spec `language/template_arguments_by_the_token_after`.

### `typename` and `template` are not needed (built)

- **C++:** required before a dependent type or member template.
- **kpp:** the compiler knows what a dependent name is. Writing them is accepted.
- **Why:** they are noise that exists for C++'s parser.
- **See:** [design 3][d3]; the [readme](../readme.md#less-boilerplate).

```cpp
template<typename Container>
Container::value_type first(const Container& container) { return container[0]; }
```

## Types and literals

### The number types are `u8` to `u64`, `i8` to `i64`, `f32`, `f64` (built)

- **C++:** `int`, `long` and friends, with `uint32_t` from a header.
- **kpp:** built-in names, each one of C++'s types under another name (`i32` is `int`, `i64` is
  `long long`), so overloads and layouts are C++'s. `usize` is `u64`. `int`, `long`, `size_t` and the
  like keep C++'s meanings for C interop and are deprecated elsewhere: `--c-type-warnings` warns at
  each, off by default so far.
- **Why:** kovarex: "I always didn't like that they were so ambigous."
- **See:** [design 13][d13]; specs `language/kpp_number_names_are_types_to_the_parser`,
  `language/c_type_names_outside_extern_c_warn`, `language/integer_types_are_distinct`.

### A character literal holds at most four characters (built)

- **C++:** `'abcde'` is conditionally supported; clang and g++ warn and keep the last four.
- **kpp:** two to four characters are an `int` as there (`'ABCD'`, the first the highest byte). More is
  an error.
- **Why:** an int cannot hold them; dropping characters would be a silent wrong answer.
- **See:** [design 11][d11]; spec `language/multicharacter_literals`.

### The literal 0 is no pointer (built)

- **C++:** `0` and `NULL` are null pointer constants.
- **kpp:** only `nullptr`. An integer 0 becoming a pointer or member pointer is an error at the 0.
  Among overloads C++ finds alike, one that would take the 0 as a pointer is no candidate, so
  `take(0)` with `take(u64)` and `take(const char*)` is `take(u64)`, where C++ finds it ambiguous.
  Imported C keeps C's rule.
- **Why:** kovarex: "writing 0 instead of nullptr is not nice".
- **See:** [kpp v0][dv0]; specs `language/zero_literal_as_pointer_is_error`,
  `language/literal_zero_never_chooses_a_pointer_overload`.

### A string literal does no pointer arithmetic with `+` (built)

- **C++:** `"abc" + 1` is a pointer into the literal.
- **kpp:** an error, and so is a literal plus a pointer. `"x = " + value` is text when the other side
  is a String or StringView; `&"abc"[n]` is the pointer.
- **Why:** a reader expects text, and the program gets the wrong address silently.
- **See:** [design 14][d14]; spec `language/string_literal_plus_integer_is_error`.

### `T^` is `UniquePtr<T>` (built)

- **C++:** `std::unique_ptr<T>`.
- **kpp:** `T^` is sugar for the library's class, one type with it. `^` is a declarator beside `*`, `&`
  and `&&` where what follows cannot start an operand; `x ^ 3` stays exclusive or. Errors, hover and
  the debugger print it back as `T^`.
- **Why:** `Vector<Item^>` reads better, and a library class keeps every library pointer first-class.
- **See:** [design 20][d20]; specs `language/unique_pointer_caret`,
  `language/unique_pointer_caret_printed_in_errors`.

### `long double` is `double`, and `char` is always signed (built)

- **C++:** both follow the target: 80-bit `long double` on x86-64 Linux, unsigned `char` on Linux arm64.
- **kpp:** `long double` is `double` on every target; an imported C function that uses a different
  `long double` is an error where it is used. `char` stays signed when arm64 comes.
- **Why:** kpp's meaning does not change with the target.
- **See:** [Targets and hosts][dtargets]; spec `interop/c_long_double_function_off_windows_is_error`.

### Undecided number types (undecided)

- **`float` and `double`:** deprecated like `int`, or plain synonyms of `f32` and `f64`. They work today.
- **`char`:** proposed to stay as the UTF-8 code unit, with no warning; the alternative is a `c8`.
- **`u128` and `i128`:** proposed, not built. `f16` is proposed for later.
- **`--c-type-warnings`:** on by default once the converter and the library have moved; whether it ever
  becomes an error is kovarex's call.
- **See:** [design 13][d13].

## Classes

### `struct` is a public value type (built)

- **C++:** `struct` and `class` differ only in default access.
- **kpp:** a struct's members and bases are all public, it has no virtual functions, and it cannot
  derive from a polymorphic class: each is an error, "make it a class". Constructors, a destructor,
  methods, operators and friends are fine. `class` is as in C++, access checked.
- **Why:** the keyword now tells the reader something, and 96% of the game's structs already fit.
- **See:** [struct and class][dstruct]; specs `classes/struct_private_member_is_error`,
  `classes/struct_virtual_function_is_error`, `classes/struct_base_not_public_is_error`,
  `classes/struct_deriving_from_polymorphic_class_is_error`.

### No virtual bases (built)

- **C++:** `class B : public virtual A`.
- **kpp:** an error. A base reached along two paths is two subobjects, as for a non-virtual base in C++.
- **Why:** deliberately left out (kovarex confirmed it on 2026-11-06).
- **See:** [design 9][d9]; spec `classes/virtual_base_is_not_supported`.

### `[*this]` is refused (built)

- **C++:** a lambda captures a copy of the object.
- **kpp:** an error. A copy of the object is an explicit init-capture, `[copy = this]`, used as `copy.x`;
  `[&this]` captures it by reference, and `[this]` is the error "write `&this`, not `this`" (kovarex,
  2026-11-07: a silent copy would be a big gotcha). `[=]` never captures the object. Nothing in the game or
  the libraries copies it today.
- **See:** [design 6][d6]; spec `language/lambda_capture_of_this_by_copy_is_error`.

### Returning a local is guaranteed to build it in place (built)

- **C++:** the compiler may elide the copy (NRVO), and the class must stay copyable or movable.
- **kpp:** when every `return` returns the same local of the result's class, it is built in the caller's
  result: no copy or move, in debug and release alike, and the class needs neither. Otherwise a return
  moves, as C++'s. `--return-copy-warnings` names the returns that copy or move.
- **Why:** what a return costs must not depend on the optimizer.
- **See:** [design 23][d23]; specs `language/named_return_elided`,
  `language/named_return_of_non_movable_type`.

```cpp
struct Pinned { Pinned() {} Pinned(const Pinned&) = delete; Pinned(Pinned&&) = delete; };
Pinned make() { Pinned p; return p; }   // compiles: an error in C++
```

### Placement new (built)

- **C++:** the place is a `void*`, so a const pointer needs a cast. Replacing a const object is undefined.
  Since C++20 old names reach the new object, with exceptions, and `std::launder` covers the rest.
- **kpp:** a place that visibly names an object declared const is a compile error. Over an object with
  const or reference members the replacement is transparent, without exceptions, and there is no
  `launder`. Any object pointer is a place, a const one too.
- **Why:** a const object may be folded or read-only, and the generator never assumes members unchanged.
- **See:** [design 21][d21]; specs `language/placement_new_over_const_object_is_error`,
  `language/placement_new_replaces_transparently`.

### Required `override` (built)

- **C++:** `override` is optional.
- **kpp:** every function that overrides a virtual one says so, `override` or `final`, but a destructor
  (below), checked in each instance of a class template whose base is its parameter.
- **Status:** an error since 2026-11-07 ([design 23b](design.md)).
- **Why:** a change in a base cannot silently unhook an override.
- **See:** [design 6][d6]; the readme's Planned.

### A polymorphic class's destructor is virtual (this stage, decided, not built)

- **C++:** a destructor is virtual only when declared so, its own or a base's; `delete` through a base
  whose destructor is not virtual is undefined behaviour.
- **kpp:** a class with a virtual function, its own or a base's, always has a virtual destructor, written
  or not (kovarex, 2026-11-07). `virtual ~X()` is allowed and redundant.
- **Status:** `--override-warnings` lists the classes it changes ([design 23b](design.md)).
- **`[[foreign_vtable]]`:** a class whose vtable an outside ABI fixes (a COM interface) says so, inherited:
  it gets no destructor in its vtable, and `delete` through a pointer to it is an error.
- **Why:** deleting through a base always runs the right destructor.

### Required `this.` and `This` (this stage, rules decided)

- **C++:** a member is named bare; a class names itself by its name and arguments.
- **kpp:** an instance's member is written `this.x`, so adding a local or a global never changes an
  existing line (kovarex, 2026-11-07: `this` is a reference everywhere, so the readme's "required
  `this->`" is `this.`). `This` is a type keyword for the class around the code, the current instance of a
  class template (`This` for `Vector<T>`), and inside an enum's methods the enum (`This::North`); a
  static is `This::count` or `Train::count`, never `this.count`; nested types and enumerators stay bare.
  `This` is built, and reserved in kpp code (not in imported C, where it stays a name): C++ that names
  something `This` must rename it; the game and our kpp have no such name. A bare name is a local, a parameter or a global, never a
  member: `void setX(i32 x) { this.x = x; }`. The member a member initializer names stays bare
  (`: speed(speed)`), as do `using` declarations and declared types. All kpp code follows it, the
  stdlib and libraries too; imported headers are exempt. Today members are named bare, as in C++.
- **Status:** `--this-warnings` lists them ([design 23c](design.md)).
- **See:** [design 6][d6]; the readme's Planned.

### `this` is a reference (this stage, decided, not built)

- **C++:** `this` is a pointer.
- **kpp:** `this` is a reference everywhere (kovarex, 2026-11-07): `this.x` is the member access and
  `this->x` the class's `operator->`; `return this;` where C++ writes `return *this;`, and `*this` is the
  class's `operator*`, an error when it has none; `&this` is the pointer (`foo(&this)`, `delete &this`);
  `this == other` compares the objects; `[&this]` captures it by reference and `[copy = this]` copies it,
  while `[this]` and `[*this]` are errors, and `[=]` never captures it; `this` is never null, so
  `if (this)` is an error. Today it is a pointer, except in an enum's methods, where it is the value.
- **Status:** `--this-reference-warnings` lists each use that changes; the game has 185,189 lines of
  `this->`, 5,097 of `*this` and 8,707 using its address ([design 23d](design.md)).
- **Why:** one meaning of `this` in every place, and `*this` gone from the code.
- **See:** [design 6][d6]; the readme's Planned.

### Explicit by default (this stage)

- **C++:** a constructor or conversion operator converts implicitly unless marked `explicit`.
- **kpp:** it converts implicitly only when marked `implicit`, and `explicit` is gone: plain `explicit`
  is an error, and `explicit(cond)` is written `implicit(cond)`. A function argument's braces name their
  type: `draw(Color{1, 0, 0})`, not `draw({1, 0, 0})`, unless the parameter is an InitializerList
  (`max({a, b, c})`); nested braces never need one. In `return {x, y};` and `Color c = {1, 0, 0};` a list
  of two or more values takes any constructor, and `{a}` needs `implicit`. A default
  constructor is never a conversion; `if (x)` takes an unmarked `operator bool`, a `switch` a marked
  conversion; `Box b = 5;` deduces from marked constructors only.
- **Status:** `implicit` and `implicit(cond)` are built. The rest is built behind `--explicit-by-default`, with
  every rule's error; it becomes the default once the game, the stdlib, the libraries and the specs build with
  it, and until then an unmarked constructor converts.
- **Why:** an implicit conversion should be something the author asked for, and a keyword that does
  nothing goes (kovarex, 2026-11-07).
- **See:** [design 6][d6] and its "Explicit by default" entry; specs `kpp/implicit_constructors`,
  `kpp/implicit_condition`, `kpp/explicit_by_default` and its three `_is_error` ones.

```cpp
struct Celsius { implicit Celsius(double degrees); };
```

### One definition for const and non-const (later, details not decided)

- **C++:** a getter is written twice, and C++23's deducing `this` helps a little.
- **kpp:** `both_const B* getB() both_const` for a getter, `class_const` for a class such as an iterator.
- **See:** [design 6][d6]; the readme's Planned.

### Named arguments (later, syntax not chosen)

- **C++:** arguments go by position, so a default in the middle cannot be skipped.
- **kpp:** passing only the defaults you change, by name.
- **See:** [design 6][d6]; the readme's Planned.

### Under consideration (undecided)

Access levels between `private` and public (a member visible to its directory, where C++ uses
`friend`), choosing a default argument by its type (`foo(B(3), C(4))`) and `accumulate`, a virtual
function that runs every level of a hierarchy. See the readme's Planned.

## Enums, switch and typed unions

### Enums with methods (built)

- **C++:** an enum has values only.
- **kpp:** the values go in an inner block, then methods, in which `this` is the value and the
  enumerators are in scope. It has `count`, `values`, `str()` and `fromStr()`, generated or hand-written,
  and the underlying type is `i32` unless written. A plain `enum class` without the inner block keeps
  C++'s rules.
- **Why:** it replaces the hand-written tables and macros beside each enum.
- **Names:** outside the enum an enumerator is `Direction::North`, in a case label too; no bare label and
  no `.North` shorthand (kovarex, 2026-11-07). Inside the enum's own methods `North`, `Direction::North`
  and `This::North` all name it. A bare one outside is an error naming the fix (spec
  `language/scoped_enumerator_needs_its_enum`).
- **No plain enums** (decided, not built): kpp code has only `enum class` (kovarex, 2026-11-07); imported
  headers keep theirs. `--plain-enum-warnings` lists them; the game has 208 ([design 8.4](design.md)).
- **See:** [design 8.4][d8]; spec `kpp/enum_from_str`; the [readme](../readme.md#less-boilerplate).

```cpp
enum class Direction : u8
{
  { North, East, South, West }
  bool isVertical() const { return this == North || this == South; }
};
```

### `switch` on such an enum is exhaustive (built)

- **C++:** a missing value is at most a warning, and an invalid one falls out of the switch.
- **kpp:** on an enum with the inner block, or a typed union, a `switch` without `default` must name
  every value, and a value outside them aborts at run time. Plain enums keep C++'s rule.
- **Why:** adding a value finds every switch, and a corrupted byte stops the program where it is seen.
- **See:** [design 8.4][d8]; spec `kpp/typed_unions_switch_copy`.

### No implicit fallthrough (this stage)

- **C++:** a `case` runs into the next without `break`.
- **kpp:** every group of statements after a label ends with an implicit `break`; labels side by side
  share one group, and falling through is `fallthrough;` as a group's last statement. A `break;` that
  ends a group, `[[fallthrough]];` and a label nested in another statement are errors, and each group
  is a scope of its own.
- **Status:** step 1 is built: a case that can reach the next label without `break;`, `fallthrough;`,
  `return`, `continue`, `goto` or `throw` is an error, and `switch` keeps C++'s meaning. The flip is built
  behind `--no-implicit-fallthrough`, with every rule's error. It becomes the default once the game, the
  stdlib, the libraries and the specs build with it.
- **Why:** falling through by accident is a bug class; a keyword that does nothing goes (kovarex,
  2026-11-07).
- **See:** [design 6][d6] and its "No implicit fallthrough" entry; specs
  `language/switch_case_running_on_is_error`, `language/switch_case_ends_written`,
  `kpp/no_implicit_fallthrough` and its `_errors_is_error`.

### Typed unions (built)

- **C++:** `std::variant`, a template read through `std::get` and `std::visit`.
- **kpp:** `union enum` is an enum whose values each carry a type: built from a value, read with
  `p[Name]` (a release_assert on another tag), switched on exhaustively. Several tags may share a type.
  Comparison, assignment and the valueless state are `std::variant`'s; its `emplace` is the union's
  destructor and placement new from a tag.
- **Why:** the game's `InputAction` and `EditorAction` have this shape, without the templates.
- **See:** [design 8.5][d8]; specs `kpp/typed_unions_switch_copy`, `kpp/typed_union_comparisons`.

```cpp
union enum Property : u8
{
  { {int, Integer}, {double, Double}, {String, Text} }
};
Property p = 5;
p[Property::Text] = String("hello");
```

## Expressions

### `?->` and `?:` (built)

- **C++:** neither (`?:` is a GNU extension).
- **kpp:** `a?->b?->foo()` calls `foo` only when `a` and `a->b` are set: the rest of the postfix chain is
  skipped as one piece, its arguments not evaluated. When it is skipped, a pointer result (raw, or a class
  with `operator->` and a bool conversion: UniquePtr, `T^`, SharedPtr) is null and a bool result false;
  any other result makes the expression `Optional<T>` (kovarex, 2026-11-07): `i32 s = t?->engine->power;`
  is an error, `i32 s = t?->engine->power ?: -1;` works, as `?:` on an Optional gives its value or the
  right side. `a ?: b` is `a ? a : b` with `a` evaluated once.
- **See:** design 23a.

### Three-way comparison has one result type (built)

- **C++:** `<=>` gives `strong_ordering`, `weak_ordering` or `partial_ordering`.
- **kpp:** `strong_ordering` is the one type, and the other two names are the same type. An unordered
  result, a NaN's, compares false with 0 every way, as in C++.
- **See:** [design 4][d4]; spec `expressions/three_way_comparison_with_nan_is_unordered`.

## Templates and compile time

### Any function can run at compile time (built)

- **C++:** only `constexpr` and `consteval` functions.
- **kpp:** any function with a body, `constexpr` optional. Signed overflow at compile time is an
  error, as in C++. Floating point at compile time is the host's IEEE `+`, `-`, `*`, `/` and `sqrt`,
  and nothing else of the C math library.
- **Why:** one interpreter runs ordinary code, so templates need not be a computation language.
- **See:** [Compile-time evaluation][dconst]; spec `language/consteval_signed_overflow`.

```cpp
int twice(int x) { return x * 2; }
static_assert(twice(4) == 8);
```

### SFINAE works, kpp code writes `requires` (built)

- **C++:** a candidate whose substitution fails is dropped, and `enable_if` and `void_t` lean on it.
- **kpp:** the same, kept for C++ compatibility: imported C++ libraries rely on it. kpp's own code
  constrains with `requires`; `EnableIf` and `VoidT` stay in the library for imported or unconverted
  code.
- **Why:** imported C++ libraries rely on SFINAE (kovarex, 2026-11-07, F's question); `requires`
  states the condition that SFINAE hides in a type.
- **See:** [design 3][d3].

### Instantiation depth is 64 (built)

- **C++:** implementations allow about 1024 nested instantiations, counted less strictly.
- **kpp:** more than 64 instantiations in a chain is an error naming the last two. Open to change by
  kovarex; the whole game needs 12.
- **Why:** templates as computation are dropped, and a runaway template gets slower with each step.
- **See:** [design 3][d3].

### Compile-time loops and reflection (built)

- **C++:** recursion over packs, macros, or C++26's reflection.
- **kpp:** `for constexpr` over `types<A, B>`, a list of constants, `members(T)` or `alternatives(U)`,
  with `m.name`, `m.type`, `m.offset` and `object.[m]`; `nameof(x)`. Inside functions today, not yet at
  namespace or class scope.
- **Why:** save, load and display are written once per class, without X-macros.
- **See:** [design 8][d8]; specs `kpp/for_constexpr_types`, `kpp/reflection_members`, `kpp/nameof`.

```cpp
for constexpr (auto m : members(T))
  result << format("{}={} ", StringView(m.name), object.[m]);
```

### Structured bindings: two simplifications (built)

- **C++:** the hidden object is passed to `get` as an xvalue when it is no reference, and a binding's
  type comes from `std::tuple_element`.
- **kpp:** it is passed as an lvalue, and a binding's type is what `get` gives without its reference.
  The two agree whenever `get` returns the element by reference.
- **See:** [design 4][d4]; spec `language/structured_binding_tuple_protocol`.

## Exceptions

### An `extern "C"` function does not throw (built)

- **C++:** the language allows it, and what happens depends on the compiler and its flags.
- **kpp:** calls to one stay plain calls, and an exception leaving one ends the program.
- **Why:** nothing on the way out is compiled for unwinding.
- **See:** [design 9][d9].

### An uncaught exception reports itself (built)

- **C++:** `terminate` calls the handler alone, and the default handler says what it likes.
- **kpp:** kpp prints the type and message on stderr, then calls the handler `setTerminate` installed.
  The search ends the program before anything unwinds, with the throw site on the stack.
- **Why:** a crash report needs them.
- **See:** [stdlib-conformance.md](stdlib-conformance.md); spec
  `language/uncaught_exception_calls_the_terminate_handler`.

## ABI and layout

### No C++ ABI (built)

- **C++:** the platform's ABI: MSVC's or Itanium's names, vtables and exceptions.
- **kpp:** no name mangling across kpp code, since everything is one program. The vtable follows
  MSVC's and COM's order, but a virtual destructor's slot is a plain destructor, overloads keep
  declaration order, and a class argument goes as a pointer to a copy. Exceptions are kpp's own.
  `extern "C"` calls follow the C ABI exactly.
- **Why:** kpp shares objects with C alone. A COM interface can still be declared as a kpp class.
- **See:** [design 9][d9]; spec `interop/c_object_with_vtable`.

### Every empty base takes no storage, on Windows too (built)

- **C++:** MSVC gives only the first empty base none, unless the class says `__declspec(empty_bases)`.
- **kpp:** Itanium's rule on every target.
- **Why:** kpp shares classes with C alone, which has no bases.
- **See:** [design 9][d9]; spec `classes/empty_base_optimization`.

```cpp
struct E1 {}; struct E2 {};
struct T : E1, E2 { char c; };   // 1 byte; 2 with MSVC's layout
```

### A base's padding holds nothing of a derived class (built)

- **C++:** MSVC lays a derived class's members into the padding an `alignas` base adds, and Itanium into
  a non-POD base's tail padding.
- **kpp:** the derived class's own parts start after the base's whole size, on every target.
- **Why:** copying a base subobject by its size never touches the derived class's members.
- **See:** [design 9][d9].

```cpp
struct alignas(16) B16 { char x; };
struct S1 : B16 { int y; };   // 32 bytes; 16 with MSVC's layout
```

### `[[no_unique_address]]` works on every target (built)

- **C++:** MSVC ignores it. Itanium also places a repeated empty member inside the class.
- **kpp:** an empty member takes no storage, as an empty base. A member whose class another part of the
  class holds keeps its byte, so some classes are larger than Itanium's. Anywhere but on a non-static
  data member that is no bit field it is an error.
- **See:** [design 9][d9]; spec `classes/no_unique_address_empty_member_takes_no_storage`.

### A pointer to a member of an incomplete class (built)

- **C++:** MSVC gives it the 24-byte unknown inheritance model.
- **kpp:** every class is known, so it has its class's own size: 8 or 16 bytes.
- **See:** [design 9][d9].

### Floating point is strict IEEE (built)

- **C++:** a compiler may contract `a * b + c` into one operation.
- **kpp:** no fast-math flags and no contraction in any build: every operation rounds as written.
- **Why:** the game's determinism depends on it.
- **See:** [design 2][d2].

## Text and the standard library

### No `std`: a library of kpp's own (built)

- **C++:** the standard library, in `std`, with allocator and traits parameters.
- **kpp:** `String`, `Vector`, `Map`, `HashMap`, `Optional` and the rest are in the global scope, with no
  allocator, traits or char-type template parameters. Each keeps the guarantees of the std type it
  replaces (invalidation, complexity, return values), and a faster type that breaks one has its own
  name (`FastMap`). `String` is UTF-8.
- **Why:** converted code relies on the guarantees, and the names are claimed once for every program.
- **See:** [design 4][d4]; [stdlib/](stdlib/).

### Printing takes arguments in sequence (built)

- **C++:** `printf` format strings, streams with `<<` and state, or `std::format`.
- **kpp:** `println("x = ", x)`, nothing parsed. A function takes text as a `Text` parameter, valid
  during the call only, and storing one is an error. `format` exists only with a literal format,
  checked against its arguments at compile time. The library has no `printf` or `cout`: C's `printf` is
  there only when imported. A bool prints as `true`, an enum as its name, and a pointer only through
  `address(p)`.
- **Why:** a text that is not a literal can contain `%` or `{}`.
- **See:** [design 14][d14]; specs `language/text_parameter_takes_arguments`,
  `language/text_parameter_placement_is_error`.

### A type becomes text through `appendTo` (built)

- **C++:** `operator<<` for streams, `std::formatter` or `to_string`.
- **kpp:** `void appendTo(String& out) const`, one member, which `print`, `toString`, `<<`, `+` and
  `Text` all use. Printing a type without it is a compile error, and a free `appendTo` is an error.
- **Why:** one method writes a deep hierarchy in one buffer, and templates need no special case.
- **See:** [design 14][d14]; spec `language/append_to_on_built_in_types`.

### `<<` is String's only append (built)

- **C++:** `s += 65` appends a character.
- **kpp:** `String` has no `+=`. `s << 65` appends the text "65", and a character comes only from a
  `char`. A `<<` chain into a String mixed with `&`, `|` or `^` without parentheses is an error.
- **Why:** `s += 65` can be read two ways.
- **See:** [design 14][d14]; spec `language/shift_chain_with_bitwise_operator_is_error`.

### A float prints its shortest exact text (built)

- **C++:** `to_string(0.1)` is `0.100000`.
- **kpp:** `toString(0.1)` is `0.1`, the shortest text that reads back to the same value, and `0.1 + 0.2`
  is `0.30000000000000004`. `fixed(x, 6)` chooses decimals.
- **Why:** kovarex: "by default, it should always print all the information".
- **See:** [design 14][d14]; spec `library/to_string_shortest_round_trip`.

### Text is UTF-8 at the edges too (built)

- **C++:** on Windows `argv` and `getenv` are in the ANSI code page.
- **kpp:** `main`'s arguments and `getenv` are UTF-8 on every target.
- **See:** [Targets and hosts][dtargets]; specs `interop/main_arguments_are_utf8`, `interop/getenv_is_utf8`.

### File streams have no text mode (built)

- **C++:** on Windows a text stream writes `\n` as CRLF.
- **kpp:** every file stream is binary, and `getline` drops a `\r` before the newline on every platform.
- **Why:** kovarex: CRLF is to be eliminated.
- **See:** [Open questions][dopen]; spec `library/file_streams_have_no_text_mode`.

### Library behaviour that differs by decision (built)

- **Containers allocate with malloc:** a program replacing `::operator new` sees its new-expressions only.
- **A path is wide text on every target**, converted to UTF-8 at each system call: a name that is not
  UTF-8 is not given back as the same bytes.
- **HashMap and HashSet iterators survive growth** for dereferencing, which `unordered_map`'s need not
  (`library/hash_map_iterator_after_growth`).
- **A container holds at most `maxContainerSize`**, for every element type, where `max_size()` depends
  on it.
- **Not in the library:** `<regex>`, and `<chrono>`'s calendars and time zones (`CivilTime` is the
  calendar). A duration is `Milliseconds(10)`, with no literals.
- **See:** [stdlib-conformance.md](stdlib-conformance.md).

## Tests

### Tests are in the language (built)

- **C++:** a framework and its macros.
- **kpp:** `test name { ... }`, `suite`, fixtures and `generate` are declarations, compiled only in a
  test build. `check` goes on after a failure, `require` ends the test, and both print the operands.
  `check` and `require` stay ordinary names outside test code.
- **Why:** these were the most used macros in the game.
- **See:** [design 8.6 and 8.7][d8]; specs `kpp/tests_in_compiler`, `kpp/test_fixture`.

```cpp
test square
{
  check(square(3) == 9);
}
```

### Comparisons inside a check (built)

- **C++:** `-1 == 4294967295u` is true, and two `char*` compare addresses.
- **kpp:** a comparison that is a check's whole expression compares a signed and an unsigned integer by
  value, as `std::cmp_equal`, and `==` or `!=` between two char pointers there is a compile error.
- **Why:** both keep what the game's `CHECK_EQUAL` meant.
- **See:** [design 8.7][d8]; specs `kpp/test_check_mixed_sign`, `kpp/test_check_char_pointers_is_error`.

## Often expected, but as in C++

These look like candidates for this page and are not differences: evaluation order (C++17's),
argument-dependent lookup and hidden friends, class template argument deduction, `noexcept` as part
of a function's type, concepts and `requires`, multiple inheritance, `dynamic_cast` and `typeid`,
destruction of globals at exit, and `friend`.

[d2]: design.md#2-compilation-model
[d3]: design.md#3-simple-templates
[d4]: design.md#4-our-own-standard-library
[d5]: design.md#5-interop-c-only
[d6]: design.md#6-language-changes-from-the-proposal-not-yet-scheduled
[d7]: design.md#7-no-preprocessor-what-replaces-macros-decided-2026-10-23
[d8]: design.md#8-compile-time-iteration-reflection-enums-typed-unions-tests-decided-2026-10-24
[d9]: design.md#9-exceptions-and-rtti-decided-2026-10-24
[d11]: design.md#11-evaluation-order-c17s-rules-decided-2026-10-30
[d12]: design.md#12-the-build-description-buildkpp-decided-2026-10-31
[d13]: design.md#13-integer-types-and-sizes-decided-2026-11-01
[d14]: design.md#14-text-output-arguments-in-sequence-text-appendto-and--decided-2026-11-01
[d15]: design.md#15-file-local-names-private-at-file-scope-decided-2026-11-01
[d16]: design.md#16-library-scopes-qualified-by-default-a-library-may-be-unscoped-decided-2026-11-01
[d19]: design.md#19-conditional-declarations-when-condition-declaration-decided-2026-11-03
[d20]: design.md#20-t-the-librarys-uniqueptrt-written-short-decided-2026-11-04
[d21]: design.md#21-placement-new-over-const-objects-decided-2026-11-04
[d22]: design.md#22-a-grammar-without-name-lookup-decided-2026-11-04
[d23]: design.md#23-named-return-elision-is-guaranteed-decided-2026-11-04
[dglobal]: design.md#one-global-scope-so-clashes-are-errors
[dstruct]: design.md#struct-and-class-decided-2026-11-01-option-3-with-friend
[dconst]: design.md#compile-time-evaluation-one-interpreter-decided-2026-10-29-steps-1-to-3-done
[dv0]: design.md#kpp-v0-the-first-runnable-subset-proposed-2026-10-23-not-yet-reviewed-by-kovarex
[dtargets]: design.md#targets-and-hosts-decided-2026-10-31-the-design-proposed-by-thread-b-q1-to-q6-kovarexs
[dopen]: design.md#open-questions
