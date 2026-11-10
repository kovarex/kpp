# kpp - C++, but better

kpp aims to be very close to C++, while fixing its biggest shortcomings. Existing C++ programmers
should feel at home, and existing C++ code should convert to kpp mostly automatically: the whole of
Factorio already has (see [Status](#status)).

**[Getting started](docs/getting-started.md)**: install kpp, build a first program, build a project.

## What kpp fixes

- **Compile times.** One persistent, multithreaded compiler sees the whole program at once: every
  file is parsed once, every template instantiated once, and an edit rebuilds only what it touched.
  Factorio converted to kpp, against its own C++ build (debug, 32 threads):

  | | C++ (MSVC, FASTBuild) | kpp 0.2.1154 | kpp faster by |
  |---|---:|---:|---:|
  | Edit of one function | 25 s | 2.2 s | 11x |
  | Edit of an inline function used across the game | 55 s | 2.5 s | 22x |
  | Clean build of the game, libraries already built | 42 s | 11.2 s | 3.8x |
  | Whole build of factorio.exe, every library included | 46 s | 13.5 s | 3.4x |
  | Whole build of factorio-test.exe (the unit tests), every library included | 60 s | 17.2 s | 3.5x |

  The kpp column is kpp 0.2.1154 (master 40a5b85e, 2026-11-09), a release build in the release's layout, run as a user
  runs it: `kpp measure` (the average of 3 rounds, the compile server warm for the edits, the machine otherwise idle) on the
  development machine (Ryzen 9 9950X, 32
  threads, 128 GB), where the C++ edit times were also measured (the average of 2 rounds, docs/test-matrix.md).
  The C++ clean and whole builds are kovarex's own measurement; the clean build is of a precompiled-header
  change, practically only Factorio recompiling. The last two rows are whole builds
  including all the C and C++ libraries the game links. Both sides build full debug information. These are today's numbers, and the work to
  bring them down is in the documents below. How they are measured: [docs/compile-speed.md](docs/compile-speed.md),
  [docs/codegen-speed.md](docs/codegen-speed.md), [docs/test-matrix.md](docs/test-matrix.md). To measure them
  on your own machine, run `kpp measure` in a factorio-in-kpp checkout
  ([docs/getting-started.md](docs/getting-started.md#measuring-the-readmes-compile-times-yourself)).
- **Run speed.** The converted game's kpp final build runs Factorio's 45 menu-screen maps in about
  0.85x the time of the C++ FinalRelease build players get (0.84x to 0.87x across runs), and two real saves
  in 0.96x and 0.97x: the same configuration and Factorio commit on both sides, identical checksums, and no
  map slower than C++ (kpp f8787782, Factorio 4c178a5b915, 2026-11-07). How it is measured:
  [docs/final-release.md](docs/final-release.md).
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
  per-compiler quirks. kpp runs on Windows and Linux and builds for Windows, Linux and FreeBSD. Factorio's
  Windows exe built on a Linux host passes all 9,501 of the game's tests and gives the C++ build's checksums.
  A release is self-contained: it compiles and links without Visual Studio or a Windows SDK.
- **No silent surprises.** An unsupported construct is a compile error, never a guess: a `struct` is a
  checked public value type, a `class` checks access, two declarations of one name are an error.
- **A build described in kpp.** `build.kpp` is ordinary kpp code, run by the compiler: targets,
  configurations, library options and a hook for the special cases, with no second build language.

## The language

kpp is C++ with a few rules changed and a few things added. Everything in this section works today;
what is decided but not built yet is under [Planned](#planned). Every rule that differs from C++ is in
[docs/differences-from-cpp.md](docs/differences-from-cpp.md).

### Safer by default

- **Exhaustive `switch` on an enum.** A `switch` without `default` must name every value, or it does
  not compile; a value outside them (a corrupted byte, a bad cast) aborts at run time instead of
  falling out of the switch.
- **No format strings.** `println("x = ", x)` prints its arguments in sequence; a type becomes text
  through its `appendTo`, and a pointer prints only through `address(p)`.
- **Safe navigation `?->` and the Elvis `?:`.** `a?->b?->foo()` calls `foo` only when `a` and
  `a->b` are set. When one is not, a pointer result is null and a bool false, and any other result is an
  `Optional`, which `?:` unwraps: `i32 power = train?->engine->power ?: -1;`. `a ?: b` is `a ? a : b`
  with `a` evaluated once.
- **Explicit by default.** A constructor or conversion operator converts implicitly only when marked
  `implicit` (`implicit Celsius(double degrees)`), and the keyword `explicit` is gone. A function
  argument's braces name their type: `draw(Color{1, 0, 0})`, not `draw({1, 0, 0})`.
- **No implicit fallthrough.** Every `case` ends with an implicit `break`; falling through is written
  `fallthrough;`, and a body several values share is one label, `case North, South:`.
- **A class with a virtual function has a virtual destructor**, written or not, so `delete` through a
  base runs the whole object's destructor. A COM interface, whose vtable is not kpp's to change, says
  `[[foreign_vtable]]` and cannot be deleted.
- **Required `this.`**: a member is written `this.x` and a static one `This::count`, so that adding a
  local or a global never changes what an existing line means; a bare name is never a member.
  `This` names the class the code is in (`Vector<T>` inside Vector). Outside a scoped enum its values
  are `Direction::North`, in case labels too.
- **Required `override`** on every function that overrides a virtual one (or says `final`).
- **`this` is a reference** everywhere: `this.x`, `return this;`, `&this` for the pointer. (Inside an
  enum's methods `this` is the value.)

### Less boilerplate

- **Enums with methods.** The values go in an inner block; the enum also has `count`,
  `values` (for iteration), `str()` and `fromStr()`, generated, or written by hand to replace them.

  ```cpp
  enum class Direction : u8
  {
    { North, East, South, West }
    bool isVertical() const { return this == North || this == South; }
    Direction opposite() const
    {
      switch (this) // no default
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
- **Typed unions.** `union enum` is an enum whose every value carries a value of its own
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
- **Reflection.** `for constexpr` over `members(T)` (or a typed union's
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
- **No `typename` or `template` disambiguators**: the compiler knows what a dependent name
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

### Tests

- **Tests in the language.** A `test` sits next to the code it tests, inside a class too
  (where it sees private members); `check` reports the expression, where it is and its operands'
  values, and goes on; `require` ends the test. Without `--test` tests cost nothing.

  ```cpp
  int square(int x) { return x * x; }

  test square
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
  `kpp --test` builds and runs them (`--filter TEXT`, `--list`, `--report FILE`).

### Existing code

- **C libraries.** `extern "C" import "header.h";` anywhere in the program imports a C
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
- **Converting C++.** The compiler's `--convert DIR` rewrites C++ sources as kpp: the includes go,
  macros are replaced by kpp's constructs or expanded, and the rest stays as it was; C sources
  convert with `--c-sources`. It handles the C++ Factorio is written in (see [Status](#status)).

## Planned

The work comes in two stages. This one makes Factorio a proper example with the full features: all
the language features and good compile times, so that kpp can be integrated into Factorio's code to
showcase its strong points. The next starts once kpp is a branch in Factorio, with PropertyTree,
kpp.net and other things.

### This stage

Decided, and not built yet:

- **Factorio on the bundled toolchain.** The game still builds with an installed Visual Studio
  (`--toolchain msvc`) because it links C++ libraries built the Visual Studio way; it moves to
  kpp's own toolchain as they are converted or rebuilt (design.md 17).

### Later

- **macOS as a target**, built on Windows and Linux ([design.md](docs/design.md), "Targets and
  hosts"), once kpp is a branch in Factorio.
- **Compiling outside Visual Studio**: builds from a terminal or another editor, with the compile
  server, are made smooth in the next stage. This one is about the Visual Studio workflow.
- **One definition for const and non-const**: `both_const` writes a getter once for both
  constnesses (`both_const B* getB() both_const`), `class_const` a class once for both (an iterator
  and its const iterator). The details are not decided yet.
- **Named arguments**, passing only the defaults you change, by name; the syntax is not chosen yet.
- **A localisation library**, with its own tools, for text whose word order changes per language.
- **An official library for LED and RGB devices** that works on Linux too (for example through
  OpenRGB's SDK), so a game lights keyboards and mice off Windows: Factorio's Logitech, Razer and
  SteelSeries lighting is Windows-only today. After the next stage.

### Under consideration, not decided

- **kpp's own IDE** on top of the language server ([docs/ide.md](docs/ide.md)).
- **Access levels between `private` and public**, for example a member visible to its own
  directory or subsystem, where C++ code uses `friend` today.
- **Choosing a default argument by its type**: `foo(B(3), C(4))` for `foo(A a = ..., B b = ...,
  C c = ...)`.
- **`accumulate`**: a virtual function that runs every level of the hierarchy without calling
  `super::`. Reflection already writes save and load once for every class.
- **Code coverage** in the compiler.
- **macOS as a host.** Building every target, macOS included, on Windows and Linux is enough for
  this stage.
- **The library registry on kpp.net**: `kpp add zlib`, versions locked by content hash, library
  options listed when you add one ([docs/library-registry.md](docs/library-registry.md)). Not for
  this stage.

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
obligation from it. Contributions come in under the same license ([CONTRIBUTING.md](CONTRIBUTING.md)).
