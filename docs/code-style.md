# kpp code style

kpp code is written in Factorio's style (kovarex, 2026-11-05: "Look at factorio code style and make it
official kpp code style"). The rules below are Factorio's code style guide (C:\Factorio's
doc/style-guide/factorio-basic-style-guide.md) carried over to kpp, where kpp has no headers and no
includes, plus kovarex's own rules for kpp code. They hold for the standard library (stdlib/), the
libraries (libraries/), the programs (programs/, kppnet/), build.kpp files, the spec snippets where a
snippet does not test the layout itself, and gorating. `tools\style\kppstyle.py` checks the rules a
program can check (see "Checking").

The aim is Factorio's: no bloat, code as compact as it can be without losing readability.

## Layout

- Lines are at most 130 characters. A line is not a wall: a little over is fine where breaking it
  would read worse.
- Indents are 2 spaces. No tabs anywhere in a text file.
- No trailing spaces.
- Every text file ends with a line end.
- No two consecutive empty lines.
- No empty line at the beginning or the end of a block (after `{`, before `}`).

## Empty lines between definitions

A function whose body fits on its line is a one-liner, and one-liners follow one another with no
empty line between them:

```kpp
i32 getX() const { return this->x; }
i32 getY() const { return this->y; }
```

A function written over several lines (its body's braces on lines of their own) is expanded, and
an expanded function has an empty line between it and what comes before and after it in the same
block (kovarex, 2026-11-05):

```kpp
i32 getX() const
{
  return this->x;
}

i32 getY() const
{
  return this->y;
}
```

A comment above a function belongs to it: the empty line goes above the comment. The first
definition in a block (after `{` or an access label such as `public:`) and the last (before `}`)
need none. A class, struct, enum or namespace written over several lines is separated the same
way.

## Braces

- A brace goes on a line of its own (no "egyptian" braces), for functions, classes, control
  statements and initializer lists alike, except in a one-liner (`{ return this->x; }`).
- No braces around a single statement:

  ```kpp
  if (a > b)
    command();
  ```

- An empty body is `{}`, no space between the braces. A constructor or destructor that does what
  the language does is `= default` (`Position() = default;`), never `Position() {}`, and one the
  language gives anyway is not written.

## Spaces

- A space after `if`, `for`, `while` and `switch`, none inside the parentheses: `if (a < b)`.
- Spaces around binary operators: `if (a < b || b > 5 && a + 1 > 5)`.
- A space before the `:` of a range for: `for (const Item& item : items)`.
- `*` and `&` go with the type: `u32* pointer`, `const String& name`.

## Control flow

- Return early when a condition for going on is not met, rather than nesting the rest deeper.
- No `else` after a `return` (except in an `if constexpr`, or an `if ... else if` chain where a
  branch before does not return).
- A pointer is tested as itself: `if (pointer)`, not `if (pointer != nullptr)`. An Optional or a
  smart pointer likewise: `if (value)`.
- A container is tested with `empty()`, not `size() == 0`.
- `case` labels are indented one step inside their `switch`:

  ```kpp
  switch (direction)
  {
    case Direction::North:
      return 0;
  }
  ```

## Line wrapping

A line that is too long is broken after an operator, which stays at the end of the line; the
continuation is indented to line up with what it continues. Where `&&` and `||` mix, a line holds
one `&&` group:

```kpp
if (functionThatReturnsBoolAndHasAVeryLongName() ||
    condition1() && condition2() ||
    condition3() && condition4() && condition5())
  item.update();
```

The same for `?` and `:` of a conditional expression.

## Names

- Types and enum constants are UpperCamelCase (`EntityPrototype`, `Direction::North`).
- Functions and variables are lowerCamelCase (`getObjectID`, `itemCount`).
- Constants are ALL_CAPITALS (`MAX_DEPTH`). (Factorio's guide says so; its code has both forms,
  about 660 ALL_CAPITALS and 760 lowerCamelCase static constexpr members: a new constant is
  ALL_CAPITALS, an old one is renamed when its code is worked on.)
- "ID" is all capitals wherever its I would be capital: `getObjectID()`, not `getObjectId()`.
- The other operand of a two-sided operator (`==`, `<`, `=`...) is named `other`.
- Integers are kpp's names, `i32`, `i64`, `u8`, `u32`, `usize`, ..., never C's (`int64_t`, `size_t`,
  `int`) outside `extern "C"` (kovarex, 2026-11-05; `kpp --check --c-type-warnings` lists them).
- kpp is written in lower case in comments and messages (only identifiers keep a capital).

## Classes

- A class keeps its data private where it has an invariant to keep (SafeHtml); a struct is a group
  of public values. A getter and a setter that only read and write a member are not written: the
  member is public instead. They are written when one of the two must not exist, or when a change
  needs more than the assignment.
- One class to a file, the file named after it.
- Members come in this order: `using super = Base;` (private, and only when it is used) and other
  usings; constructors; the destructor; member functions; static functions and variables; data
  members, last (kovarex: Factorio's layout).
- A default value is given at the member (`i32 x = 0;`), not repeated in each constructor's
  initializer list.
- An initializer list longer than a couple of plain members goes one member to a line:

  ```kpp
  Inserter(const InserterPrototype& prototype, MapPosition position)
    : EntityWithOwner(prototype, position)
    , pickupTarget(nullptr)
    , dropTarget(nullptr)
  {}
  ```

- One base is written on the class's line (`class Derived : public Base`); more than one go one to
  a line, `:` before the first and `,` before the others.
- A function that overrides a virtual one says both `virtual` and `override`.
- `super::` calls the base's version of a virtual function (in save, update...), not its
  constructor.
- `==` that compares every member is defaulted (`bool operator==(const Position& other) const =
  default;`).
- Every enum is an `enum class`.

## Inside functions

- A member is always named through `this->`: `this->count`, `this->update()`. A static member or a
  class's constant through its class: `Inserter::MAX_STACK_SIZE`.
- A parameter that allocates or is bigger than two pointers is passed by const reference.
- A pointer that cannot be null is a reference, as a parameter and as a result.
- `auto` where the type is noise (iterators, generic lambdas), not where it hides a type the reader
  needs.
- Structured bindings name a pair's parts: `for (const auto& [entityID, count] : this->counts)`.
- Text is built with `<<` (`out << "count: " << count`), not `.append(` or `+` chains, and SQL and
  messages as Text pieces (design.md 14).

## Comments

A comment goes at the end of the line it explains when it fits there (`i32 depth = 0; // nesting
so far`); above it when the line would get too long. A comment above a definition says what the
definition is for, in a sentence, and the empty line of "Empty lines between definitions" goes
above the comment.

## What is not carried over

Factorio's include rules (forward declarations, two sorted groups) have no counterpart: kpp has no
headers or includes. Factorio's `LOG_AND_ABORT` is kpp's `assert` and exceptions.

## Checking

`tools\style\kppstyle.py PATH...` reports the rules it can check in .kpp files, by file and line:
line length, tabs, trailing spaces, the final line end, consecutive empty lines, empty lines at a
block's start and end, the empty lines between expanded definitions, braces at the end of a line,
a missing space after `if`/`for`/`while`/`switch`, `X() {}`. `--fix` corrects the empty lines and
the spaces at line ends (it adds and removes empty lines only, never inside a string or comment
that spans lines); the rest it reports. `--summary` counts by rule, `--rule NAME` keeps one rule.

The spec snippets are not reformatted by `--fix`: their expected messages name line numbers. A new
snippet is written in the style.
