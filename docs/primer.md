# kpp for a C++ programmer

kpp is C++20 with some rules changed and some things added. What this page does not mention works as in
C++: templates, lambdas, RAII, exceptions, `auto`, range `for`, structured bindings, `requires`, namespaces,
operators. Most C++ habits that are wrong in kpp are compile errors whose message names the fix; section 1
is the two that compile and do something else. In an existing file, write as the code around you does.

## 1. Two habits that compile and do something else

```kpp
switch (mode)
{
  case Mode::Idle:                  // a case ends at the next label: nothing falls through,
    rest();                         //   and a `break;` that ends a case is an error
  case Mode::Charging:              // WRONG habit: this case does nothing, it shares no body with the next
  case Mode::Working, Mode::Moving: // one body for several values is a list
    work();
  case Mode::Broken:
    fallthrough;                    // running into the next case is said, as the last statement
  case Mode::Lost:
    report();
}                                   // no default: every value of an enum class must be named, or it is an error
std::println("x = {}", x);          // WRONG habit: prints `x = {}5`. Text is pieces in order (section 4)
```

## 2. C++ habits that are errors

| C++ | kpp |
|---|---|
| `#include`, headers, forward declarations, `#define`, `#ifdef` | none: every top-level name is visible in every file, in any order; a new `.kpp` file under the source root is in the program |
| a member named bare (`count`, `update()`), `this->count` | `this.count`, `this.update()`: always through `this`, which is a reference |
| `return *this;`, `f(this)`, `delete this` | `return this;`, `f(&this)`, `delete &this` |
| a static member named bare | `This::count` (`This` is the class around the code) or `Turret::count` |
| `[this]`; `[=]` with a member used | `[&this]`; `[=, &this]` (`[&]` is as in C++) |
| `explicit T(i32 x)` | `T(i32 x)`: nothing converts implicitly unless marked `implicit T(i32 x)`, `implicit operator bool() const` |
| `draw({1, 0, 0})` | `draw(Color{1, 0, 0})`: an argument's braces name their type |
| an override without `override` | `virtual void update() override` |
| `using super = Base;`, `Base::update()` | `super::update()`: `super` is the first base, built in |
| `bool operator!=(...)` | never written: `a != b` is `!(a == b)` |
| `if (x = next())` | `if ((x = next()))` |
| `int`, `unsigned`, `size_t`, `uint8_t`, `float`, `double` | `i8` `i16` `i32` `i64`, `u8` `u16` `u32` `u64`, `usize`, `f32` `f64` (C's names only at a C boundary) |
| `NULL`, `0` for a pointer | `nullptr` |
| `std::string`, `vector`, `unordered_map`, `unique_ptr<T>`, `make_unique`, `to_string` | `std::String`, `std::Vector`, `std::HashMap`, `std::UniquePtr<T>` (also written `T^`), `std::makeUnique`, `std::toString`: types UpperCamelCase, functions lowerCamelCase, the rest as in C++ (`push_back`, `find`, `size`, `std::sort(v.begin(), v.end())`, `std::move`) |
| `printf`, `std::cout`, `s += x` | `std::println("x = ", x)`, `s << x` (section 4) |
| `using namespace std;` | `std::` written out |
| an unused local, parameter, lambda capture or private field | remove it, leave the parameter unnamed, or keep it with `(void)x;` or `[[maybe_unused]]` |
| `static` or an anonymous namespace for a file's helper | `private i32 helper() { ... }`; any other top-level name is global to the program, and a second one of that name in another file is an error, a `static` or `const` one too |
| `f64 ratio = a / b;` of integers | `f64(a) / b` |
| a `struct` with a private member or a virtual function | `class`: a struct is an all-public value type (constructors and methods are fine) |
| a function that can reach its end without returning its value | an error: return or throw on every path |
| `a < b < c`, `return std::move(local);`, `if (x);` | errors too |

Also: a local without an initializer is zero (`i32 n;`), but reading it before anything is assigned is an
error, so write `= 0` where the zero is meant. A class with virtual functions has a virtual destructor
without writing one. A case is a scope of its own: its locals need no braces. `typename` and `template`
before dependent names are not needed. Any function may be called in a constant expression. A name the
library has under another spelling: the error says `did you mean`.

## 3. What a class no longer needs

```kpp
struct Position
{
  i32 x = 0;
  i32 y = 0;
};
Position a(1, 2);               // a struct is built member by member, also beside constructors it declares
Position b(1);                  //   (the last members may be left to their initializers)
bool same = a == Position{1, 2}; // and has ==, unless a member is a pointer or reference: do not write one

class Account
{
public:
  Account(i64 balance) : balance(balance) {}

  static i32 opened = 0;        // a static member is initialized where it is declared

private:
  i64 balance = 0;
};
Account empty;                  // a class keeps its default constructor beside its other constructors

void build(Position at, Direction direction = Direction::North, PlaySound sound = PlaySound::True);
build(a, PlaySound::False);     // an argument that fits no position goes to the default parameter of its type
```

## 4. Text

```kpp
std::println("x = ", x, ", name = ", name);    // the pieces in order: no format string
std::String s = std::toString("id ", id);      // the same pieces as a String
s << " at " << position << " " << std::hex(mask) << " " << std::fixed(ratio, 2); // << appends; there is no +=
std::String t = "robot " + name;               // + makes a new String
void log(std::Text text);                      // a function that takes pieces: log("took ", ms, " ms")
void appendTo(std::String& out) const { out << this.x << "," << this.y; } // this one member makes a type printable
assert(count > 0, "count = ", count);          // a statement; release_assert stays in a release build
```

An enum prints as its name, a bool as `true`, a `u8` as its number.

## 5. Enums, flags, explicit types, typed unions

```kpp
enum class Direction : u8
{
  { North, East, South, West }                 // the values in a block of their own when members follow

  bool isVertical() const { return this == North || this == South; } // this is the value
};
// every enum class has Direction::count, Direction::values, d.str(), Direction::fromStr("East") (an Optional)
// and d.isValid(); it indexes an array (counts[d]) and is an integer only by a cast: u8(d)

flags Usage : u8 { Damaged, Marked, Frozen };  // a set of named bits, instead of an enum of masks
Usage usage = Usage::Damaged | Usage::Frozen;  // `Usage usage;` is the empty set; it prints as `Damaged | Frozen`
usage.set(Usage::Marked);
usage.clear(Usage::Damaged);
if (usage.has(Usage::Frozen) && !usage.hasAny(Usage::Damaged | Usage::Marked)) // the integer: usage.bits(), Usage::fromBits(n)
  thaw();

using ItemIndex = explicit u16;                // a type of its own: ItemIndex(3), u16(index), and it mixes with
using PlaySound = explicit bool;               // no other type (index == ItemIndex(0)); PlaySound::True, PlaySound(b)

union enum class Action : u8                   // a tagged union, instead of std::variant
{
  { {Move, Position}, {Say, std::String}, {Stop, void} } // always in a block of their own

  i32 cost() const { return this == Tag::Stop ? 0 : 1; }
};
Action action = Action::Move(Position{3, 4});  // a creator per alternative: Action::Say("hi"), Action::Stop()
if (action == Action::Tag::Move)
  action[Action::Tag::Move].x = 7;             // u[tag] is the value, and asserts that tag is held
switch (action.tag())                          // exhaustive, as an enum's
{
  case Action::Tag::Move:
    go(action[Action::Tag::Move]);
  case Action::Tag::Say, Action::Tag::Stop:
    wait();
}
// in a statement, u[] stands for whichever alternative is held: `this[].save(output);`, `return this[].id;`
```

## 6. Null checks and configurations

```kpp
std::String label = crate?->label?->text ?: "none"; // ?-> skips the rest of its chain when the left side is null,
train?->stop();                         //   so use it where C++ tests each pointer; a ?: b is a ? a : b
if constexpr (config::debug)            // instead of #ifdef: at class and namespace scope too, and the branch
  verify();                             //   not taken is not checked, in any function
when (config::debug) i32 checks = 0;    // a variable or member that exists only in that configuration
if constexpr (!std::isStruct<T>)        // after an if constexpr that returns, the rest of the block is its else
  return std::toString(object);
for constexpr (auto m : members(T))     // reflection: m.name, m.type, object.[m]; also enumerators(E) (e.name,
  out << m.name << "=" << object.[m];   //   e.value), alternatives(U) (a.name, a.type, a.tag), nameof(x)
```

`extern "C" import "header.h";` anywhere imports a C header for the whole program. A project is described
by its `build.kpp` (kpp code, `void build(Project& project)`), which names source directories, not files.

## 7. Tests

```kpp
test squareOfThree              // a name, not a string
{
  check(square(3) == 9);        // goes on after a failure
  require(square(0) == 0);      // ends the test
}

suite Counting                  // its helper functions are seen by its tests only
{
  test FromTen : Counter(10)    // a fixture: the body is a member of a class derived from Counter
  {
    require(this.value == 10);
  }
}
```

## 8. Style

- 2 spaces, no tabs, lines up to 130 characters. Braces on lines of their own; none around a single statement.
- A function whose body fits on its line is a one-liner (`i32 getX() const { return this.x; }`), and
  one-liners follow one another with no empty line between them. A function written over several lines has
  an empty line before and after it. Never two empty lines, none after `{` or before `}`.
- Member functions are written in the class; data members come last. Each class, enum and helper namespace
  has a file of its own, named after it.
- Return early instead of nesting; no `else` after a `return`. `if (pointer)`, `if (!items.empty())`. Where a
  null pointer only skips a call or a read, `?->` says it, not an `if`.
- Types and enumerators `UpperCamelCase`, functions and variables `lowerCamelCase`, constants `ALL_CAPITALS`,
  `ID` in capitals (`getItemID`). A default value goes at the member (`i32 x = 0;`), not in each constructor.
- A getter and setter that only read and write a member are not written: the member is public.
- kpp is written in lower case.
