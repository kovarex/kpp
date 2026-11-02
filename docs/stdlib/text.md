# Text: numbers as text and the formatting pieces

Text output (design.md §14) puts a text together from pieces, one after another, with nothing parsed:
`print("Hello ", name, " you are ", age, " years old")`, `out << value << ", "`. Every type that can be
a piece has `void appendTo(String& out) const`, which writes its text at the end of `out`. This page
specifies the texts of numbers and the formatting pieces, which choose another text for a value. The
pieces are what converted printf specifiers become.

## Numbers

- An integer is its decimal digits, with a `-` before a negative one: `toString(-42)` is `-42`.
- `bool` is `true` or `false`.
- A floating value, `double` or `float`, is the shortest text that reads back to the same value of
  its own type: all its information, nothing lost (kovarex, 2026-11-01). `0.1` is `0.1`, `0.1 + 0.2`
  is `0.30000000000000004`, `1.0 / 3` is `0.3333333333333333`, and the float `1.0f / 3` is
  `0.33333334`. The form is C's `%g` at that number of significant digits: an exponent below 1e-4
  (`1.5e-07`) and from 1e16 (`1e+21`, `1.2345678901234568e+17`), positional between (`100`, `-2.75`,
  `16777216`). A negative zero is `-0`; infinities are `inf` and `-inf`, and NaN is `nan`, on every
  platform.

**Difference from C++:** `std::to_string` of a floating value is `%f`, six decimals (`0.100000`). The
conversion is to write `fixed(x, 6)` for it, which is that text exactly (with the printf rewrite).

## Formatting pieces

Each is a small value whose `appendTo` writes the text described; the names are kovarex's
(design.md 14). None cuts a longer text. Each is what a converted printf specifier becomes.

- `hex(x)`, `hex(x, digits)`, `hexUpper(x)`, `hexUpper(x, digits)`: an integer in hexadecimal,
  lowercase or uppercase, zero-padded to `digits`. A negative value is the bits of its own type:
  `hex(-1)` of an `int` is `ffffffff`, of a `long long` `ffffffffffffffff`, of a `short` `ffff`.
  (C's `%x`, `%08x`, `%llx`, `%X`.)
- `octal(x)`, `octal(x, digits)`: the same in base 8 (`%o`): `octal(0644)` is `644`.
- `fixed(x, decimals)`: a floating value with `decimals` digits after the point, rounded to the
  nearest and a tie to even, as C's `%.Nf`: `fixed(3.14159, 2)` is `3.14`, `fixed(2.5, 0)` is `2`,
  `fixed(0.1, 20)` is `0.10000000000000000555`.
- `significant(x, digits)`: `digits` significant digits with trailing zeros removed, an exponent
  below 1e-4 and from 1e(digits), as `%.Ng`: `significant(1234567.0, 6)` is `1.23457e+06`,
  `significant(100.0, 6)` is `100`. C's `%g` is `significant(x, 6)`.
- `scientific(x, digits)`: one digit, the point, `digits` more and an exponent of at least two
  digits, as `%.Ne`: `scientific(1234.5, 2)` is `1.23e+03`.
- `withSign(piece)`: the piece's text with a `+` before it unless it starts with `-` (C's `+` flag):
  `withSign(fixed(1.25, 1))` is `+1.2`.
- `alignRight(x, width)`, `alignRight(x, width, fill)`, `alignLeft(...)`: the text of any piece `x`
  in `width` characters, `fill` (a space by default) before it or after it (`%5d`, `%8s`, `%-8s`).
- `zeroPadded(x, width)`: the text of `x` zero-padded to `width` after its sign (C's `0` flag):
  `zeroPadded(-5, 4)` is `-005`, `zeroPadded(withSign(3), 4)` `+003`.
- `address(p)`: a pointer as 16 uppercase hexadecimal digits, no prefix, as MSVC's `%p`. It is the
  only way to print a pointer: a pointer is no piece by itself.

Infinities and NaN are `inf`, `-inf` and `nan` in every floating piece, on every platform.

## Appending and printing

- `out << x << y`: appends the pieces' texts to a String and returns it. It is String's only append
  of a text: String has no `+=` (design.md 14). On a temporary String it returns the String:
  `String(name) << '_'` (C++'s `std::string(name) += '_'`).
- `print(pieces...)`, `println(pieces...)` (a newline after): the text on standard output.
  `toString(pieces...)`: the text as a String. `format(pattern, pieces...)`: each `{}` of the
  pattern replaced by the next piece, the pattern checked when compiling (a `{` without its `}`
  is an error), and so is a call whose pieces are not one per `{}` (`format("{} has {}", name)`:
  "FormatString does not take 1 piece: it throws LogicError("format: 2 {} for 1 piece")"); `{{`
  and `}}` are no escapes: a text with braces of its own is written in pieces.
- `a + b`, one side a String or a StringView: a new String of both texts.
- C text is no piece: `StringView(p)` is a `const char*`'s text, `address(p)` its address.
- **A piece refers to its object; it is read when the text is written**, after every argument of
  the call is evaluated, as std::format reads its arguments (by reference), and unlike printf's
  copies. `println(cache.hits, " ", cache.get())`, where get() increments the mutable `hits`, prints
  the count after that get(): 3, not 2, after two earlier calls (compiler/tests/spec/classes/
  const_and_mutable). To print the value before the call, read it into a local first. C++ leaves
  the order of a call's arguments unspecified, so code relying on printf's copy for this was
  already unspecified.
