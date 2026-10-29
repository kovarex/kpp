# From here to Factorio built by kppc

Written 2026-10-24, answering "what is needed to compile the game with our compiler". The counts
are from the Factorio corpus (`src`, `tests`, `libraries/CommonUtil`, `libraries/Agui`, 1.33 M
lines) with plain `grep` over use sites, so they say how much code touches each thing, not how
hard each site is.

## Where kppc is

- **Front end.** Parses 99.54% of Factorio's files and types 77% of the expressions and 78% of
  the member accesses in 0.32 s on 32 threads. What it cannot type is the standard library
  (54 k `std::` references), macro-generated code and a tail of resolver gaps (templated
  aliases, dependent members through templated bases).
- **Code generation.** The Kpp v0 subset runs end to end: classes with constructors, destructors,
  copies and operators, single inheritance with virtual functions, templates with full
  specialisation and parameter packs, lambdas, `if constexpr`, the own library (`String`,
  `Vector`, `HashMap`, `Optional`, `format`), C headers imported straight from `windows.h` and
  the UCRT. 15 end-to-end tests; the lexer written in Kpp runs as fast as its C++ version.
- **Backend.** One IR module per group of files, clang in parallel, unchanged modules cached: a
  program with Factorio's number of function bodies builds in 1.7 s (debug), 1.25 s after a
  one-line change, 3.8 s of LLVM for release.
- **Language decisions made.** No preprocessor; `assert` statements, caller locations,
  `conditional` functions, typed `config` constants, `_` locals, attributes, real enums with
  `nameof` are in; `for constexpr`, reflection, typed unions and compiler tests are specified in
  design.md section 8 and wait for review.

## What the game needs that kppc does not have

| area | in Factorio | what it takes | size |
|---|---|---|---|
| **Standard library parity** | `std::string` 13,877 sites, `vector` 6,243, `unique_ptr` 4,068, algorithms (`sort`, `find`, `min`...) 4,619, `optional` 1,675, `function` 1,576, `map` 925, `pair` 873, `set` 745, `array` 500, `chrono`/`filesystem` 445, `deque` 131, `shared_ptr` 103, `unordered_map` 48, `variant` 32, streams 29; threads and locks 629, `atomic` 153 | `String`, `Vector`, `HashMap`, `Optional` exist. Missing: `UniquePtr`, `SharedPtr`, `Function`, an ordered `Map`/`Set`, `Pair`/`Tuple`, `Array`, `Deque`, `Span`, the algorithms, `Thread`/`Mutex`/`Atomic` over Win32, clocks, files and paths, `ssprintf`-style formatting | XL: the long pole, and every one of those 35 k sites goes through the converter |
| **Exceptions** | 3,198 `throw`, 746 `try`, 542 `catch`; used for loading, scripting and network errors | A decision first: implement them (LLVM landing pads with the Windows personality; unwinding through Lua's C frames needs care) or have the converter turn them into error values, which rewrites control flow. Implementing is the faithful route | L |
| **RTTI** | 647 `dynamic_cast`, 93 `typeid` | Type descriptors behind the vtable, `dynamic_cast` through them (single inheritance only, which is the common case) | M |
| **SIMD intrinsics** | 1,122 `_mm_*` calls (bitmaps, atlas, DXT compression) | Intrinsics as compiler builtins lowered to LLVM vector operations; imported `immintrin.h` declares nothing usable | M |
| **Statics and global objects** | about 5,000 `static` declarations; codegen rejects globals with constructors and arrays of objects | Static locals with one-time initialisation, global constructors in a defined order (per file, files in dependency order), arrays of objects | M |
| **`constexpr`** | 4,517 uses, mostly constants, some functions | The constexpr interpreter from the design: steps 1 to 3 are done (typed values with C++'s arithmetic, floats, function bodies with loops and assignments, folds over packs, objects, constructors, constant arrays and strings, class operators, virtual calls, references; 2026-10-29); step 4's interpreter half (brace lists, InitializerList, range-for, addresses, member pointers, bit fields at their width; the value contract in consteval.hpp) is done, the code generator's lowering of constant data is its thread's | M |
| **Smaller gaps** | multiple inheritance beyond the first polymorphic base, init-captures, member pointers, bit fields, `goto` (15), stream operators (136), partial specialisations (about 6) | Each small; the converter's error list tells which to do first | S each |
| **Macro replacements, the big four** | the X-macro lists, `ADD_VALUE` reflection, `InputAction`-style unions, UnitTest++ | design.md section 8: `for constexpr`, `members(T)`, `union enum`, `test` blocks (after review) | L in total |
| **Resolver tail** | 22% of expressions untyped today | Most of it is `std::`; the rest is templated `using` aliases, dependent lookups and ADL, fixed as the converter exposes them. And the checker checks every member function of a class template instance whether the program uses it or not (GamePair::str, which its instances cannot compile): C++ instantiates a member only where it is used, and the eager checks put failures no program would see into the `--why` statistics (a static_assert in such a member already counts only when the function is emitted) | M, continuous |
| **C libraries** | Lua (1,665 includes), SDL3 (69), curl, zlib, png, jpeg, opus, vorbis, zstd, openssl, minizip, stb_image, fontstash, nvapi, Logitech, CL | `extern "C" import` works. Lua is imported as C now and converted later (kovarex, 2026-10-28); the converter expands the function-like macros of lua.h, lauxlib.h and luaconf.h at their 642 uses (programs/factorio/overrides/expand-macros.txt). Lua's internals, which 12 of the game's files reach into (index2addr, TValue, Table, lobject.h's macros), are imported too, through a C bridge ("Lua's internals through a C bridge" below) | S |
| **C++ libraries** | tracy (62), steam (62), glm (19), re2 (13), imgui (9), cxxopts, open-vcdiff, StackWalker; metal-cpp (193) is macOS only | Steam has a flat C API; tracy a C API; re2 and imgui need thin C wrappers; glm is a port (small); the rest are replaceable | M |
| **DirectX 11** | 17 includes, COM interfaces | COM is C-callable through vtables; the importer must take the `lpVtbl` form of the headers, or a thin C wrapper does it | M |
| **Debug info** | | `!dbg` metadata for CodeView so the game can be debugged in Visual Studio; not needed to run, needed to develop | M |
| **The converter** | 1.33 M lines, 8,056 files | Removes includes and forward declarations, inserts `this->`, `explicit`, `fallthrough`, rewrites enums, macros (per the survey) and `std::` to the library, reports global-scope name clashes; semi-automatic, with the compiler's errors as the worklist | L for the tool, XL for the iteration over the corpus |

Not needed for a Windows build: Metal, the Switch and Linux paths, PCH and unity-build
machinery, FASTBuild.

## The converter, first run (2026-10-24)

`kppc --convert OUT <dirs>` rewrites the parsed and resolved C++ into Kpp: preprocessor lines
evaluated for Windows/MSVC/debug (`#include` of C headers becomes `extern "C" import`, the rest
goes), constant macros become `const auto`, other macros stay as `// TODO(kpp)` comments,
forward declarations go, converting constructors get `implicit`, members get `this->`, a case
that falls through gets `fallthrough;`, `typename`/`template` disambiguators go, and `std::`
names map to the library's spelling (`std::string` to `String`, `std::vector` to `Vector`, 150
names). A header and its source of the same stem merge into one `.kpp` file; `CONVERSION.md` in
the output lists what was done and what is left per file.

On `libraries/CommonUtil` (29 files, 4,140 lines): 21 Kpp files, 1,008 `std::` names renamed,
109 forward declarations removed, 26 macros turned into constants, 4 kept as comments. kppc
parses the result 100% clean and types 88% of its expressions with the current library; the
unresolved rest is the library worklist, in order: `Pair` (350 uses), `StringView` (38),
`forward`/`move`, `min`/`max`, the type traits (`isArray`, `isSame`, `RemoveConst`...),
`WString`, `npos`, `RuntimeError`, `Set`, `Deque`, `Chrono`, and `String` methods (`begin`,
`end`, `push_back`, `erase`, `resize`, `find_first_not_of`, `replace`), then `va_list` and the
`trio_*` printf functions behind `ssprintf`.

Later the same day, with that library worklist done, exceptions implemented and hand-written
overrides possible (`programs/factorio/convert.cmd`, `--overrides`): 100% of files parse, 96% of
type names and 88.6% of expressions resolve, and code generation stops at 53 places (from 82
before the library work). By kind: 13 member accesses the library lacks (`rbegin().base()`,
`wstring_convert`/`codecvt`, `chrono` durations), 8 reference bindings and constructor calls
with no matching library constructor (`String` from a `char`, `Vector` from a brace list,
`StringView` into a `String&`), 7 locals of unknown type (the anonymous unions `Math.kpp` uses
for float bit casts, `thread_local`, `auto begin = ...data()`), 6 unresolved calls (`va_start`,
`va_copy`, the `EXTRACT_WORDS` macro, `trio_snprintf`), 3 `String` to integer conversions in
`strtoint`, 3 unknown function types in a dropped `#if` region of `Demangle`, and single cases
of `Optional` in boolean context, `nullopt`, `NAN`, globals with constructors. Nothing in the
list is a design question; the chrono/codecvt/varargs files are the first candidates for
hand-written overrides, the rest is library and compiler work.

## CommonUtil compiles, links and runs (2026-10-25)

`programs/factorio/convert.cmd` now converts `libraries/CommonUtil`, compiles the result with
zero code generation complaints, links it (without `SystemClipboard`, which needs SDL) with
`programs/factorio/tests/smoke.kpp`, runs it and compares the output with `smoke.expected`:
trimming, splitting, `ssprintf`, number parsing with its `InputException`, UTF-8/UTF-16
conversion, the fast float approximations and fdlibm's `pow`, `LowercaseString`, `CpuTimer`.

What it took, in the order it was found:

- **Hand-written overrides at definition granularity.** `--overrides DIR` used to replace whole
  files; now a file under `overrides/` is parsed, and every function, class or global it defines
  replaces the converted definition of the same qualified name (functions by arity; a name
  defined once replaces all its overloads, since the original's parameter list may hide behind
  a macro such as `FORMAT_STRING(...)`; a definition the parser did not recognise is removed by
  text). The rest of the override file is appended. Four files have overrides, 24 definitions:
  `CpuTimer` on the library's `Clock`, `widen`/`narrow` on `MultiByteToWideChar`, the trim
  functions without reverse iterators, `ssprintf`/`vssprintf` on `vsnprintf` and a static
  buffer, the four Math functions that used anonymous unions (now `bitCast<T>()`), fdlibm's
  word-access macros as small functions, and the unicode search map as a sorted static array.
- **Compiler features:** exceptions (section 9); free `operator+(const char*, const String&)`
  and friends found from either operand; conversion operators (`explicit operator bool`
  in conditions); `->` through `operator->`; converting constructors for `const T&` and by-value
  class parameters (`f("text")` with `f(const String&)`); globals with constructors or
  non-constant initializers (built at startup through `llvm.global_ctors`, in definition order
  within a file; destructors at exit do not run yet); arrays from nested brace lists and unsized arrays sized by their initializer;
  `va_start`/`va_end`/`va_copy` and functions defined with `...`; array parameters as pointers;
  MSVC integer suffixes (`1i64`); `(DWORD)-11` folded as a constant; integer literals typed by
  value as in C++ (`0xffffffff` is unsigned; before, `ui & 0xffffffff` kept all 64 bits);
  template parameter types recognised in casts (`(T)x`); out-of-line qualified definitions
  (`void StringUtil::f() {}`) attached to their namespace; a template declared in one block and
  defined in another; `T = const int` keeps its const when deduced through `T*`; function
  template specialisations get distinct names (resolution of calls to them is still naive);
  pointer arguments are no longer silently converted to integers; the linker drops unused
  functions (`-ffunction-sections`, `/OPT:REF`) and links the usual Windows libraries, `--lib`
  adds more.
- **Parser bug worth remembering:** `index < 0 || index >= this->length` was parsed as the
  template-id `index<0 || index>` followed by `= this->length`, because `>=` was split into
  `>` `=` as a template close. C++ splits only `>>`. Every such condition in the library and in
  converted code compiled to an assignment; the smoke test found it through `String::erase`.
- **Library:** `data()` on String, StringView, WString, Vector, Span (the fields are `chars` and
  `items` now); `back()`/`front()` by reference; `erase` by pointer range; `find_first_of` with
  a start position and a StringView set; `Vector(int count)`; `Optional` with `nullopt`,
  `value()`, `*`, `->`, `operator bool`; `InitializerList<T>` (call sites with brace lists are
  not generated yet); `Time.kpp` (Duration, TimePoint, Clock on QueryPerformanceCounter);
  `bitCast`; `lowerBound` with a comparator; `Limits<float/double>::quiet_NaN()`; `NAN`; `abs`
  for floats. Converted `std::sqrt` and the other C library functions become `::sqrt`, so a
  namespace's own `sqrt` wrapper does not call itself.
- **Converter:** `<cstdint>`-style headers map to their C names, `stdio.h` joins the common
  imports, `conio.h` and more Windows headers import, `trio_*` becomes the C runtime's printf
  family, explicit instantiation declarations go, `std::` names in `#else` branches the parser
  never tokenized are renamed by text.

Still open from that round: calls to function template specialisations, brace lists passed to
`InitializerList` parameters, `thread_local`, destructors of globals at exit, wide string
literals (`L"..."`), and SDL as a linked library for the few CommonUtil functions that use it.

## Towards Agui (2026-10-25, later)

`libraries/Agui` (235 files, 132 Kpp files after conversion) parses 100% clean and types 92.6%
of its expressions; code generation started at 1,208 complaints and stands at 249 after one
pass over the biggest categories (`compiler\build\tests\agui` is the scratch directory; the
scouting command is the check step of convert.cmd pointed at the Agui output):

- **Non-type template parameters** (`template<typename T, int N> struct Array`, `bool` defaults
  as in CommonUtil's `UniquePointer`): a value argument is a type of a new kind (`Value`, the
  constant and its type), so template argument lists, instantiation keys and substitution did
  not change shape; `N` in the body is a constant of the instantiation; defaults for
  parameters not written; `Array<char, bufferSize>` with a constant as the argument (the parser
  reads a bare name as a type, the resolver evaluates it). Test: `valueparams.kpp`.
- **Alias templates** (`template<class T> using Ptr = UniquePointer<T>`), member typedefs
  through instantiations (`Vector<int>::iterator`, also through an alias of the class),
  out-of-line member definitions seeing their class scope for parameter types, reference
  members (initialised with the address, never destroyed), friend declarations dropped by the
  converter.
- **Pointers to data members**: `&Style::field` is the field's offset (an `i64`), `.*`/`->*`
  address it, `T C::*member` parameters deduce both the member and the class type: Agui's
  Style properties (`getProperty(&Style::defaultGraphicalSet)`, 290 sites) compile.
  Test: `memberptr.kpp`. Pointers to member functions are not supported.
- **Library**: `UniquePtr<T>` with `makeUnique` (what `std::unique_ptr` converts to; CommonUtil's
  `UniquePointer` with a deleter exists beside it), `Queue<T>`, `Optional::emplace`, `at()`,
  `insert` at a position (also of a range), `replace`, `advance`, `makeOptional`, iterator
  typedefs; `!object` through the class's bool conversion, class-typed `?:`, `(void)x`,
  delegating constructors.

The second pass (2026-10-25, later) took Agui from 249 to 103 complaints:

- **`Function<R(Args...)>`** (stdlib/core/Function.kpp): the compiler reads the signature and
  makes `Function<void(int)>` mean `FunctionImpl<void, int>`, a library class with the callable
  on the heap behind `invoke`/`destroy`/`clone` function pointers to member template
  specialisations; lambdas with captures, function pointers and functors; copied, moved,
  reset, tested for emptiness. Test: `function.kpp`.
- **Value categories.** Overload resolution now knows an lvalue from an rvalue: an lvalue
  argument presents itself as `T&`, binds to no `T&&`, and a forwarding reference `T&&` given
  an lvalue deduces `T = U&` (reference collapsing), so `emplace_back(name)` copies and
  `emplace_back(move(name))` moves, as in C++. Code generation copies lvalues through the copy
  constructor only (it used to take the last same-class constructor, the move constructor, and
  emptied the source), moves from `move(x)` and temporaries, and gives classes without copy or
  move constructors memberwise moves. Deleted functions are diagnosed when chosen. Library:
  `Vector`, `String`, `WString` have move constructors and move assignment, `push_back(T&&)`.
  Test: `moves.kpp`.
- Range-for over iterator objects (`rbegin()`, `ReverseIterator<T>`), init-captures
  (`[x = move(x)]`), pack expansions as the only constructor argument (`T(forward<Args>(args)...)`
  for copies, moves and aggregates), default arguments carried by the converter from a dropped
  header declaration to the definition, diagnostics naming the instantiation and who requested it.

The third pass (2026-10-25, later) took Agui from 103 to 6 complaints, with several fixes that
matter beyond Agui:

- **Overload resolution** now rejects a template whose deduction left a parameter dependent
  (`operator<<(UniquePtr<T>&)` given a `Label&` used to rank as "unknown" and win), checks
  viability even for a single candidate (`owner == raw` went through the explicit
  `UniquePtr(T*)` and deleted the object twice), deduces `T&` from a const lvalue as `const T`
  (so `forward<Args>` works for `const Event&` elements), treats a converting constructor from
  another instantiation of the same template as a conversion, and takes the callable's
  parameters into account when a `Function<R(Args...)>` is built from a lambda
  (`onSliderMove(Function<void()>)` next to `onSliderMove(Function<void(double)>)`).
- **Code generation**: calls through function pointers use the Kpp calling convention
  (classes and references by pointer, sret); casts to references are lvalues (a `forward`
  returning a scalar reference used to return the address of a dead temporary); `?:` with two
  objects yields one of them; brace lists as arguments (`push_back({a, b, c})`,
  `max({a, b, c})` through `InitializerList<T>`), as ranges, and as member initializers;
  arrays of objects with constructors; assignment through converting constructors
  (`invertColors = InvertColors::True`, `view = string`); enumerators bound to references;
  `constexpr static` members as constants; `using Base::f` members; nested enums of a base
  through a derived class; local type aliases as variable types; out-of-line constructors of
  class templates (`template<typename Policy> ScrollBar<Policy>::ScrollBar(...)`).
- **Class template argument deduction** from a constructor (`ScopedSetter guard(x, true)`).
- **Converter**: function-like macros defined in a .cpp are expanded at their uses
  (`HANDLE_CASE`, `E(i, j)`), `unique_ptr<T[]>` becomes `UniqueArray<T>`, std algorithms and
  `move`/`forward`/`swap` are spelled `::name` (a class's own `move` or a namespace's `anyOf`
  no longer captures them), `a & b` and `a && b` in conditions are no longer read as reference
  declarations.
- **Library**: `HashSet<T>`, `UniqueArray<T>`, `remove`, `max`/`min` of a brace list,
  `Vector::insert` of rvalues and `erase` returning the next position, `cbegin`/`crbegin`,
  `String(first, last)`, `Optional == value`, `UniquePtr == pointer`/`nullptr`, `hashOf`
  for pointers. Tests: `conversions.kpp`, `library2.kpp`, `callables.kpp`, `refparams.kpp`,
  `fnpointer.kpp`, `outofline.kpp`.

Two more fixes closed the pass: a template body re-typed for an instantiation now looks its
qualifier up from the definition's namespace (`ScrollBar<Policy>::ScrollBar` in `agui` could
not see `TextButton`), and `typeid(x)` yields the library's `TypeInfo`: every vtable starts
with the class's name, so the dynamic type of a polymorphic object is one load away;
`typeid(T)` and non-polymorphic objects get their static name. Test: `typeid.kpp`.

The generic lambda (an `auto` parameter) was the one construct left out at this point:
`Table::findFirstRowWhere` was a hand-written template in `programs/factorio/overrides/Agui/Widget/Table.kpp`
until generic lambdas arrived the same evening (below); the override is gone and `Table.cpp`
converts as written.

## Agui links and runs (2026-10-25, later)

The converted Agui (132 files) plus CommonUtil link with `programs/factorio/tests/agui_smoke.kpp`
into one program, and the program computes a layout: a `Gui` of 640 x 480 with its Void backend
(the backend Factorio uses for headless runs: fonts of 10 pixels per character and 20 per line),
a `Label` added to the top container, `logic()` once, and the label measures 100 x 20 for
"Hello Agui". convert.cmd builds it and compares the output with `agui_smoke.expected`, next to
the CommonUtil smoke test. Agui's style properties are inherited along a parent chain whose root
must define every property (the game fills the roots from its Lua prototypes); the smoke test
fills the three root styles by hand. `SystemClipboard` (SDL) is stubbed in `tests/stubs`.

Linking found four gaps: a function declared in several modules was declared twice in one
(declarations are now deduplicated per module), `= default` members referenced through vtables
or function pointers had no body (they are synthesised through the worklist), out-of-line
members of class templates were never paired with their declarations (the pairing used the
first part of the qualified name), and `static` written on the declaration only was lost on the
definition. Running found seven miscompiles that compile-time statistics, linking and the test
suite had all passed over, each now a test:

- Function-local statics were allocas (`static Style result(nullptr, nullptr)` in
  `Widget::getStyleStatic`): now a guarded global of the program. `staticlocal.kpp`.
- Reference globals were objects (`static const Style& defaultWidgetStyle = getStyleStatic()`):
  now a pointer bound by the global constructor. Out-of-line definitions of static data members
  (`double Gui::scale = 1.0`, `const Style& Widget::defaultWidgetStyle = ...`) are paired with
  their in-class declarations, so the initializer is no longer lost, and a constant defined that
  way serves as an array bound. `staticmembers.kpp`.
- `this` in a const member function was `C*`, so `this->getStyle()` picked the non-const
  virtual overload (null for the top container) over `getStyle() const`. `constthis.kpp`.
- A global of class type without user constructors was zero-initialised even when the class has
  default member initializers, a vtable pointer, or members that need constructing.
  `constthis.kpp`.
- `Derived*` to `Base*` skipped the adjustment to a base that is not at offset 0 (a polymorphic
  `Widget` over the plain `GenericTargetable`: the base sits after the vtable pointer), because
  both are `ptr` in LLVM; and a null pointer must stay null across that adjustment, in both
  directions. `nonprimarybase.kpp`.
- An out-of-line static member emitted before any call to it kept a hidden `this`
  (`ResizableText::splitNewlinesOnly` read its arguments shifted by one and produced empty
  lines): the main pass now pairs every out-of-line definition with its declaration first.
  `staticoutofline.kpp`.
- Default arguments of constructors called by default construction and of members of class
  templates (re-typed for the instantiation); `using namespace` inside a body. `defaultargs.kpp`,
  `templatemembers.kpp`, `outoflinedefault.kpp`, `defaulted.kpp`.

Crashes were located without a debugger: `--lib "-Wl,/MAP:file"` writes the linker map, the
smoke program installs a vectored exception handler that prints the faulting address relative
to the module and every stack slot pointing into the image, and a small script resolves them
against the map. The fastest route to each cause was to add `printf` lines to the converted
source (convert.cmd regenerates it), then shrink the finding to a probe against the standard
library alone (seconds to build, against 1.5 minutes with CommonUtil and Agui), then to a test.

What was left in Agui proper at that point: generic lambdas and `dynamic_cast` (both done since,
see below), pointers to member functions, thread-safe statics, destructors of globals at exit. Next: the SDL backend through C import and a
window, or the game's `Util` library, whichever the game's dependency order wants first.

## The specification suite (2026-10-25, later)

kovarex's direction for the next step: instead of learning what the compiler lacks by converting
and recompiling the whole game at every step, build a suite of small snippets, one per construct
the Factorio sources use, and let the suite be the specification of what is needed.
`compiler/tests/spec/<area>/<name>.kpp` holds one program per construct with its expected output;
`compiler\tests\spec.cmd` compiles and runs them all (129 snippets in two seconds) and writes
[spec-status.md](spec-status.md), where every construct is pass or pending (a known gap, with the
compiler's first complaint and the construct's count in the game's sources). A pending snippet that
starts passing fails the run until its pending line is removed, so the document never goes stale.

The construct counts come from a regex survey over the 7,371 files of `src`, `CommonUtil` and
`Agui` (scratch script, numbers in the snippet headers): default arguments 32k, `override` 16k,
`std::string` 11k, range-for 8.6k, lambdas 5.3k, `std::vector` 5.2k, `std::unique_ptr` 3.8k,
`throw` 3.1k, operator overloads 2.9k, `std::string_view` 1.7k, `std::optional` 1.6k,
`std::function` 1.5k, `requires` 1.3k, `std::map` 827, `dynamic_cast` 443, generic lambdas 330,
`std::array` 372, `std::span` 147, variadic templates 175, fold expressions 110.

The first run found 49 gaps (80 constructs pass), among them several that the Agui smoke test had not reached: aggregate
initialisation drops default member initializers of omitted members, two-dimensional array
initialisation, assigning through a returned reference, binary literals, designated initializers,
`goto`, bit-field layout, `alignas`, structured bindings (one form crashes the compiler), partial
specialisations silently ignored, per-instantiation static members of class templates shared, the
recursive variadic pattern, `emplace_back(7)` into `Vector<int>`, `::sort` without a comparator,
and the library's missing `Map`, `Tuple`, `Array`/`Span`, `SharedPtr`, `Variant`, threads.
Small library additions came out of it too: `memset`, `abs(int)`, the C math functions,
`OutOfRange`/`InvalidArgument`, `UniquePtr = nullptr`.

## Working down the suite (2026-10-25, evening)

kovarex's rule for the suite: fix errors first. In that order, one commit per few fixes, each
checked against the suite, the 40 end-to-end tests and the CommonUtil and Agui smoke tests:

1. **Crashes.** `auto& [w, h] = s;` crashed the compiler: the parser only recognised bindings
   under a plain `auto`, and the resolver and code generator had nothing for them. Bindings now
   type and address the matching member (or array element) of the hidden object, by value and
   by reference.
2. **Silent miscompiles.** Aggregate initialisation dropped default member initializers of the
   members left out; `int a[2][3]` was built as `int[3][2]`; `cond ? a : b` was no lvalue, so
   `pick(x, y) = 20` wrote into a temporary; `0b1010` was 0; `return local;` copied instead of
   moving; two conversion operators shared one symbol; a `Derived*` to a base after the vtable
   pointer was not adjusted and null did not survive the adjustment (found the same day by the
   Agui smoke test).
3. **Silently ignored.** Partial specialisations (the primary was used), static members of class
   templates (one object for all instantiations), designated initializers (taken positionally),
   constexpr functions as array bounds (bound 0), requires clauses (not consulted), alignas,
   bit-field widths, dynamic_cast (static), `goto`.
4. **Compile-time gaps.** Enum values as template arguments; a template calling itself deducing
   its own parameters afresh; `Args&&...` packs under references and `T(forward<Args>(args)...)`
   for scalar T; alias templates (their parameters were invisible while the target was converted);
   `typename T::Unit` and `Num<I>::Type` as dependent member types resolved on substitution (a
   new type kind, `DependentMember`), with value arguments matching specialisations; nested
   classes of class templates carrying the enclosing arguments; out-of-line members of templates
   with value parameters; default template arguments of function templates after deduction
   (`EnableIf` removes a candidate); `Conditional`; `sizeof(T[N])`; wide string literals; calls
   leaving a required parameter without an argument are now an error instead of a call with
   garbage (which is what `agui::label()` in the smoke test had been).
5. **Library.** `Map`, `Array<T, N>`, `Span<T>` (Vector and Array convert to it), `Tuple` with
   `get<I>`, `SharedPtr`/`WeakPtr`, `Thread`, `Mutex`, `LockGuard`, `UniqueLock`,
   `ConditionVariable`, `Atomic` (SRW locks, CreateThread, and `kpp_atomic_*` builtins the
   compiler turns into atomic instructions), `Vector` from a brace list, `memset`, `abs(int)`,
   the C math functions, `OutOfRange`/`InvalidArgument`.

6. **Generic lambdas** (330 Factorio uses) and deduced returns of function templates. `[](auto x)
   {...}` is a template: the parser invents a type parameter per `auto` parameter (as C++ does),
   the closure carries the parameter list, and the call operator is typed per call. The type
   checker keeps, for every lambda with template parameters, the environment it was written in
   (the enclosing locals, scope, `this`, instantiation); a call deduces the lambda's parameters
   from the arguments like a function template's, types the body in that environment once per
   set of arguments, and takes the result type from there. The code generator emits one call
   operator per instantiation, at the first call and after the call's arguments: a lambda
   argument's capture list is read where it is written, so its closure has to exist before the
   operator's body calls it (found by Agui's `Table`, where a predicate lambda captures another
   lambda and `this`; the closure of a `const auto f = [this](int) {...}` is the expression's,
   found by `ElementImageSet`). A lambda type depends only on parameters it does not bind itself,
   so a generic lambda can be a template argument. `auto&`, `const auto*` and plain `auto`
   returns of function templates take the written shape of the deduced type and are recorded on
   the call like any deduced arguments. `Table.cpp` converts as written; its override is gone.
7. **`for constexpr`** (design.md 8.2), the first of the section 8 features: `for constexpr
   (typename T : types<A, B>)` and `for constexpr (int i : {1, 2, 4})` inside functions. The loop
   variable is a template parameter of the body; the body is re-typed and generated once per
   element exactly as a pack expansion is (the resolver's per-instantiation re-typing with the
   element bound, then the statement emitted under those arguments), so `T::name()`,
   `squared<i>()` and `int buffer[i + 1]` work in it. Spec: `kpp/for_constexpr_values`,
   `kpp/for_constexpr_types`, `kpp/for_constexpr_constant`; `kpp/for_constexpr_switch` (one
   `case` per type) is the next step.
8. **Reflection** (design.md 8.3) on the same machinery: `for constexpr (auto m : members(T))`
   binds `m` to a member descriptor (a new compile-time type kind holding the member, its type
   in the class's instantiation and the class), `m.name` is a string constant, `m.offset` the
   field's offset, and `object.[m]` the member itself (an lvalue, so save and load loops can
   read and write it). Spec: `kpp/reflection_members`, `kpp/reflection_bases_offsets`.
9. **`E::fromStr(text)`** (design.md 8.4): generated on every Kpp enum as `static Optional<E>
   fromStr(const char*)` and lowered to the library's `enumFromStr<E>`, written in Kpp over
   `E::values` and `str()`. Found on the way: a qualified name through a template parameter
   bound to an enum (`E::values`, `E::count` with E = Direction) stayed dependent; the lookup now
   follows enum substitutions as it did class ones. Spec: `kpp/enum_from_str`,
   `templates/enum_through_parameter`.
10. **Typed unions** (design.md 8.5), the shape of `InputAction` and `std::variant` uses. The
    parser desugars `union enum Property : u8 { { {int, Integer}, {String, Text} } ... }` to a
    class with a nested Kpp enum `Tag` (unscoped, so `Property::Integer` is the tag value), a `tag`
    member and an anonymous union of one storage member per alternative; the type checker gives
    `p[Property::Integer]` the alternative's type and `p == Property::Integer` bool; the code
    generator makes `p[E] = v` destroy the alternative held, set the tag and construct the value,
    checks the tag on every other `p[E]` (release_assert), switches on the tag for `switch (p)`
    (exhaustive like a Kpp enum), and copies, assigns and destroys by the tag. Spec:
    `kpp/typed_unions`, `kpp/typed_unions_switch_copy`.
11. **`case` labels from `for constexpr`** (the third example of design.md 8.2): a switch body
    walks its `for constexpr` statements once per element to collect the labels (the Kpp enum
    exhaustiveness check sees them too) and once more to emit them. **Pointers to member
    functions** (3 Factorio uses): `int (Calc::*op)(int) const = &Calc::mul;` parses as a
    declaration whose type keeps the class and the function type, `&Calc::mul` is the function's
    address (virtual ones are refused for now), and `(c.*op)(3)` calls through it with the
    object first, sharing the indirect-call emission with function pointers. Spec:
    `kpp/for_constexpr_switch`, `classes/member_function_pointers`.
12. **Template template parameters** (one Factorio use): `Container<T>` with `Container` a
    template template parameter is the parameter applied to arguments (a parameter type carrying
    them); deduction against `Vector<int>` binds `Container` to the template itself and `T` to
    `int`, and substitution applies the bound template to the substituted arguments. **The SDL
    header import** passes in the suite now that the runner hands kppc the game's SDL3 include
    directory when the Factorio checkout is present: `SDL_Rect` and `SDL_INIT_VIDEO` come straight
    from `SDL3/SDL.h`. Spec: `templates/template_template_parameters`, `interop/sdl_header`.

The suite went from 80 pass / 49 pending to 137 pass / 1 pending; the Factorio front end stayed
at 99.27% parsed and 77.6-77.8% typed (members of dependent qualifiers now count as dependent
instead of being guessed from the primary template). Left pending, in order of Factorio use:
only `Variant`, which stays pending by decision (a `std::variant` becomes a typed union; the
converter will need a mapping for the 24 uses). Not in the suite yet, and still open in design.md
section 8: `m.type`, `enumerators(E)`, `alternatives(U)`, and the `--tests` runner.

## src/Util through the converter: the first run (2026-10-25, night)

With the suite's compiler-side list empty, the next module of the roadmap is the game's `Util`
(370 files, 47k lines; 144 .cpp and 226 .hpp merged into 304 Kpp files). The first run, with
CommonUtil and Agui as context and nothing hand-written yet:

```
compiler\build\kppc.exe --no-std --quiet --convert OUT --overrides programs\factorio\overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui C:\Factorio\src\Util
compiler\build\kppc.exe --std stdlib --cinclude C:\Factorio\libraries\SDL3\include --errors 60 --unresolved 60 --emit-llvm out.ll OUT\CommonUtil OUT\Agui OUT\Util
```

| | |
|---|---|
| files parsing clean | 96.26% (43 parse errors in 18 files) |
| expressions typed | 85.4% (7.3% dependent, 7.3% unknown) of 340k |
| members bound | 82.9% (4.5% on an object of unknown type, 3.2% no such member) |
| unresolved names | 8,719 (mostly other modules: `GlobalContext`, `Log`, `Map/MapSerialiser`, `RuntimeError`, Lua) |
| std names without a mapping | ~80 kinds: traits (`is_trivially_*`, `is_arithmetic_v`...), `variant_size_v`/`visit`, `priority_queue`, `reverse_iterator`, `future`/`async`, `strong_ordering`, `time_t`/`localtime`, `uninitialized_*` |

The parse errors fall into two piles. Compiler gaps, each now a pending snippet: declaration
specifiers after the type (`float static f()`, `void inline g()`, `const static int x[]`),
array and function types as alias targets (`using Row = T[N]`, `using Unary = double(double)`),
operator functions called by name (`!operator==(l, r)`), `__stdcall` inside function pointer
types, a `requires` clause between a member's qualifiers and its body, and deduction guides with
`using Ts::operator()...` (`make_overload`). Converter bugs, to fix in `convert.cpp`: default
arguments `= {}` written as `= ()`; a removed friend or macro-generated declaration leaving its
`template<...>` header or `[[noreturn]]` attributes behind; the printf-format attribute macro
written as `(4, 5)` after the parameter list; a macro standing for `extern "C"` turned into a
constant; and `__try`/`__except` (SEH in CrashHandler and SystemUtil, which want override files
rather than a translation). The C import also needs curl's include directory (`Util/Http`), and
the 8.7k unresolved names say Util cannot be compiled alone: the game's root headers (`Log`,
`GlobalContext`, `RuntimeError`), `Map` and the Lua binding come with it, so the next conversion
set is Util plus those, with stubs for what the smoke tests do not touch.

The second pass, the same night, closed every parse error. Parser: specifiers after the type
(`float static f()`, `void inline g()`, `const static int x[]`), array and function types as
alias targets, operator functions called by name, calling conventions dropped from the tokens of
every source (as the C importer already did), the `requires` clause before a body no longer read
as a braced construction, and deduction guides parsed (and dropped by the converter: Kpp deduces
from constructors). Converter: a carried default argument is the text after its `=` with std
names mapped (`= std::string()` had become `= ()`), a removed friend or declaration takes the
`template<...>` header and `[[attributes]]` written above it, `FORMAT_ATTR(4, 5)` goes with its
arguments, `NTAPI`/`WINAPI`/`EXTERNC` are known macros, and `__try`/`__except` become
`try`/`catch (...)` with a todo naming the dropped filter. Util: 0 parse errors in 304 files,
85.4% of expressions typed. The whole Factorio front end moved with it, from 99.27% to 99.54% of
files parsing clean (93 errors in 37 files, down from 208 in 59). Spec: `language/specifier_order`,
`language/alias_array_function_types`, `language/explicit_operator_call`,
`interop/calling_convention_pointers`, `templates/requires_clause_before_body`;
`templates/deduction_guides` stays pending (the `Overload{lambdas...}` idiom needs `using
Ts::operator()...` and aggregate deduction).

A first self-contained subset (`Util/Container`, `Util/Sort`, `Util/MathExpressions`, `BitBuffer`
with CommonUtil) compiles to 81.9% typed with 22 code generation complaints, and showed two
compiler gaps, both fixed the same night: a class template's default argument naming an earlier
parameter (`template<class K, class C = Less<K>>`) was converted without the parameters in
scope, so `Sorted<int>` got `Less<<unknown>>`; and a qualified name through a local alias
(`using Kind = Token::Special;` then `case Kind::None:`) was not followed into the alias's type.
Spec: `templates/class_template_default_arguments`, `language/switch_through_local_alias`. What
is left in the subset is the conversion set, not the compiler: `FlatMap`'s default comparator
lives on a forward declaration (`FlatMapForwards.hpp`), which the converter drops with the
declaration, and `FunctionArgumentIndex` lives in the game's root `Typedefs.hpp`; the converter
needs to carry default template arguments from forward declarations, and the Util set needs the
root headers.

Both done next: the converter carries default template arguments from forward declarations to
the definition's parameter list the way it carries default arguments of functions (`class Comp =
Less<>` on `FlatMap`), a single file can be a conversion input (`C:\Factorio\src\Typedefs.hpp`
joins the set), the library gained `Less<T>`/`Greater<T>` and their transparent `Less<>`/
`Greater<>` (std::less<>), and `Name<>` written with empty brackets takes every default. The
subset is at 82.6% typed with 15 complaints: the `LOG_*` macros (the game's `Log` is not in the
set), `Serialiser`/`Deserialiser` (`Map`), `FlatMap`'s initializer-list constructor (a loop
variable over `InitializerList<Pair<K, V>>`) and a brace list used as a value. Spec:
`library/less_comparator`.

## The Util smoke test runs (2026-10-25, night)

`programs/factorio/tests/util_smoke.kpp` evaluates four expressions with the game's
`MathExpressions::Evaluator` (`1 + 2 * 3`, `(1 + 2) * 3`, `10 / 4`, `2 * (3 + 4) - 5`) and prints
7, 9, 2.5 and 9: the tokenizer, the postfix tokenizer and the evaluator, their `FlatMap` tables
of operators and named functions (lambdas stored as function pointers), `Pair`, `StringView`,
`Span`, `debug_float_cast` and the exceptions they declare, all converted from C++ and compiled
by kppc. `convert.cmd` now converts Util with CommonUtil and Agui (461 files), links the subset
the evaluator needs (CommonUtil, `Util/Container` without `ExtendedBitBuffer`, `Sort`,
`MathExpressions`, `BitBuffer`, `ToStr`, the root `Typedefs`, `FloatCast`, `BitmapFormat`,
`NoiseRegisterIndex`) with four stand-ins in `tests/stubs` (`Log.kpp`: `logAndAbort`, which the
converter now makes of `LOG_AND_ABORT` and `LOG_ENUM_AND_ABORT`; `UtilHelpers.kpp`: the two map
helpers of `Util/Util.hpp`, whose whole header wants `Result`, SDL message boxes and regular
expressions; `MathStubs.kpp`: `Math::fabs`, since `Util/Math.hpp` brings the CPU intrinsics;
`FloatCast.kpp`: the failure handler that logs through the global context), and compares the
output.

Getting there took, in the compiler: class template argument deduction in expressions
(`Pair(a, b)`, `Span(p, n)`), `Vector::emplace(position, args...)`, range-for bindings inside
instantiations, `Alias{}` of a scalar, `Map`/`Set` from brace lists, `Less<>` and `Name<>` taking
every default, a conditional of a class and a converting branch having the class type, lambdas
without captures as pointers to function (a thunk per lambda, also for pointer-to-function
parameters and references to them), and the KPP_DEBUG_CALLS trace no longer crashing on
operators; in the converter: the function-macro table with text around `$1` (`toCStr`), the Log
macros, default template arguments carried from forward declarations, a single file as an input.

The first run linked and died at startup with heap corruption, exit 0xC0000374. Unbuffered marks
on stderr (`fputs` + `fflush`, each followed by `HeapValidate`) placed it: `PostfixToken`'s
constructor computes `number(type == Number ? strtodouble(String(data, length)) : 0)`, and the
code generator destroyed the `String` temporary at the end of the statement whichever branch had
run, freeing an unbuilt object for every token that is not a number. Temporaries made inside a
branch of `?:`, or inside the right operand of `&&`/`||`, now die inside that branch (spec:
`language/temporaries_in_branches`). Same lesson as Agui's: compiling and linking prove little,
running does. Spec suite: 156 pass, 2 pending.

## The whole converted set through the code generator (2026-10-26)

Running `kppc --emit-llvm` over everything converted (CommonUtil, Agui, Util and the root
headers: 488 files) is the next check after the smoke tests, since it exercises the generator
on all the code that types. It first died three ways: `friend Description;` (a friend class
named without the keyword) made an unnamed member that crashed the member walk; `operator<=>`
and `this->listValue.~vector()` were not parsed; and a value of unknown type passed where a class
is expected looped for ever through the copy constructor (LabelString, from `src`, is not
converted yet). With those fixed, the real gaps the report named each got a pending snippet and
then a fix: the defaulted three-way comparison (45 of Factorio's 58), `switch` on an object with
a conversion to an enum (Direction, Dimension), `String(count, char)` and the count forms of
`append`/`insert`, members found in a base taking the base instantiation's substitution
(`movement.x` on a `const Vector2D&` was typed `T`), `Table::value_type(65)` through an alias of
an instantiation, static constexpr members of instantiations, inherited constructors next to the
class's own, `explicit(Explicit)` on FixedPointNumberTemplate's constructors, requires-expressions
in `if constexpr`, `isConstructible<T, Args...>`, variable templates, explicit arguments binding a
trailing pack, `3 == id` through the reversed `operator==`, `Vector::operator==`. The game's own
2D `Vector` is spelled `Vector2D` in the conversion, since `Vector` is the library's std::vector.

The report went from about 1830 lines to 1650; what is left is almost entirely names from
modules not yet converted (Log, GlobalContext, Lua, Map, the root `src` headers), which the next
widening of the subset addresses. Spec suite: 173 pass, 2 pending (Variant by decision, the
Overload deduction-guide idiom). Front end unchanged at 99.54% of files parsing, typed
expressions 78.0% to 78.5%.

## The order that gets to a running game

1. Decide exceptions and RTTI; implement them (they shape what the converter emits).
2. Library parity, driven by the converter on `libraries/CommonUtil` first (313 includes, its
   own unit tests), then `Agui`, then `Util`: the first real modules of the game compiled and
   tested by kppc, with the resolver and codegen gaps fixed as they appear. (CommonUtil and
   Agui compile, link and run smoke tests as of 2026-10-25.)
3. Statics, intrinsics, constexpr and the small language gaps in the order the converter reports
   them.
4. The section 8 features, which the macro rewrite needs.
5. Lua, SDL3 and DirectX through C import; a window with the game loop; then the rest of `src`
   module by module, measuring the typed percentage climbing to 100.
6. Debug info, then the persistent compiler for the edit-build-run loop.
7. Release-build code quality: inlining across chunks, by ThinLTO or by kppc inlining trivial
   functions at IR emission (design.md section 2, decided 2026-10-28).

The compiler-side items are each days to a couple of weeks at the pace of the work so far; the
long pole is the conversion of 1.3 M lines, mechanical once the converter exists but a long tail
of constructs and library idioms, so it should start on CommonUtil as soon as exceptions are
decided rather than after the compiler is "complete".

## Full-src baseline (2026-10-28)

The compiled subset now includes everything: CommonUtil (29 files), Agui (235 files), and the
entire `src/` directory (3,779 files), plus tests. 4,043 Kpp files through the code generator,
86,115 template instantiations before the OOM cap (5,172 remaining), 32,884 codegen errors.
(The command behind this first figure was not recorded; it is superseded by the definition below.)

### The headline number: how it is measured

The one number tracked from here on is **full-src codegen errors**: the error lines the code
generator writes for the converted CommonUtil, Agui, cxxopts, StackWalker, Lua and `src/`. After `convert.cmd`
(or just its conversion step into `kpp\`), from `programs\factorio`:

```
..\..\compiler\build\kppc.exe --std ..\..\stdlib ^
  --cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\SDL_Mixer\include ^
  --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include --cinclude C:\Factorio\libraries\glad\include --cinclude native\include ^
  --errors 0 --unresolved 0 --emit-llvm kpp\full.ll kpp\CommonUtil kpp\Agui kpp\cxxopts kpp\StackWalker kpp\Lua kpp\src 2> kpp\full-errors.txt
find /c /v "" kpp\full-errors.txt
```

- Inputs: exactly `kpp\CommonUtil`, `kpp\Agui`, `kpp\cxxopts`, `kpp\StackWalker`, `kpp\Lua` and `kpp\src`, not the
  whole `kpp\`: that directory also holds the `kpp\Util` copy of `kpp\src\Util` the smoke tests
  use and whatever stubs were copied in. `kpp\cxxopts` is in since 2026-10-28 (see "cxxopts"
  below); a count from before that leaves out its errors (13 when it came in). `kpp\Lua` is in
  since 2026-10-29: it holds only `LuaCPPUtilities.kpp`, the game's C++ overloads of Lua's API
  (`lua_pushstring` of a String, ...), converted from `libraries\Lua\LuaCPPUtilities.hpp` while Lua
  itself is imported as C. `kpp\StackWalker` is in since 2026-10-29 (see "StackWalker" below): the
  crash handler's stack walker, converted with the game; it came in with 1 error line and took the
  game's 8 StackWalkerToStream lines ("a base class names 'StackWalker'") with it.
- The SDL_Mixer include directory is required: the imported C headers compile as one unit, so a
  single missing header fails all of them and every Lua, SDL and C library use becomes an error.
  The run is valid only if its output has no `importing C headers failed` line and shows the
  header count of its tree (50 on 2026-10-28; 54 since glad's include directory joined on
  2026-10-29, when the converter imports `glad/glad.h` for the OpenGL backend).
- What it counts: every line on stderr, which is one code generator error per line (`file:line:
  codegen does not support ...`, `function type unknown, skipped`, ...). Front-end complaints
  (unresolved names, unknown expression types in the statistics on stdout) are not in it.
- The instantiation cap: there is no deliberate cap. When the code generator runs out of memory
  it stops the template worklist and writes one line, `codegen: out of memory after N template
  instantiations (M remaining)`. That line is included in the count; the errors the M remaining
  instantiations would have produced are not. Report the figure together with N and M. (Since
  69ffce0 it does not happen: the "out of memory" was an out-of-bounds read, not memory.)
- A run that ends with `codegen: out of stack (recursion without end) in F` stopped there: the
  code generator recursed without end on F and the process ended (continuing after a stack
  overflow can leave the heap broken, which is what made some runs spin for 400+ s until the
  fix of 2026-10-28). The count of such a run is not a headline; report F instead.
- The count moves by a few lines between identical runs (the parallel front end's instantiation
  checks race on the AST's type fields): take two runs.

Measured on master f688026 (2026-10-28): **12,750** codegen errors, out of memory after 88,868
instantiations with 7,758 remaining; 12 s on the 9950X (90eab80, before the SAVING_TAG change:
13,168, out of memory after 91,746 with 5,735 remaining). For comparison, the full-program step
inside `convert.cmd` (all of `kpp\`, 4,403 files, without the SDL_Mixer include, so the C import
fails) gives 15,976 stderr lines on f688026, of which 15,969 are codegen errors. Since 0d91626,
`convert.cmd` runs exactly the headline command and prints the count.

The converter's share, measured with one compiler (master 11dc873) and the converter as of
d40fb8a against the converter at 11dc873: **14,445 -> 12,352** (-2,093). What changed: BITFIELD
and MAKE_OPTIONAL expansions end with `;` (17 files failed to parse) and BITFIELD has
`contains<value>()` and `uint64_t`; SAVING_TAG and SAVING_STATISTICS are `_` guard locals; the
Log macros call Logging with the use's file and line; ADD_VALUE, SKIP_VALUE and ADD_ENUM expand
use by use, with `#NAME` as the argument's original spelling; the 31 hidden friend operators
survive (member operators, or functions at namespace scope). By package, the biggest moves:
Entity 1,278 -> 782, Graphics 1,175 -> 814, Logistics 316 -> 173, AI 186 -> 76, Fluid 120 -> 28,
src top level 949 -> 814; Util rises 1,722 -> 1,923 because code that failed to parse or
resolve before now reaches the code generator. The `.first`/`.second` failures are not a
missing Pair mapping (std::pair converts to the library's `Pair`): they are range-for loops with
`auto` over dependent ranges in templates, spec `templates/range_for_auto_dependent` (pending).

Silent fallbacks in the code generator turned into errors (2026-10-28), which raises the count;
it is a correction, not a regression: those lines were code compiled to something other than
what it says. On one frozen tree (master 5b17992's converter output), two to three runs each:
**11,409 to 11,415 -> 11,537 to 11,540** (+125). Of that, 146 lines are `a conversion from a
value of unknown type to T`: convert() used to treat a value of unknown type as a pointer, so a
condition on it became `!= null` (always true for anything non-zero), an integer became its bits
through `ptrtoint`, and a pointer passed it through. The rest of the change is the hang fix: two
recursions without end that overflowed the stack (10 functions, each `ran out of stack`, and
some runs then hung) are errors now: `converting A to B: the constructor chosen needs the same
conversion for its own parameter` (5) and `the class String, which contains itself` (4, in
NoiseExpressionStorage::getOrCreate; a layout with a member of its own type). Earlier in the day
the same rule removed other silent answers without changing the count much: copy-initialisation
from another class's prvalue (the result's bytes became the object), an inherited
`operator bool` never found (the object's address compared with null instead).

**Class layouts (2026-10-28): 9,754 to 9,758 -> 38,014 to 38,022.** The jump is the same kind
of correction, and the largest: the layout of a class silently dropped what it could not lay
out, and every function using the class compiled against that wrong layout. Four fallbacks: a
second polymorphic base was erased from the layout (its fields and vtable gone, its virtual
calls finding no base); a member of unknown type became 8 placeholder bytes (every later field
at a wrong offset); a base of unknown type, or a base or anonymous member whose own layout
failed, was skipped; and after a layout failed once, every later use got the half-built layout.
All are errors now (`the layout of C: ...` where it first fails, then `the class C, whose layout
failed where it was first used` in every function that needs it). About 260 root causes carry
the 28,000 new lines (one frozen tree, master 4c98ac0's converter output, two runs each):

- **140 classes with a second polymorphic base**: multiple inheritance of virtual functions is
  not implemented (one vtable pointer per object). Among them the core classes, so the cascade is
  large: `Entity` 2,125 failing functions, `Player` 1,034, `Surface` 671, `EntityPrototype` 541;
  `TriggerList<X> : ..., TriggerListBase` for every tip trigger. Without this one the count is
  28,579 to 28,596. Implementing it (secondary vtable pointers, this-adjusting thunks) waits for
  kovarex's scope decision (full multiple inheritance or one polymorphic base plus interfaces).
- **107 members of unknown type** (116 lines), by written type: time points and durations
  (`Chrono::steady_clock::time_point` 13, `TimePoint` 4, `Chrono::system_clock::time_point` 3,
  `Chrono::nanoseconds` and member pointers to it, `ClockDuration`, `Clock::time_point`, ...,
  about 35 in all); `UnderlyingType bits` in SmallBitset<N> (11); `Signal<>` members such as
  `onChanged` (11); a template parameter left in a member type (`T`, `T*`, `A`, `B`, `EntityT*`,
  `IndexType`, about 15); nested container types (`HashTable<...> table` 5, `RbTree<...> tree` 2);
  `Variant<...>` (4); one each of CURLcode, cxxopts::ParseResult, Gdiplus Status, a DXGI function
  pointer and a few local enums.
- **5 bases of unknown type.**

**Undeclared type names (2026-10-29): 23,197 -> 24,168.** A correction of the same kind: a type
name declared nowhere becomes the resolver's placeholder class (a draft, so that checking goes
on), and codegen laid that out as an empty class, so every function using it compiled against a
one-byte nothing. Now a layout that needs one is an error naming it (`the type 'X' is not
declared`, `the layout of C: the member m (...) names 'X', which is not declared`): 1,239 lines,
60 names. By name: CSteamID 803 (SteamID's base: the Steam API is not imported), GLuint 125 and
GLenum 16 (the OpenGL backend's object ids), CComPtr 80 and DXGI_SWAP_EFFECT 41,
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN 8, IStream 3 (the DirectX backend), RE2 25 (MacroReplacer's
pattern), `path` 24 (std::filesystem::path, Path's base), HAuthTicket 12, TooltipProvider 12,
ReferenceWrapper 11, StackWalker 8, ProtoAutoplacer 6, LevelRange 5. The same change has codegen
check the static_asserts on a class's size or alignment (the checker leaves them to the
layouts; those of class template instances were never checked): 12 fail, most because layouts
have no empty-base optimisation yet (CorpseSaveFlags, DrawingFlags, SamplingMode,
SpriteDrawOrder, ...), and 128 lines are DeserialiserHelper's catch-all `loadValue(..., T&,
Args&&...)` with its `static_assert(sizeof(T) == 0, "Unknown type")`, which overload resolution
picks for `Optional<X>` targets (silent before: the assert needs T's layout). About 400 lines of
other errors went, from functions whose first error is now the undeclared name. One tree (the
converter output of master eac68d2), master's compiler against this one.

**Unknown constant arguments in layouts (2026-10-29): 24,143 -> 27,742.** Another correction: a
class template instance whose constant argument the checker could not evaluate was laid out with
the argument as 0, so `char data[N]` was an empty array that every write ran past (and a member
or local array whose length was no evaluable constant had length 0 the same way). Now each is an
error (`the layout of Imm<<unknown>>: the argument for N is a constant the compiler could not
evaluate`, `... is an array whose length is no constant the compiler could evaluate`): 4,314
lines, nearly all from four gaps of the checker's constant evaluator, for the interpreter
(design.md, "Compile-time evaluation"):
- **A call of the library's max**, `ImmutableStringTemplate<::max(sizeof(SharedPtr<const String>)
  + 1u, 2 * sizeof(const char*) - 1)>`, PropertyTree's KeyType: 3,366 lines (every function that
  touches a PropertyTree). `::max(size_t(3), size_t(5))` alone does not evaluate.
- **A scoped enumerator cast to an integer**, `RenderLayer::LayerType(RenderLayer::LAST)` (DrawQueue's
  sprites, 605 lines), `size_t(TileRenderLayer::LAST)`, `size_t(AlertType::ALERTS_COUNT)`,
  `size_t(TileRenderGroup::Last) + 1`: the arrays of the draw queues and the alert center, about
  800 lines in all.
- **A constexpr static member computed by those**, InputAction::inputActionCount (Bitset<?>, 91).
- **A constexpr member function called on a constexpr array**,
  `ChunkGeneratedStatus::generationTaskStates.size()` (28).
About 740 lines of other errors went, from functions that now fail first at such a layout. One
tree (the converter output of master b2e6053), master 9909b71's compiler against this one.

**The interpreter's first step (2026-10-29): 22,776 -> 22,562.** The checker and the code
generator compute constants with one typed arithmetic (design.md, "Compile-time evaluation",
step 1). What went: the PipetteID and FactoriopediaID constructors (65 lines, behind constants
the code generator's folds compute now); the last unknown constant arguments in layouts
(`Array<Deque<MapGenerationRequest>, generationTaskStates.size()>` 28,
`Bitset<Limits<DamageTypeID::IndexType>::max() + 1>` and the other Bitset<?> 14, six other
Array<?> 6); InputAction::inputActionCount read at run time (6); the static_asserts the compiler
cannot evaluate, 39 -> 22 (floats, enumerators converted to integers, `sizeof` of a local array,
folds over packs). With it, bit fields with a default member initializer (`bool reserved : 1 =
false;`, AsteroidChunk): the parser read the width as the assignment `1 = false` (92 lines, and
the initializer was lost); with the width read right, the code generator wrote each bit field's
initializer over the whole storage unit it shares, and an aggregate's values went to the units
instead of the members, silent wrong values both, now stored into each bit field's own bits (spec
classes/bitfield_default_initializer). The lines that came in are code that gets further:
multiple inheritance in Surface's and Entity's layouts (+12), `Bitset<256>::reference` and
Pair's constructors (library gaps). One tree (the converter output of 9a5ce8d, with the glad
include), 9a5ce8d's compiler against this one.

### The configuration (2026-10-29)

The converter preprocesses the game as one real build of it: **FastDebug for Windows x64 with
Visual Studio 2022**, the configuration the clang comparison in prototype-results.md used. Its
defines come from `fastbuild/fbuild.bff`: the Windows `.defines` (WIN32_LEAN_AND_MEAN,
CRT_NONSTDC_NO_DEPRECATE, _CRT_SECURE_NO_WARNINGS, _SCL_SECURE_NO_WARNINGS, _WINDOWS,
_USE_MATH_DEFINES, USE_THREADS, WIN32, _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS, LUA_USE_LONGLONG,
FACTORIO_USE_D3D), the x64 WIN64, vs2022's `_WIN32_WINNT=0x0601`, FastDebug's own (_DEBUG, DEBUG,
USE_COMMON_HPP, LUA_USE_APICHECK, DISABLE_EXPRESSION_ID, SPACE_AGE) and the factorio project's
(NOMINMAX, BUILDING_WARNLESS_C, CURL_STATICLIB, SDL_MAIN_HANDLED, SDL_SENSOR_DISABLED); then what
MSVC predefines for x64 (_WIN32, _WIN64, _MSC_VER, _M_X64, _M_AMD64, `__cplusplus` as 199711L,
since the game does not pass /Zc:__cplusplus) and the Windows SDK names the game tests
(sdkddkver.h's versions, ENABLE_VIRTUAL_TERMINAL_PROCESSING, DISABLE_NEWLINE_AUTO_RETURN,
GUID_DEFINED). The table is `buildDefines` in convert.cpp.

Every other name is undefined, as in C, but a file's own `#define` and `#undef` lines count from
where they stand, and an `#include` of a converted header brings what that header leaves defined
(HAS_SSE2 from CpuIntrinsincs.hpp, DEBUG_NAVMESH_STATS, DEV_ONLY_DEBUG_TOOL and IMGUI_ENABLED from
the debug headers, CXXOPTS_HAS_OPTIONAL inside cxxopts.hpp). A header is preprocessed once, on its
own, so it does not see what its includer defined before including it (no game header depends on
that). An `#if` on a name that no build flag and no converted file defines, outside the list of
names undefined on purpose (`undefinedOnPurpose`: other platforms, compilers and build types, and
the debug switches the sources keep commented out), gets a TODO saying it was taken as undefined;
on this tree there is none. The front end lexes each file with the same branches
(`SourceConfiguration`, built once from all the inputs), so the two never disagree.

Before, a name outside two short lists was "unknown" and the converter kept the first branch of
its `#if`, so `#ifdef STEAM` counted as defined while `#ifndef STEAM` counted as undefined, and
`#if defined(NINTENDO_SWITCH2) && !defined(SPACE_AGE)` (GlobalContext::isSpaceAge) took the
Switch branch. The Steam integration is now out, as in every non-Steam build: SteamID.hpp's
`#else` stub replaces the class over CSteamID, and the Steam API needs no interop for this build
(a Steam build would need a port of SteamContext and SteamSocket over steam_api_flat.h).

FACTORIO_PACKAGE, which the converter defined since its first commit with no reason recorded, is
the packaged (shipping) builds' flag; FastDebug does not define it, so the conversion no longer
does. Without it the developer tools are in: DEV_ONLY_DEBUG_TOOL and IMGUI_ENABLED become defined
(DebugTools and its imgui windows, the debug renderers, DrawQueue's debug drawing), the entity
leak counter and the developer-only commands and options. Paired on the same tree, headline as
defined above: FastDebug with FACTORIO_PACKAGE 22,968, exact FastDebug 23,720; the 752 lines are
that developer code, much of it calls of imgui, a C++ library without an interop yet (design.md's
route: a thin C wrapper; cimgui is one). Against the old configuration (24,458 on b77b5ff): 958
lines fewer (the Steam code: CSteamID, HAuthTicket, CCallResult in 800 functions) and 220 more
(functions now reaching their next error, and the developer code). kovarex's standing rule is to
compile the whole game, so FACTORIO_PACKAGE stays undefined and imgui joins the C++ libraries to
wrap (after RE2 and DirectX, via cimgui).

### RE2 through a thin C wrapper (2026-10-29)

The first C++ library on design.md's route ("a thin C wrapper"): 17 game files use RE2, about 20
fixed patterns (FullMatch with string, StringPiece and integer targets, Replace) and
MacroReplacer's patterns built at run time. `programs/factorio/native/re2_c.cpp` is the C
interface: an RE2 object behind a pointer, a match with its submatches as offsets, Replace and
GlobalReplace, and RE2's own parsers of submatches into numbers (so FullMatch's parsing is RE2's,
not a copy). `native\build.cmd` compiles it with clang-cl as the game's Release builds are made
(/MT, /O2, against libraries\re2) into `native\build\factorio-native.lib`; a link of the converted
game passes it with `--lib`, next to `C:\Factorio\vs2022\Releasex64\re2.lib`. The Kpp side is the
RE2 class and re2::StringPiece (a StringView with RE2's ToString, CopyToString, AppendToString) in
`overrides/src/Util/RegexUtil.kpp`, appended to the converted RegexUtil; the game's RegexUtil
itself converts as written. Headline 22,558 -> 22,529 (paired on de36765): the 20 MacroReplacer
layout lines, the 8 "RE2 is not declared" and the StringPiece calls go.

### DirectX through generated COM classes (2026-10-29)

The DirectX 11 backend calls COM interfaces C++-style (`device->CreateBuffer(...)`) and keeps
them in ATL's CComPtr. Kpp's single-inheritance vtables are MSVC's since 8b63e33, so a COM object
the system hands out is called through a Kpp class whose virtual functions are the interface's
methods in vtable order. `programs/factorio/native/generate_com.py` reads that order and the C
signatures from the Windows SDK headers' C definitions (each interface's `IFooVtbl`), the bases
from their C++ ones, and writes:

- `overrides/added/src/Graphics/DX11/ComInterfaces.kpp`: 101 interfaces (all of d3dcommon, dxgi
  to dxgi1_5, d3d11, d3d11_1, d3d11sdklayers; IUnknown, ISequentialStream, IStream,
  IWICImagingFactory), 1,739 vtable slots, each class with only its own methods and no destructor.
- `native/include/kpp_directx.h`, the one import the converter writes for any of the DirectX, DXGI,
  WIC and COM headers: it marks those interfaces defined (`__IFoo_INTERFACE_DEFINED__`) before
  including the SDK headers, so the import keeps their types, functions and constants but not the
  interfaces' C structs, and then declares their IIDs. What else those guards hold it declares
  again after the SDK's headers: IUnknown's `typedef IUnknown *LPUNKNOWN;` (CoCreateInstance's
  parameter) and IStream's LPSTREAM, STATSTG and its enums (STREAM_SEEK_SET, ...). The headline
  command passes `--cinclude native\include` (relative to programs\factorio).

`overrides/added/` is new: hand-written files with no C++ counterpart, written into the output at
their path (CComPtr is `overrides/added/src/Graphics/DX11/ComPtr.kpp`, ATL's semantics). The
converter turns `__uuidof(IFoo)` into `&IID_IFoo` (C's REFIID is a pointer). Checked on WARP:
D3D11CreateDevice, CreateBuffer and GetDesc through the classes, QueryInterface for IDXGIDevice,
GetAdapter, GetDesc, and the Release counts (link d3d11.lib, dxgi.lib, dxguid.lib from the SDK).

Headline 22,513 -> 22,405 (paired on 9e06cde): the CComPtr, DXGI_SWAP_EFFECT,
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN and IStream lines go. What the backend reaches next is the
Windows SDK's macros, which the import does not make yet (the codegen thread's next importer
item): FAILED and SUCCEEDED (about 28 functions), ZeroMemory, LoadLibrary and _T, the HRESULT
constants (E_INVALIDARG, E_FAIL, DXGI_ERROR_*, whose bodies call _HRESULT_TYPEDEF_), plus
D3D11CalcSubresource (an inline function d3d11.h has only for C++), SHCreateMemStream (shlwapi.h)
and a GUID written with brace elision (language/brace_elision_in_aggregate, pending).

### The first link (2026-10-29, morning)

`programs/factorio/link.cmd` gets to the linker for the first time: all 64 modules compile in clang
(no LLVM rejects, no crash), and link.exe ends with LNK1120, **7,372 unresolved externals**:

- 7,346 game functions, the ones codegen errors leave out (the headline);
- 17 SSE intrinsics the code generator does not implement yet (_mm_cvtss_f32, _mm_set_ss,
  _mm_unpacklo_epi8/epi16, _mm_packus_epi16, ...): intrin.h's are now imported as typed functions,
  so a missing one is an unresolved symbol, not an untyped call; the game uses about 70 more of
  them in functions that still have other errors;
- MSVC's __cpuid and _BitScanReverse, ws2tcpip.h's inline gai_strerrorW, the overrides' kppStbirMalloc
  and kppStbirFree (native's stb_image_resize calls them), and stb_image_write's
  `stbi_write_png_compression_level` variable. (version.lib, which SDL's IME code needs, is now on
  the link line.)

What it took, from 64 rejected modules (each module's first error): the scan that declares the
names a module uses without defining them read the `@` inside quoted names and strings (a local
lambda `f::__lambda_1_2@10834` became `declare void @10834()`, and two string constants became the
"functions" `factorio.com` and `serialized`); an `auto` function template's signature was typed from
a copy of its instantiation's copy, so the lambda it returns was another closure type than its
body's (`%closure.X.2`, never laid out); a closure whose layout failed half way was used half built
(a call with no callee); closures nobody could build (their function failed first) get a placeholder
type under --keep-going; `return s.append(x)` with String::append returning void (cxxopts) is now an
error, not `ret ptr` with no value; intrin.h's `typedef float __m128 __attribute__((__vector_size__(16)))`
made `__m128` a float (vector typedefs are no longer imported, so __m128 & co. stay the compiler's
vector types), `__inline__` stopped the import of every static inline function, and the SSE
operations returned slot addresses that locals then stored as vectors (vector values are registers
now, interop/sse_vector_values); a lambda emitted between a `goto` and its label forgot the label
(language/goto_past_lambda); value-initializing a 1 MB array member (`store [1048576 x i8]
zeroinitializer`) crashed instruction selection (a memset now); and the entry is `wmain`.

### The link line (2026-10-29)

`programs/factorio/link.cmd` links the converted tree: kppc `--keep-going --exe` against the game's
own Release builds of its libraries (C:\Factorio\vs2022\Releasex64), `native\build\factorio-native.lib`
(built first by native\build.cmd), and the Windows libraries of the game's link line
(fbuild.bff's .extraExecLibs: dnsapi, wldap32, gdi32, comdlg32, ole32, winmm, psapi, opengl32,
gdiplus, shell32, shlwapi, user32, imm32, oleaut32, setupapi) plus dxguid, uuid and windowscodecs
for the COM interfaces' IIDs, which C++ compiled in through __uuidof. The native library now also
holds the header-only C libraries the game implements inside its .cpp files, with the
configuration those files give them: stb_image_resize with BitmapUtil's allocator, sdf,
stb_rect_pack (header_only.c) and jc_voronoi in double precision with the game's deterministic
Math::atan2 (jc_voronoi.cpp: the game's copy of the header calls std::floor). The game functions
they call are exported to C by overrides (kppStbirMalloc, kppStbirFree in Graphics/BitmapUtil,
kppMathAtan2 in Logistics/Triangulation), and the import of jc_voronoi.h goes through
native/include/kpp_jc_voronoi.h, which gives it the same double precision. The converter also
leaves out the two files the Windows build does not compile (fbuild.bff's
.factorioFilesForOtherPlatforms: Graphics/PngLoad.cpp, which is what needed libpng, and
Util/StacktraceToStream.cpp), and an #error the configuration reaches is a TODO, no longer only
removed.

On master 8179bf6 with these, every one of the 64 modules still fails in clang (IR codegen
rejects), so the link itself lists nothing yet. Counted over the modules instead: of the 552 C
functions declared and defined in no module, 43 are in none of the link line's libraries either
(llvm-nm over each): 16 libpng functions (PngLoad, now left out), the LLVM intrinsics codegen
names as functions (llvm.memcpy, llvm.sqrt, llvm.va_*, 14 llvm.x86.sse*), MSVC's
_BitScanReverse, __cpuid and _mm_prefetch, ws2tcpip.h's inline gai_strerrorW, and two names
that are no functions (`factorio.com`, `serialized`). Headline 22,321 -> 22,318.

### What the link misses: the second number (2026-10-29)

The headline counts the code generator's error lines; a function the game needs can also be
missing from the link with no error line at all. So link.cmd's unresolved externals are tracked
too, split by `programs/factorio/externals.py`, which link.cmd runs after the link:

- **left out, codegen failed**: a function with an error in link-errors.txt, or one left out
  because a function it asked for failed (a lambda in it, `requested by`). These follow the
  headline, split by the owner of the error.
- **never emitted, no error**: declared, used and never defined, and nothing says why. The
  headline does not see these. Report this count next to the headline.
- **C names**: a function of a C library the link line lacks, or an inline function of a C header,
  which the import declares and does not define.

On master cf27481 with the Logitech bridge and iphlpapi (below): headline 262, LNK1120 492 = 223
left out + 266 never emitted with no error + 3 C names (_alloca, which codegen should lower;
gai_strerrorW and IsWindows8OrGreater, inline functions of ws2tcpip.h and versionhelpers.h). The
266, by cause, each a pending spec:

- about 200: out-of-line definitions of class templates' members whose return type names the
  class's member type through the class, `typename ID<T, I>::IndexType ID<T, I>::loadIndex(...)`
  for `static IndexType loadIndex(...)`: the definition is not attached (ID::loadIndex,
  Prototype::getLoader, SimpleSlot::getCount, ComplexExpression's compile*Arguments,
  FlowDataFrame, LazyCreatedStatistics, BezierInterpolation;
  templates/out_of_line_member_dependent_return_type);
- about 44: out-of-line member templates of class templates, `template<class S> template<class F>
  void Frame<S>::merged(F) const` (FlowDataFrame::getMergedValues, ImmutableStringTemplate's
  operators; templates/out_of_line_member_template_of_class_template);
- 4: out-of-line members of partial specializations (FallbackValueHolder;
  templates/out_of_line_member_of_partial_specialization);
- 2: MainLoop's `Atomic<HeavyMode> globalHeavyMode(HeavyMode::Disabled);`, read as a function
  declaration (language/global_with_parenthesized_initializer);
- the rest: function templates and two lambdas that templates' instances use and codegen never
  instantiates (countsAsButtons, getSortedCounts; not reduced yet).

On master 251e4e2 (later the same day): LNK1120 55 = 53 left out + 2 never emitted + 0 C names.
Every one of the 53 has its root cause named, almost all in a pending spec that says its owner
(25 the resolver's, 19 codegen's, 9 the templates thread's); the never emitted are a CRTP base's
static member taken through the derived class (computeConstant) and an MplVector::forEach of a
lambda. To redo the split: link.cmd, `externals.py kpp\factorio.exe.link.txt kpp\link-errors.txt
GROUP` for a group's symbols, each one's function's first error in link-errors.txt, and
`kppc --why-at "File.kpp:LINE,..."` (quoted: cmd splits arguments at commas) on the lines whose
error names no cause by itself.

The link also takes the Logitech LED SDK now (fbuild.bff links `libraries/Logitech/x64/
LogitechLEDLib.lib`): its functions are C++, so `native/logitech_led_c.cpp` gives the six the game
calls C names and `overrides/src/LED/Logitech/Includes/LogitechLEDLib.kpp` defines the header's
functions on them; and iphlpapi for UDPSocket's GetIpAddrTable.

### The first run (2026-10-29, afternoon)

`programs/factorio/run.cmd [game arguments]` builds the converted tree as link.cmd does and runs
it: `run.cmd --version`, `run.cmd --dump-data`, or no arguments for a normal start.

- **Where it runs.** factorio.exe goes to `C:\factorio-in-kpp\bin\x64\`, where the C++ build puts its
  own relative to the data, so the game's path discovery works unchanged and reads
  `C:\factorio-in-kpp\data`. run.cmd writes `C:\factorio-in-kpp\config-path.cfg`
  (`use-system-read-write-data-directories=false`) and `run\config\config.ini` when they are
  missing, so config, log and saves go to `C:\factorio-in-kpp\run\`: never `%APPDATA%\Factorio`,
  never C:\Factorio. bin\, run\ and config-path.cfg are not part of that repository.
- **The reference** is the C++ FastDebug build's log, `C:\Factorio\factorio-current.log`; the
  converted game's is `C:\factorio-in-kpp\run\factorio-current.log`.
- **Stubs, a bring-up tool.** run.cmd compiles with `--keep-going --stub-missing`. Every function
  the code generator left out (it failed, or it is used and nothing defines it) becomes a stub that
  prints its qualified name and source line and aborts, so the game links and runs up to the first
  one it reaches. The flag is opt-in, only works with --keep-going, and is never a default; the
  compile still exits 1 and its errors stay in `kpp\run\build-errors.txt`; run.cmd prints the
  stub count and the error count at the top of each run. Nothing ships with it.
- **Inline C functions.** The import declares a C header's static inline functions but kppc
  compiles no C bodies, so `native/inline_c.c` takes the addresses of the ones the game calls
  (ws2tcpip.h's gai_strerrorW, versionhelpers.h's IsWindows8OrGreater) and clang emits them into
  factorio-native.lib with the headers' own bodies.

What the first runs found, each with its spec:

1. The link failed on one stub-free name, `ScriptRenderBase::operator bool() const`: a pure virtual
   conversion operator called directly, not through the vtable (an impure one would have run the
   base's body). Conversion operators dispatch like other virtual functions now
   (classes/virtual_conversion_operator). With that, on master b7d0ee8 plus this work, the game
   links with 192 stubs and 220 error lines.
2. `run.cmd --version` stopped in static initialization:
   `InputActionList.kpp:130: assertion failed: !InputActionList::allActionsSortedByAction.empty()`.
   The startup functions of globals ran in the order the code generator happened to reach them
   (per module, by name hash), not in definition order; editableActionsSortedByAction is built by
   a function that reads allActionsSortedByAction, defined just before it. They are now one
   llvm.global_ctors list in module 0, in definition order within a file, the files in the
   driver's order with the library first (design.md section 2; language/startup_in_definition_order).
3. Then the stub of `TimeStatisticsBuffer::generateProperties` (TimeStatistics.kpp:82), which
   `properties = generateProperties()` calls at startup: its definitions are explicit member
   specializations written through an alias of the instance, `template<> Vector<...>
   UpdateBuffer::generateProperties()`, which master's ef6f81c now attaches.
4. On master 0d842b2 (headline 162 with these, 143 stubs) the link missed WMI's
   CLSID_WbemLocator and IID_IWbemLocator and winerror.h's inline HRESULT_FROM_WIN32: run.cmd had
   its own copy of link.cmd's libraries (now both call `link-settings.cmd`), and native/inline_c.c
   emits HRESULT_FROM_WIN32 too.
5. Then the stub of StdoutWriteStream's constructor, built at startup for
   `StdoutWriteStream::instance`: `: FileWriteStream(stdout)` found no constructor, because the
   library's stdout was a `void*` and FileWriteStream takes a `FILE*`. stdin, stdout and stderr are
   `FILE*` now, FILE being the UCRT's own struct (interop/stdout_is_file_pointer).

Found on the way, from the coordinator's list of layout asserts: Decorative's bit fields (`uint8_t`
and `bool` ones in one byte, MSVC's rule: one storage unit while the types have one size;
classes/bitfields_of_one_size_share_a_unit), and a silent layout bug of the C import: MS C's
anonymous member of a named type, `MONITORINFO;` inside MONITORINFOEXW, was read as a macro
statement and dropped, so the struct had 64 bytes instead of 104 and GetMonitorInfo would have
written past it (interop/c_anonymous_member_of_named_type). Inventory's `sizeof(Inventory) <= 40`
fails for another reason: the library's UniqueArray holds its count beside the pointer (16 bytes
where std::unique_ptr<T[]> has 8), which makes Inventory 48 (the converter thread's b0f3cb4 makes
UniqueArray one pointer).

6. **`factorio --version` runs** (master 42199d3, 122 stubs): "Version: 2.1.0 (build 85837, win64,
   full)", the binary and map versions, exit 0, as Main.cpp prints them.
7. `--dump-data` starts up into headless mode (system and memory info through WMI, "Audio is
   disabled") and crashes. The log said "Operating system: Windows 8 <running in compatibility
   mode>": the executable had no manifest. native/factorio.rc now gives it the game's manifest
   (fastbuild/win/Factorio.manifest) and icons as a resource (fbuild.bff's /MANIFEST:EMBED needs
   mt.exe, which the link does not see), and the log says Windows 10.
8. The game's crash handler then hung ("Generating symbolized stacktrace, please wait ..."). For
   bring-up, run.cmd links with `/DEBUG /MAP` (a PDB of the functions' names: the objects carry no
   debug information) and native/crash_report.c, an unhandled-exception filter that prints the
   exception and the stack named from that PDB and ends the process; the game runs with
   `--no-crash-handler`. `run.cmd --no-build ...` runs the last build again.
9. What it showed: the crash handler itself died in `RtlCaptureContext`, because the import
   ignored winnt.h's `__declspec(align(16))` and StackWalker's local CONTEXT was aligned to 8
   (`__declspec(align(N))` is the struct's alignment now; interop/c_declspec_align). Underneath:
   `ModManager::addHistoryForFont`'s `prototypeHistory[FontBank::type][name]` calls
   PrototypesHistoryTemplate's const lookup `operator[](StringView) const`, which aborts with "No
   history for the given key: font", instead of the inserting `T& operator[](const String&)` that
   C++ chooses for the non-const object (the arguments rank the same, the implicit object parameter
   decides; language/non_const_member_overload_for_non_const_object, pending, for the resolver).
   The next stop of `--dump-data`.

### dear imgui through a generated C interface (2026-10-29)

The game's debug tools (DebugTools.cpp, ImguiSDL.cpp, DebugInfoRenderer.cpp; 891 `ImGui::`
uses) are built on its copy of imgui 1.53, which is C++: namespace ImGui with overloads and
default arguments, structs with methods, the ImVector template, and structs imgui owns whose
fields the game reads and writes (ImGuiIO, ImGuiStyle, the draw lists). In the manner of cimgui,
`programs/factorio/native/generate_imgui.py` reads imgui.h and writes both sides of a thin C
interface:

- `native/imgui_c.cpp`: an extern "C" function for each of the 320 functions of namespace ImGui
  and for the 127 methods, constructors and destructors of the 17 structs Kpp mirrors. Classes
  cross by pointer (an ImVec2 argument, a reference, an ImVec2 result through an out pointer); a
  variadic function (Text, TreeNode, ...) formats through imgui's va_list variant (LogText, which
  has none, formats first and passes "%s"). It also asserts the layouts the script computes: every
  mirrored struct's size and each field's offset, against imgui.h as clang compiles it.
- `overrides/added/src/Debug/ImGui.kpp`: imgui's types with imgui's names: the typedefs, enums and
  object-like macros (as constants), the structs with their fields in imgui's order, their methods
  and namespace ImGui with imgui's signatures and default arguments, each calling its C function
  (a variadic one is a template forwarding its pack to the C one); ImVec2, ImVec4, ImColor and
  ImVector are imgui's inline code written in Kpp, and imgui_internal.h's ImVec2 operators are
  there too. static_asserts check the sizes. Only declared, as only pointers to them cross:
  ImDrawListSharedData, ImGuiContext, ImGuiStorage, ImGuiTextFilter, ImGuiTextBuffer and
  ImGuiOnceUponAFrame. The va_list variants are left out (Kpp has no va_list).

The converter needs nothing beyond `imgui.h` in `overrides/expand-macros.txt`, for IM_COL32 at its
four uses. `native\build.cmd` builds the shim into factorio-native.lib, which link.cmd already
links with the game's imgui.lib. The check is convert.cmd's fourth smoke test:
`tests/imgui_smoke.kpp` runs three headless frames (font atlas, a window with text, widgets,
columns, a tree and draw-list primitives) and prints the draw data, style and io counters, and
`tests/imgui_smoke.cpp`, the same frames written in C++ against imgui.h, printed
`tests/imgui_smoke.expected`: the outputs are equal, down to the vertex and index checksums. On the
way, a char array initialised by a string literal stays empty (language/char_array_from_string_literal,
pending).

### glm through a port (2026-10-29)

glm is C++ header-only templates, so the part of it the game uses (vec2, vec3, vec4, mat4, the
component functions, translate, scale, rotate, ortho, inverse, value_ptr; 11 game files) is ported
to Kpp instead of wrapped: `overrides/src/API/glm.kpp`, appended to the conversion of the game's
`API/glm.hpp`, specified in [glm.md](glm.md). Its rule is glm's results to the bit (the same
operations in the same order, and the C library's float functions for floats); convert.cmd's fifth
smoke test, `tests/glm_smoke.kpp`, prints vector, noise and matrix results in hex, equal to
`tests/glm_smoke.cpp` compiled against glm itself.

### Lua's internals through a C bridge (2026-10-29)

The game's Lua is Lua 5.2 with Wube's changes (tables that keep their insertion order, a userdata's
pointer to the game object it binds, `factorioBindableObject`), compiled as C++, and 12 of the
game's files reach past its API into its internals: LuaBinder, LuaBindableObject, LuaValueSaver,
LuaValueLoader, LuaTableIterator, LuaRef, LuaHelper and others use `index2addr`, TValue, Table,
Node and Udata and lobject.h's macros (hvalue, uvalue, ttisnil, setobj2s, gkey, gval, ...). Lua stays
C for now (kpp\Lua is the game's C++ around it), so the internals are imported as C too:

- `programs/factorio/native/include/kpp_lua_internals.h` imports lobject.h, lstate.h, ltable.h,
  lgc.h and ltm.h. lobject.h's one C++ word, `class LuaBindableObject*` in Udata, is read as a
  struct pointer (the C import's struct is the game's class), and Node, which names itself without
  `struct`, gets its typedef first. It declares index2addr, which lua.lib has with C linkage. The
  converter imports it next to the API's headers for any of Lua's internal headers a file includes.
- Their function-like macros are expanded at their uses only in the files that include one of
  those headers, directly or through a converted header, as only those see them in C++
  (`included` lines in overrides/expand-macros.txt). An object-like macro that names a function-like
  one (`#define setobj2s setobj`) expands as that one.
- What lua.lib has under C++'s names (in the game's C++ build of Lua, LUAI_FUNC is a plain
  `extern`): luaH_getint gets a C function of its name that calls the C++ one through clang's asm
  label (`native/lua_internals.c`), luaT_typenames_ a linker alias there.

convert.cmd's sixth smoke test checks the bridge: `tests/lua_internals_smoke.kpp` prints the layout
(TValue 16 bytes, Udata 56 with factorioBindableObject at 40, Table 88, Node 56, lua_State 232) and
reads a real Lua state through it (a table's array part, its nodes in insertion order, luaH_getint,
a userdata's bound object), and its output equals `tests/lua_internals_smoke.cpp`'s, the same
against the game's Lua headers compiled as C++.

All of this goes when Lua itself is converted: the game's code then uses Lua's types and macros as
Kpp code, and kpp_lua_internals.h, lua_internals.c and the `included` lines are deleted.

### Converting Lua: the plan (2026-10-29, not started)

**What.** `libraries/Lua`: 59 files, 21,449 lines (lua.c and luac.c, the standalone interpreter
and compiler, left out as the game's build leaves them out). fbuild.bff compiles it as C++ (`/TP`)
with LUA_COMPAT_ALL and `override_printf.h` force-included, which sends the printf family to trio
so that numbers format the same on every platform.

**The game's changes** (C:\Factorio's history: 136 commits since the import of Lua 5.2.1 in 2012,
+2,902/-855 lines in 47 files):
- *Determinism.* lmathlib.c computes the math functions itself instead of calling libc (+738
  lines, for the same results on every platform; atan2 of NaN), float to int conversions without
  undefined behaviour, the bit library's tricks replaced, string.format of 64-bit integers, printf
  through trio.
- *Tables.* Iteration in insertion order (ltable.c), a flag field for recursion detection, integer
  keys looked up without a rehash, table constructors and SETLIST that size the array part right,
  large sequential tables saved and loaded correctly; userdata and tables point at the game object
  they bind (LuaBindableObject) instead of a `__self` field.
- *Mods kept in their sandbox.* load() takes no binary chunks and lundump.c checks bytecode
  (+298); the VM rejects manufactured SETLIST and FORLOOP uses; the debug library cannot mutate
  (debug.setupvalue not on C functions), debug.getinfo has an option 'p'; external libraries
  cannot be loaded and the unused io, os and coroutine parts are not exported.
- *Additions.* string.pack, packsize and unpack from Lua 5.4.6 (lstrlib.c, +473); API in lapi.c
  (+280) for the binder and its error messages ("bad argument #4 of 4", stack traces that stop at
  pCallWithStackTrace's frame); the collector stays blocked during __gc calls; 5.2.3's ephemeron fix.

**How.**
1. The converter reads `.cpp`/`.hpp`: it takes Lua's `.c` files as the C++ they are compiled as,
   with the library's define and force-included header, into `kpp\Lua\` beside LuaCPPUtilities.kpp
   (convert.cmd and the headline command already build kpp\Lua).
2. Converter work, from the trial below: macros that are member lists (`CommonHeader`,
   `TValuefields`, `ClosureHeader`) expanded in class bodies; macros that name types (`LUA_NUMBER`
   behind lua_Number, `l_noret`); string macros joined to literals (`LUA_QS`).
3. Until the converted Lua compiles almost clean on its own, the game keeps the import. Then the
   switch: `#include <Lua/...>` stops being an import (the converter's Lua rules go), the game's
   code calls the converted lua_*, luaH_* and TValue directly, and lua_internals.c,
   kpp_lua_internals.h, the `included` lines of expand-macros.txt and lua.lib in link.cmd are
   deleted. The Lua smoke test stays as a check of the converted layout against the C++ build's.
4. Lua's own formatting keeps trio (trio.lib stays linked): the converter's trio-to-CRT renames are
   for the game's code, and Lua's tostring and string.format must format as the C++ build does
   (saves, and multiplayer between the two builds).
5. Checks: a Lua script run by both builds with the same output (number formatting, pairs order,
   the math functions to the bit, string.pack), then the game's own Lua tests.

**What it costs, measured.** The trial (Lua's `.c` copied as `.cpp`, converted with today's
converter, built alone the headline's way): 564 error lines, with parse errors in 15 files. The
largest causes are the converter's: the member-list macros (about 200 lines: lua_State 115,
lua_TValue 17, the other GC objects) and the type macros (about 150: SemInfo's `lua_Number r` 81,
`nval` 35, conversions of what they leave unknown). So the switch in step 3 would add about 560 to
the headline today; after step 2, an estimated 100 to 150, compiler gaps in C-style code (unions
of GC objects, casts between them, exceptions as Lua's error mechanism). The game's side should not
change: it compiles against the import now.

### What the parse of the C++ does not see (2026-10-29)

The front end parses the game's C++ to convert it, with the #if branches the configuration drops
blanked. Since 2026-10-29 it also blanks, for the parse only, what is no C++ to it and what the
conversion writes differently (convert.cpp, blankForParse), and the converter writes each one's
replacement where it stood: the empty macros the converter drops (BEGIN_SUPPRESS_CONVERSIONS
before a definition, END_FORCE_OPTIMIZE, FAR in `(VOID FAR* FAR*)&buffer`, FORMAT_ATTR(4, 5)),
FORMAT_STRING( and its `)` around a parameter, and MSVC's structured exception handling. That took
the parse errors in the converted C++ from 20 to 4; those four are X-macro lists the parse cannot
read where they stand (a member initializer list built by CALL_ON_EVERY_STYLE_TYPE, an expression
built by CALL_ON_EVERY_INPUT_ACTION_TYPE_WITH_DATA, two by CALL_ON_EVERY_ID_TYPE), whose
expansions the converter writes all the same.

### Structured exception handling (2026-10-29)

Kpp has exceptions (design.md 9: try/catch on Windows SEH underneath) but no SEH filters, so
`__try { A } __except (filter) { B }` becomes `try { A } catch (...) { B }` with the filter dropped
(a todo per use). A `catch (...)` catches Kpp exceptions only (the try's filter looks for Kpp's
exception code), not a crash in a C function, so the game's four uses get no protection from it:

- SystemUtil's thread naming raises the debugger's MS_VC_EXCEPTION only on Windows before 10 and
  only with a debugger attached, which takes the exception first: the dropped filter changes
  nothing there.
- The two calls into the D3D driver (createDeviceAndSwapChainExpectingCrash, dxgiSwapChainPresent
  in GraphicsInterfaceDX11) caught a driver crash to restart with the blit presentation model or
  ask the player to update the driver. A crashing driver now reaches the game's crash handler,
  which writes a minidump and shows the plain crash message.
- CrashHandler::createMinidumpAndTerminate threw to itself to get the exception pointers for its
  minidump: overrides/src/Util/CrashHandler.kpp writes the dump without an exception record and
  terminates as before.

### Standard input (2026-10-29)

Kpp has no iostreams (design.md 4), and the game reads std::cin in one place: the Windows path of
InterruptibleStdioStream::read, the dedicated server's console, `std::getline(std::cin, result)`.
`overrides/src/Util/Streams/InterruptibleStdioStream.kpp` reads the line with the C library's
fgets on stdin instead, piece by piece up to its line break (left out, as getline leaves it); a
partial line at the end of the input counts, an empty one does not, as with getline. unblock()
still ends a pending read by putting an Enter key press into the console's input.

### GDI+ through a port of its classes (2026-10-29)

The game uses GDI+ in two places on Windows: GDIPlus.cpp saves screenshots (startup, the image
encoders, a Bitmap filled through LockBits and saved to a stream with an encoder's parameters) and
GDIPLoaderData.cpp loads a sprite (Bitmap::FromStream, LockBits) when the build does not use WIC.
gdiplus.h is C++: classes whose inline methods call GDI+'s flat interface, the C functions
gdiplus.dll exports. So, as glm, the part the game uses is ported:
`overrides/added/src/Graphics/GdiplusClasses.kpp` has gdiplus.h's Status, Rect, GdiplusStartupInput,
the pixel formats, BitmapData, ImageCodecInfo, the encoder parameters, EncoderQuality, Image and
Bitmap with their names, values and behaviour (an Image owns its GDI+ image and keeps the last
status that was not Ok), each method calling its flat function, declared there `extern "C"`
(gdiplus.lib is on link.cmd's line). The game includes GdiPlus.h inside its own namespace GDIPlus,
so the classes are GDIPlus::Gdiplus, where the game names them; the include itself is dropped as
any other. (The file is not Gdiplus.kpp: the conversion of GDIPlus.cpp is GDIPlus.kpp in the same
directory, the same name to Windows.)

The game's IStream implementation (WindowsWriteStream, which GDI+ writes the encoded image to)
compares the interface id it is asked for with `iid == __uuidof(IUnknown)`. C++ compares the ids'
values there (guiddef.h's operator==); in Kpp a REFIID is a pointer, as in C and the COM classes,
so the converter writes `(memcmp(iid, &IID_IUnknown, sizeof(IID)) == 0)`, C's IsEqualIID, for an
id compared with `__uuidof`, and REFIID itself as `const IID*`.

convert.cmd's seventh smoke test checks the port: `tests/gdiplus_smoke.kpp` prints the structs'
layouts, starts GDI+, lists the encoders, writes a 3x2 bitmap through LockBits, saves it as PNG and
JPEG (with EncoderQuality) into memory streams, reads both back (the PNG's pixels), and loads a
stream that holds no image; its output equals `tests/gdiplus_smoke.cpp`'s, the same steps against
the SDK's gdiplus.h.

### StackWalker (2026-10-29)

The crash handler writes its stack traces with Jochen Kalmbach's StackWalker (BSD licence; the
game's copy in `libraries/StackWalker`, with Wube's ShowCallstacks, OnEntrySymbolized and
ShouldLogEntry), which the game's StackWalkerToStream derives from. It is C++ over DbgHelp and
ToolHelp, which it loads with LoadLibrary and GetProcAddress, so it is converted with the game, as
cxxopts is: convert.cmd, link.cmd and the headline command take `libraries/StackWalker` (into
`kpp/StackWalker`), and link.cmd no longer links the game's stack-walker.lib. What its conversion
needed:

- MSVC's C++ forms of the secure CRT functions, which take the destination as an array and its size
  from it (`strcat_s(szTemp, ".local")` through tchar.h's _tcscat_s, `_snprintf_s(buffer, count,
  format, ...)`): `overrides/added/StackWalker/MsvcSecureCrt.kpp` has them as templates over the C
  forms the import declares.
- The narrow names of the Windows functions it calls by their generic ones (GetModuleFileName,
  GetFileAttributes, GetEnvironmentVariable) and tchar.h's _tcscat_s, in the converter's table.
- A `#define` among a class's members (`#define MAX_MODULE_NAME32 255` in StackWalkerInternal)
  becomes a `static constexpr` member of the class, where it was a data member of each object.

Its last line went with 1806a0d: USED_CONTEXT_FLAGS is CONTEXT_FULL, which the import writes
before the constants it is made of (language/auto_global_from_later_global). `(HANDLE) -1` is
written `(HANDLE)(-1)` by the converter (a subtraction from HANDLE to Kpp, which codegen turned into
IR clang rejected, losing a whole module at link time; language/c_style_cast_of_negative_number).

### The libraries' C++ around C, and more of Windows (2026-10-29)

What the game takes from C++ headers around C libraries and Windows, each given to Kpp the way
the headers' C would be:

- **WIC and WMI.** The sprite loader decodes with WIC (IWICStream, IWICBitmapDecoder,
  IWICBitmapFrameDecode, IWICFormatConverter over IWICBitmapSource) and SystemUtil reads the RAM
  modules through WMI (IWbemLocator, IWbemServices, IEnumWbemClassObject, IWbemClassObject):
  generate_com.py generates them with the DirectX interfaces, kpp_directx.h imports wbemidl.h,
  the converter imports it for `<Wbemidl.h>`, and link.cmd takes wbemuuid for their ids.
  comdef.h's `_bstr_t` (and its `bstr_t`), which the WMI query passes, is
  `overrides/added/src/Util/ComBstr.kpp`: a BSTR the object owns, from a wide or a narrow string.
- **crc32fast** (`libraries/crc32fast`) is C++, so its crc32_fast, which Util/Crc calls, has a C
  name in `native/crc32fast_c.cpp`, and `overrides/src/Util/Crc.kpp` defines crc32_fast on it.
- **open-vcdiff** (`libraries/open-vcdiff`, the updater's patches) is C++ as well:
  `native/open_vcdiff_c.cpp` decodes through its VCDiffDecoder into a block a C function returns,
  and `overrides/added/src/OpenVcdiff.kpp` declares the game's `open_vcdiff::VCDiffDecoder` with
  the one member Updater::processDiff calls, Decode, on it.
- **stb_dxt's internals.** DXTCompression.hpp includes stb_dxt's implementation as static functions
  and calls them from its own SSE compressor (stb__InitDXT, stb__MatchColorsBlock, the stb__OMatch5
  and stb__OMatch6 tables, ...). native/header_only.c builds stb_dxt and gives those it calls C
  names (kpp_stb__*, the tables through pointers to them), `native/include/kpp_stb_dxt.h` declares
  them, and the converter imports that header for `<stb_image/stb_dxt.h>` and writes the names.
- **The UCRT's handlers.** CrashHandler installs set_terminate and _set_new_handler, which eh.h
  and new.h (C++ headers) declare and ucrtbase exports under C's names:
  `overrides/src/Util/CrashHandler.kpp` declares them. They reach what runs on the C runtime only:
  Kpp's own uncaught exceptions print and abort (design.md 9) and its new does not call the new
  handler, so for Kpp code neither runs.
- **Smaller ones**, in the converter's tables: d3d11.h's C++-only inline D3D11CalcSubresource
  (`mip + slice * mipLevels`), luaconf.h's LUA_NUMBER_FMT (a string the import makes no constant
  of), WriteConsoleInput as WriteConsoleInputA. The game's class names the converter renames (Map,
  Vector, Optional, Pair, RuntimeError) are no longer renamed where they are members reached through
  `.` or methods called through `->`: ID3D11DeviceContext's Map was being called as GameMap.
- **std::get of a template's type** on a variant (TextDrawSection's `std::get<IDType>(this->extraData)`
  in followingTextForID<IDType>): no alternative is spelled IDType, so each union the converter
  writes has generic `get<T>()`, `holds<T>()` and `getIf<T>()`, which find the alternative of type T
  through `alternatives(U)` when the instance is made, and the converter writes the use with them.

### From emitted functions to an executable (2026-10-28, night)

The first run of the full src past IR (`kppc --keep-going --exe`, e7d4669; timings in
compile-speed.md), against the game's own Release builds of its C libraries. What stands between
"every function emitted" and an executable, beyond the headline's codegen errors:

- **255 functions emitted with IR that LLVM rejects** (codegen bugs the headline does not count;
  46 of the 64 modules failed until they were taken out, one at a time, by a script):
  - 164 a pointer where a value is expected or the reverse, mostly a lambda returned by value
    (`ret %closure.N %o3`: the closure's address returned as if it were the closure), and class
    values handed on as addresses;
  - 15 SSE vector values (`<4 x float>`, `<4 x i32>`, `<2 x double>`) used as pointers
    (float32x4 and its kin);
  - 30 unsized types allocated or loaded (a class whose layout was never written);
  - 12 two functions with one IR name: overloads whose parameter types print the same
    (`NamedBool<?>` for two different unresolved arguments; two classes of one simple name, since
    types print by their last name only);
  - 17 calls with the wrong number of arguments, `void` values, an invalid return type;
  - 5 functions that crash LLVM's instruction selection (BufferedReadStream's constructor and
    four more).
- **10,915 unresolved symbols at the link:**
  - 9,650 functions a codegen error left out (the headline);
  - 1,079 Kpp functions declared and called but never defined anywhere codegen saw: the
    InputAction family (314), GuiStyle's style getters (205), EditorAction (136), Filesystem (49,
    stdlib declarations without definitions), PrototypeInfo and PrototypeMigrationList (42 each),
    ID (42), Serialiser (28), and the methods of classes declared inside function bodies (the code
    generator does not look inside bodies for them; spec classes/local_class);
  - 114 of the functions taken out for invalid IR;
  - 72 C functions: Windows libraries the driver does not link yet (gdi32, imm32, version,
    setupapi/cfgmgr32, opengl32, oleaut32, winmm, wldap32 for curl's LDAP), two compiler
    intrinsics imported as functions (`_BitScanReverse`, `_mm_prefetch`), an inline function of
    ws2tcpip.h (`gai_strerrorW`), and libpng and stb_image_resize, which none of the linked
    libraries contains.
- **No entry point:** the game's entry is `wmain` (UTF-16 arguments), which codegen does not
  treat as the program's entry the way it treats `main`, and `fmain`, which it calls, is one of
  the functions with errors.
- **One duplicate definition**: `Curl_HMAC_MD5`, defined both by curl.lib and by the imported
  curl header (a C header's data definition emitted as a definition, not a declaration).

**The functions defined nowhere (2026-10-29).** Counted over the 64 emitted modules (`kppc
--keep-going --exe`; with most modules rejected by clang the link lists nothing, so a script
compares the modules' `declare`s with their `define`s and leaves out the functions a codegen error
names): on master d1ccfb8, 1,345 Kpp functions are called and defined nowhere. Most of them were
X-macro expansions: InputAction's 316 constructors, GuiStyle's 312 style getters, EditorAction's
136, PrototypeMigrationList's and PrototypeInfo's, Serialiser's counters. Their .cpp writes
`CALL_ON_EVERY_INPUT_ACTION_DATA_TYPE(CREATE_INPUT_ACTION_CONSTRUCTOR)`, but the list macro is
defined in the header, which the converter handled as a file of its own, so the use stayed
unexpanded text. The converter now gives every file the function-like macros of the game headers
it includes, directly or not, that those leave defined (except the ones it rewrites by name), with
`__FILE__`/`__LINE__` in an expansion taken at the use and `std:: string` (an X-macro's `PREFIX
TYPE`) renamed like `std::string`. Local macro expansions go from 2,701 to 7,149. Together with
the stdlib's makeOptional<T>(args...) and the array form of make_unique_for_overwrite: defined
nowhere 1,345 -> 530, unresolved at the link 9,052 -> 8,570, headline 23,422 -> 23,252. Of the 530
left, the largest groups are ID<> and Prototype<> (42 each), FunctionImpl<> (28),
LuaCommonEventFilters (24) and the stdlib's Filesystem (53: declarations without definitions,
to be replaced by a real std::filesystem subset).

### cxxopts (2026-10-28)

The command-line parser the game uses (`libraries/cxxopts`, one header) converts like game code:
it is in the conversion and in the headline inputs. Its one standard header without a Kpp
counterpart is `<regex>`, for five fixed patterns matched with `std::regex_match`: an integer,
`true`, `false`, an option (`--name=value` or `-abc`) and an option specifier (`s,long`).
Decided: no stdlib Regex while cxxopts would be its only user (about 500 lines for five
patterns); `programs/factorio/overrides/cxxopts/cxxopts.kpp` hand-writes the five matchers, with
std's capture groups and `regex_match`'s whole-text rule (`basic_regex`, `match_results`,
`sub_match`, `smatch` and `regex_match` in namespace cxxopts; any other pattern throws). A
library Regex comes when a second user does. kovarex can overrule this.

The converter evaluates `#if` over the symbols it knows (`CXXOPTS_USE_UNICODE` and
`CXXOPTS_NO_RTTI` are undefined: the game builds cxxopts without ICU and with RTTI), but the front
end parsed the first branch of every `#if`. In cxxopts that put the ICU branch in the AST and the
`std::string` branch in the output (`typedef String String;`), so every rewrite that needs the
AST missed the code written out. In convert mode the front end now lexes each C++ file with the
branches the converter drops blanked out (`selectConfiguredBranches` in convert.cpp; offsets are
unchanged), so the two see the same code. In the game's own sources that changed 7 files, each
for the better: Log macros, `ADD_VALUE`, the `GameMap` rename and `std::cin` now reach code in
`#else` branches. cxxopts went from 26 errors to 13; the rest of the headline did not move (30,524
before, 30,521 after, within the run-to-run noise); with cxxopts the count is 30,534.

### Library and converter round (2026-10-28 evening)

Measured with one compiler per pair (the base worktree at master, then the change on top),
headline as defined above:

| change | headline |
|---|---:|
| master at 54e87e8 (after the comparator, member-type, clash-rename, Atomic and Limits work) | 28,992 |
| algorithms as iterator templates (docs/stdlib/algorithms.md) | 28,912 |
| std::chrono as Duration/TimePoint/SteadyClock/SystemClock (docs/stdlib/time.md), literals | 27,866 |
| Lua's function-like macros expanded at their 642 uses | 27,411 |

By package from 28,992 to 27,411: Script 2,881 -> 2,545, src top level 2,456 -> 2,235, Util 1,670
-> 1,404, Net 579 -> 378, Gui 2,779 -> 2,618, Graphics 1,036 -> 969, Input 458 -> 389, LED 79 ->
48, CommonUtil 57 -> 38; Entity (8,113 -> 8,111) and CircuitNetwork (1,270) wait on class layouts
(the second polymorphic base). Earlier the same evening, from 29,783: Map/Set comparators and
HashMap hashers (-104), Atomic and SharedPtr atomics with `_WIN32_WINNT=0x0601` (-370), container
member types and PriorityQueue (-209).

On 2026-10-29, master at 25e7d74 (the other threads' fixes since 27,411) measures 24,111. The
converter now drops a std container's allocator argument (`std::vector<T, Allocate>` becomes
`Vector<T>`: the library's containers have no allocator parameter) and the template parameter
that only it used, so CommonUtil's `Util::eraseIf` and `eraseEmpty` overloads match again: 24,097
(the unresolved calls in the 21 functions that call them are gone; 8 of those functions now stop
at a later, unrelated class layout error). Later that night, on master 9c965bd (23,973): durations
and time points add, subtract and compare in their common duration as in C++ (docs/stdlib/time.md;
`Util::sleep_for`'s two mixed-period calls): 23,971; `std::thread::hardware_concurrency()` as
`Thread::hardwareConcurrency()` (8 functions get past the call; 3 of them stop at a later error):
23,966. Then Vector's range insert from any input iterators (SmallVector's MoveIterator): 23,927;
on master deca1f5 (23,720) macro parameters on continued lines (CALCULATE_VERTICES in the sprite
drawing): 23,714; LuaCPPUtilities.hpp converted into kpp\Lua, which the headline now includes:
23,548.

std::variant (2026-10-29): the converter already turns the 9 std::variants the game declares into
typed unions (`union enum`, design.md 8.5) and their get/holds_alternative/get_if/visit uses into
the union's operations. Code written for any `std::variant<T...>` (SerialiserHelper's saveValue,
DeserialiserHelper's loads that recurse over variant_size_v, the is_std_variant specialization) is
dropped, 7 declarations, and the overrides write it for typed unions with `for constexpr (auto a :
alternatives(U))`. Those overrides add members to converted classes: an override file's `class
[[members]] Name { ... }` puts its members into the converted class Name, whose other members stay
as converted (a member that finds no such class is reported on stderr). `Util::visitVariant` is
overridden for its one use (one lambda). 23,548 -> 23,546. Then, on master e6120e0 (23,636): each
converted union gets std::variant's `==` (the same alternative with equal values; a member
template, so that an alternative without `==` matters only where unions are compared), and
`f(std::in_place_type_t<T>(), args...)` becomes `f(T(args...))`: 23,634 (DrawCommandBatch's batch
comparisons; its in-place emplace_back now waits on typed unions direct-initialized from a value,
spec kpp/typed_union_direct_init). The game's is_std_variant<T> asks `isTypedUnion<T>` (an override
of TemplateHelpers), which routes the 9 unions' save and load through the members above: 23,633.
An alternative named like a type hides the type inside the union, so the converter qualifies such
names in the alternatives' types, now also inside template arguments (`Pair<::EntityWithQualityID,
::EntityWithQualityID>`): UndoActionDescription's and TextDrawSection's unions lay out, 23,526.

static_assert became a real check on master 89058a6 (the headline now includes kpp\Lua: 23,548).
Until then the game's 470 static_asserts were parsed and never evaluated, so the step adds lines
honestly: 23,643 (+95), all of them static_asserts. 83 have a condition the checker cannot evaluate
yet (one error per place, however many instances meet it: constexpr function bodies such as
AreCreatedInOrder<...>(), variable templates over traits, floating constants, which the checker
no longer cuts to integers); each is a gap of the checker's constant evaluation. 12 "fail", and
those are the checker's mistakes, not the game's: a trait it answers wrongly (isBaseOf through
the bases it cannot see, isMoveConstructible) or an instance it should not have made. A further
268 assert a class's size or alignment, which only the code generator's layout knows exactly (bit
fields, empty bases); they are counted on the report and left to the code generator's check.

### Error breakdown by root cause

| category | count | % | what it is |
|---|---:|---:|---|
| **(a) Third-party references** | **9,217** | **28%** | Code touching libraries kppc does not convert |
|   Lua | 8,899 | 27% | `lua_pushinteger`, `luaL_checkstring`, the scripting layer |
|   SDL | 312 | 1% | `SDL_Rect`, `SDL_CreateWindow`, rendering and input |
|   other (tracy, imgui) | 6 | <1% | Profiler and debug UI |
| **(b) Unexpanded macros** | **~1,271** | **~4%** | Macros the converter left as comments |
|   `SAVING_TAG` / `SAVING_STATISTICS` | ~1,271 | 4% | 527 files; done 2026-10-28: scoped guards, converted to `_` locals (macro survey F) |
| **(c) Serialisation system** | **2,032** | **6%** | `MapSerialiser<T>`, `MapDeserialiser<T>`, `SerialiserHelper` chains |
| **(d) ID system templates** | **341** | **1%** | `ID<T>`, `IDType`, `IDConnector`, `PrototypeList` |
| **(e) Resolver gaps** | **2,998** | **9%** | Types the resolver did not propagate |
|   type left as `?` | 1,777 | 5% | A member or return type the resolver could not follow |
|   `<unknown>` type | 713 | 2% | An expression whose type is completely unknown |
|   template parameter not substituted | 377 | 1% | A `T` that stayed `T` after instantiation |
|   cascade (`?` in template arg) | 131 | <1% | An instantiation whose argument was already `?` |
| **(f) Codegen gaps** | **16,207** | **49%** | Types present in the subset but code not emitted |
|   unresolved member access | 4,422 | 13% | `this->commandable->getMap()` — resolver did not chain |
|   unresolved call | 4,196 | 13% | `entity.getPosition()` — call target not resolved |
|   unresolved name | 1,729 | 5% | `normalQuality` (411), `Nothing` (152), `Success` (142) — enum values |
|   missing operator | 1,399 | 4% | `==`, `<`, `!=` on game types |
|   constructor call | 1,248 | 4% | `MapPosition(x, y)` — constructor not matched |
|   local of unknown type | 788 | 2% | `auto x = ...` where the initialiser did not resolve |
|   type conversion/binding | 716 | 2% | Implicit conversions the resolver missed |
|   range-for | 593 | 2% | `for (auto& e : entities)` — iterator not resolved |
|   wrong member kind | 582 | 2% | A member found but not as the expected kind (field vs method) |
|   member on non-class | 486 | 1% | `.x` on something typed as a non-class |
|   template deduction failure | 48 | <1% | `template<class T> f(T)` — deduction did not run |
| **(g) Other** | **2,087** | **6%** | Remaining long tail |

### Top 5 concrete patterns

1. **Unresolved member chain** (4,422): the resolver resolves the first access but not the
   second. Example: `this->commandable->getMap()` in `BuildBaseBehavior::prepareBuilding()` —
   `commandable` resolves to `Commandable*` but `getMap()` on the result does not propagate.

2. **Unresolved call on a known type** (4,196): a method call on a type that IS in the subset.
   Example: `entity.getPosition()` where `entity` is typed `Entity&` — the `getPosition` method
   exists but the resolver did not bind the call.

3. **Enum values as unresolved names** (1,729): enum members like `normalQuality` (411 uses),
   `Nothing` (152), `Success` (142) — the parent enum is in scope but the unscoped enumerator
   is not found by name lookup.

4. **Missing operators on game types** (1,399): `==`, `<`, `!=` on types like `MapPosition`,
   `Direction`, `ItemID` — the types exist but their comparison operators are not found.

5. **Unexpanded SAVING_TAG macro** (~1,271 in 527 files). Done 2026-10-28: it is not an X-macro
   but a guard object with a per-line name, so the converter writes `Serialiser::TagWriter
   _(output, "item", ...)` (macro survey section F), also where a file's own macro wraps it.

### Where to focus next

1. **Resolver chaining** (unresolved-member + unresolved-call): ~8,600 errors (26%). Making the
   resolver propagate types through `a->b->c()` chains would cut almost a third of all errors.
2. **Enum value lookup**: ~1,700 errors (5%). Unscoped enum values need to be found from the
   enclosing scope.
3. **Operator resolution**: ~1,400 errors (4%). Find `==`/`<`/`!=` on game types.
4. ~~**SAVING_TAG converter mapping**~~: done, `_` guard locals.
5. **Third-party stubs or exclusion** (Lua): 8,899 errors (27%). C import of Lua headers or
   exclusion of the scripting layer.

### Rule

No silent fallbacks. Every one of these 32,884 errors must end up as either compiled correctly
or a proper diagnostic. The `--errors` / `--unresolved` caps are for iteration; the target is
zero.
