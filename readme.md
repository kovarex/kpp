# kpp - C++, but better

kpp aims to be very close to C++, while fixing its biggest shortcomings. Existing C++ programmers
should feel at home, and existing C++ code should convert to kpp mostly automatically: the whole of
Factorio already has (see [Status](#status)).

**[Getting started](docs/getting-started.md)**: install kpp, build a first program, build a project.

## What kpp fixes

- **Compile times.** One persistent, multithreaded compiler sees the whole program at once: every
  file is parsed once, every template instantiated once, and an edit rebuilds only what it touched.
  Factorio converted to kpp, against its own C++ build on the same machine (debug, 32 threads):

  | | C++ (MSVC, FASTBuild) | kpp |
  |---|---:|---:|
  | Edit of one function | 22 to 29 s | about 1.5 s |
  | Edit of an inline function used across the game | 51 to 59 s | about 1.6 s |
  | Clean build of the game | 57 to 61 s | about 10 to 12 s |
  | Clean build, every library included | 57 s | 33 s |

  How these are measured: [docs/compile-speed.md](docs/compile-speed.md),
  [docs/codegen-speed.md](docs/codegen-speed.md), [docs/test-matrix.md](docs/test-matrix.md).
- **Disk use.** Factorio's C++ build directory holds more than 16 GB of object files. The kpp debug
  build of the same game is about 1.5 GB in all, executable and debug information included, and
  shrinks further as the compiler keeps more in memory instead of on disk.
- **Memory.** No translation unit re-parses the same headers, so the compiler holds each declaration
  once instead of once per file. (Measured comparison to come.)
- **No includes, no forward declarations.** Every top-level name is visible in every file, in any
  order; there are no headers, no `.hpp`/`.cpp` pairs, and code can move between files freely.
  `private` keeps a name inside its file, and libraries keep their names behind their own
  qualifier (`zlib::compress`) unless they choose otherwise.
- **The editor knows what the compiler knows.** The compiler itself is the language server
  (`kpp --lsp`), so completion, errors and navigation in VS Code, Visual Studio and other editors come
  from the same compilation, in the configuration you build.
- **One compiler everywhere.** The same compiler and standard library on every platform, no
  per-compiler quirks. kpp runs on Windows today and builds for Windows, Linux and FreeBSD from there;
  Linux and macOS as hosts, and macOS as a target, are planned. A release is self-contained: it compiles and links without Visual Studio or a Windows SDK.
- **No silent surprises.** An unsupported construct is a compile error, never a guess: a `struct` is a
  checked public value type, a `class` checks access, two declarations of one name are an error,
  and printing has no format strings to get wrong (`print("x = ", x)`).
- **Less boilerplate.** Enums with methods, typed unions, reflection, tests in the language and
  `appendTo` as the one way to turn anything into text: see [The language](#the-language).
- **A build described in kpp.** `build.kpp` is ordinary kpp code, run by the compiler: targets,
  configurations, library options and a hook for the special cases, with no second build language.

## The language

kpp is C++ with a few rules changed and a few things added. Everything below is marked by where it
stands today: **works** (in today's compiler, with a test in `compiler/tests/spec/`), **planned**
(decided, with the syntax shown, not built yet) or **open** (not decided). The decided rules are in
[docs/design.md](docs/design.md).

### Safer by default

- **Exhaustive `switch` on an enum** (works). A `switch` without `default` must name every value,
  or it does not compile; a value outside them (a corrupted byte, a bad cast) aborts at run time
  instead of falling out of the switch.
- **No implicit fallthrough** (planned). Every `case` ends with an implicit `break`; falling
  through to the next case is written `fallthrough;`. Today `switch` still has C++'s rules, and
  `fallthrough;` is accepted as C++'s `[[fallthrough]];`.
- **Explicit by default** (planned). A constructor converts implicitly only when it is marked
  `implicit`: `implicit Celsius(double degrees)`. The keyword works today; a constructor without it
  still converts implicitly, as in C++, until the rule is turned on. Conversion operators get the
  same rule.
- **Required `this->`** (planned). A member is always written `this->x`, so adding a local or a
  global never silently changes what an existing line means.
- **Required `override`** (planned). A function that overrides a virtual function must say
  `override`; today `override` is optional, as in C++.
- **`this` is a reference** (planned): `this[7]`, `!this`, `this.x`, since `this` can be neither
  changed nor null. Inside an enum's methods `this` already is the value (`this == North`, below).
- **No format strings** (works). `println("x = ", x)` prints its arguments in sequence; a type
  becomes text through its `appendTo`, and a pointer prints only through `address(p)`.

### Less boilerplate

- **Enums with methods** (works). The values go in an inner block; the enum also has `count`,
  `values` (for iteration), `str()` and `fromStr()`, generated, or written by hand to replace them.

  ```cpp
  enum class Direction : u8
  {
    { North, East, South, West }
    bool isVertical() const { return this == North || this == South; }
    Direction opposite() const
    {
      switch (this) // no default: every value is named, and a value outside them aborts
      {
        case North: return South;
        case East: return West;
        case South: return North;
        case West: return East;
      }
    }
  };

  int main()
  {
    for (Direction d : Direction::values)  // North true South, East false West, ...
      println(StringView(d.str()), " ", d.isVertical(), " ", StringView(d.opposite().str()));
    println(Direction::count, " ", Direction::fromStr("West").has_value());  // 4 true
    return 0;
  }
  ```
- **Typed unions** (works). `union enum` is an enum whose every value carries a value of its own
  type, without std::variant's templates: constructed from a value of one of its types, read with
  `p[Name]` (checked against the tag), switched on exhaustively, compared and copied as
  std::variant is.

  ```cpp
  union enum Property : u8
  {
    { {int, Integer}, {double, Double}, {String, Text} }
  };

  String describe(const Property& p)
  {
    switch (p)
    {
      case Property::Integer: return format("int {}", p[Property::Integer]);
      case Property::Double: return format("double {}", p[Property::Double]);
      case Property::Text: return format("text {}", p[Property::Text]);
    }
  }

  int main()
  {
    Property p = 5;                       // the alternative of an int: Integer
    println(describe(p));                 // int 5
    p[Property::Text] = String("hello");  // destroys the int, holds a String
    println(describe(p));                 // text hello
    return 0;
  }
  ```
- **Reflection** (works). `for constexpr` over `members(T)` (or a typed union's
  `alternatives(U)`) writes code once for every member, which replaces the macros and the
  hand-written lists that save, load or show a class:

  ```cpp
  struct Settings
  {
    int width = 640;
    bool fullscreen = false;
  };

  template<typename T>
  String describe(const T& object)
  {
    String result;
    for constexpr (auto m : members(T))
      result << format("{}={} ", StringView(m.name), object.[m]);
    return result;
  }

  int main()
  {
    println(describe(Settings()));  // width=640 fullscreen=false
    return 0;
  }
  ```
  The same `for constexpr` iterates a list of types (`types<A, B, C>`) or of constants. There is no
  preprocessor: `config` constants with `if constexpr`, `assert`, and these loops replace macros.
- **No `typename` or `template` disambiguators** (works): the compiler knows what a dependent name
  is.

  ```cpp
  template<typename Container>
  Container::value_type first(const Container& container) { return container[0]; }

  int main()
  {
    Vector<int> numbers;
    numbers.push_back(7);
    println(first(numbers));  // 7
    return 0;
  }
  ```
- **The safe navigation `?->` and the Elvis `?:`** (planned). `a?->b?->foo()` calls `foo` only
  when `a` and `a->b` are set, and as an expression gives the last member's default value (null for
  a pointer) when one is not; `a ?: b` is `a ? a : b` with `a` evaluated once. The parser already
  reads both; code generation does not yet.
- **One definition for const and non-const** (planned). `both_const` writes a getter once for both
  constnesses (`both_const B* getB() both_const`), and `class_const` a class once for both, so an
  iterator and a const iterator are one definition (`iterator` and `iterator::const`). The exact
  rules are not written down yet.
- **Named arguments** (planned): passing only the default arguments you want to change, by
  name. The syntax is not decided yet. Choosing among default arguments by the argument's type
  (`foo(B(3), C(4))` for `foo(A a = ..., B b = ..., C c = ...)`) is open.
- **Accumulated virtual functions** (open). A virtual function that runs every level of the
  hierarchy without calling `super::` (`save`, `load`) was proposed as `accumulate`; reflection
  (above) now writes save and load once for every class, and whether `accumulate` is still needed
  is open.

### Tests

- **Tests in the language** (works). A `test` sits next to the code it tests, inside a class too
  (where it sees private members); `check` reports the expression, where it is and its operands'
  values, and goes on; `require` ends the test. Without `--test` tests cost nothing.

  ```cpp
  int square(int x) { return x * x; }

  test "square"
  {
    check(square(3) == 9);
    check(square(-2) == 4);
  }

  int main()
  {
    println(square(4));
    return 0;
  }
  ```
  `kpp --test` builds and runs them (`--filter TEXT`, `--list`, `--report FILE`). Code coverage in
  the compiler is open.

### Existing code

- **C libraries** (works). `extern "C" import "header.h";` anywhere in the program imports a C
  header, its constants included. C++ libraries are used through a thin C wrapper or converted:
  kpp calls only C.

  ```cpp
  extern "C" import "math.h";

  int main()
  {
    println(sqrt(2.0));
    return 0;
  }
  ```
- **Converting C++** (works for the C++ that Factorio uses). The compiler's `--convert DIR`
  rewrites C++ sources as kpp: the includes go, macros are replaced by kpp's constructs or
  expanded, and the rest stays as it was. Factorio,
  its tests and its own libraries convert this way and run (see [Status](#status)); C libraries
  convert with `--c-sources`.
- **Editors** (works). The compiler is the language server (`kpp --lsp`), so VS Code, Visual
  Studio and any editor speaking the Language Server Protocol get completion, errors and navigation
  from the same compilation that builds the program ([docs/editors.md](docs/editors.md)).

## Planned

Decided or intended, and not built yet; none of it works today.

- **Language rules** (planned, above): required `this->`, required `override`, `this` as a
  reference, explicit by default, no implicit fallthrough, `?->` and `?:`, `both_const` and
  `class_const`, named arguments.
- **Linux and macOS as hosts, macOS as a target** (planned). kpp runs on Windows and builds for
  Windows, Linux and FreeBSD today ([design.md](docs/design.md), "Targets and hosts").
- **Factorio on the bundled toolchain** (planned). The game still builds with an installed Visual
  Studio (`--toolchain msvc`) because it links C++ libraries built the Visual Studio way; it moves
  to kpp's own toolchain as they are converted or rebuilt (design.md 17).
- **Streaming code generation** (planned). LLVM inside the compiler, each thread handing its
  functions straight to the back end, instead of writing IR text for clang to read again;
  estimated at a few seconds off a clean debug build of the game
  ([docs/codegen-speed.md](docs/codegen-speed.md)).
- **The library registry on kpp.net** (planned): `kpp add zlib`, versions locked by content
  hash, library options listed when you add one ([docs/library-registry.md](docs/library-registry.md)).
- **A localisation library** (planned), with its own tools, for text whose word order changes per
  language.
- **kpp's own IDE** (under consideration). Editors get kpp through the language server today;
  whether to build an editor of its own on top is open ([docs/ide.md](docs/ide.md)).
- **Access levels between `private` and public** (under consideration), for example a member
  visible to its own directory or subsystem, where C++ code uses `friend` today.

## Status

kpp is young, under heavy development, and changes every day; releases are numbered
`0.MINOR.PATCH`.

- The compiler (lexer, parser, resolver, type checker, templates, compile-time evaluation, code
  generation through LLVM) and its own standard library work on real programs.
- **Factorio 2.1.25 is converted to kpp** and runs: its 9,501 unit tests pass with the same results,
  checked step by step against the C++ build's (CRCs), and its 45 menu maps replay identically, in
  debug and release. Its C libraries (zlib, zstd, ogg, stb_image, ...) are being converted too.
- Every construct the compiler supports has a test in `compiler/tests/spec/`; the decided rules are in
  [docs/design.md](docs/design.md).

## License

kpp, its standard library and its official libraries are Apache-2.0 WITH LLVM-exception
([LICENSE](LICENSE)): use them freely, also commercially; programs you build with kpp carry no
obligation from it. Contributions are signed off ([CONTRIBUTING.md](CONTRIBUTING.md)).
