# String

A sequence of bytes (UTF-8 code units, not checked), stored contiguously and always followed by a
zero byte, so `c_str()` is free. Like a Vector of `char` with text operations. One type: no
character-type, traits or allocator parameters.

Short strings (up to an implementation-defined length, at least 15 bytes) are stored inside the
String object itself without allocating; longer ones in a heap block.

## Invalidation

Iterators are `char*`. Pointers and references into a String (including `c_str()` and `data()`)
are invalidated by every operation that changes the String (assignment, append, insert, erase,
resize, reserve, clear, push_back, pop_back, swap, being moved from) and by its destruction; they
stay valid across calls of const functions. *Note*: moving a String may leave its characters in
place in the source object (short strings), so pointers into a moved-from String do not follow
the move.

## Positions and npos

Indices are sizes (`ContainerSize`, common.md). Searches return the index of the match, or `npos`,
the largest size (2^64 - 1), as C++'s npos is the largest value of its size
type: a search result compares with npos and with sizes as in C++ (`min(s.find(x), s.size())` is
the size when x is absent, and `s.find(x) < s.size()` is false), and as a count npos (or any count
past the end) means "to the end". Stored in an `int` it is -1 and in a `size_t` it is `SIZE_MAX`, as
C++'s are. (Until 2026-10-29 npos was -1, which made the game's
`Math::min(input.find(separator), input.size())` -1, and its localised strings came out as their
own suffixes; until 2026-11-01 it was the largest `int`, then briefly the largest `u32`.)

## Members

Construction: `String()` (empty), from a zero-terminated `const char*`, from `(const char*, count)`,
from a `[first, last)` pair of `const char*`, `(count, c)` (count copies of c), from a StringView.
Copy and move as in common.md.

Access:
- `size()`, `length()`, `empty()`, `capacity()` (bytes that fit without allocating).
- `operator[](i)` for i in [0, size()]; index size() reads the terminating zero (writing it
  anything else is undefined). `at(i)`: throws OutOfRange when i is not in [0, size()).
- `front()`, `back()` (not empty), `data()`, `c_str()`: the bytes, zero-terminated, valid also
  for an empty String.
- `begin()`, `end()`; `rbegin()`, `rend()`: the bytes from the last to the first.

Changing (each keeps the zero terminator; appending is amortized O(added bytes) through geometric
growth; a text argument may point into this String). append, insert at an index, erase at an
index, replace and assign return the String (`return s.append(x);`, `s.append(a).append(b)`); the
forms that take a position return a position:
- `append(text)`, `append(text, count)`, `append(first, last)` (the bytes of a pointer range),
  `append(count, c)`, `append(other, index, count)` (count bytes of other from index, fewer when
  other ends first), `push_back(c)`, `out << x` (any text piece, text.md; String has no `+=`).
- `insert(index, text)`, `insert(index, count, c)`, `insert(position, first, last)`: throws OutOfRange
  when index > size() (the string unchanged); index size() appends. O(size() + inserted).
  `insert(position, c)` returns the new character's position.
- `erase(index, count)`: removes count bytes from index (to the end when count is negative or
  reaches past it). Throws OutOfRange when index > size(). `erase(position)`,
  `erase(first, last)` with pointers, which return where the byte after the removed ones is now.
- `replace(index, count, text)`, `replace(index, count, text, n)`: erase then insert; the text may be part of
  the string (it is the text as it was before the call). `replace(index, count, n, c)` (n copies of c),
  `replace(first, last, text)` with pointers, `replace(index, count, other, index2, n)` (up to n bytes
  of other from index2; OutOfRange when index2 > other.size()).
- `resize(n)`, `resize(n, c)`: cut to n bytes, or append zero bytes / copies of c.
- `reserve(n)`: afterwards `capacity() >= n`; `clear()`: empty, capacity stays; `pop_back()`.
- `assign(text, count)`, `assign(first, last)` (the bytes of a pointer range), `assign(text)`,
  `assign(other)`, `assign(count, c)`, `assign(other, index, count)` (up to count bytes of other from
  index; OutOfRange when index > other.size()): replace the contents (text may point into this String).

Searching (each returns an index or npos; `from` is where the search starts):
- `find(text, from)`, `find(c, from)`, `find(text, from, n)` (the n bytes at text): first occurrence at
  or after from. An empty text is found at from when from <= size().
- `rfind(text, from)`, `rfind(c, from)`: the last occurrence starting at or before from (anywhere
  when from is npos or omitted). `find_first_of(set)`, `find_first_not_of(set)`,
  `find_last_of(set)`, `find_last_not_of(set)`: first/last byte that is (not) in set;
  `find_first_not_of(c)`, `find_last_not_of(c)`: first/last byte other than c; each takes a `from` too
  (set or c), as C++'s pos: the last ones search at or before it.
- `startsWith(prefix)`, `endsWith(suffix)` (kpp additions), and C++20's `starts_with(x)` and
  `ends_with(x)` of a text (a String, a StringView, a literal) or a char; StringView has the same.

Producing new strings:
- `substr(start, count)`: count bytes from start (all the rest when count is negative or runs
  past the end). Throws OutOfRange when start > size(); start == size() gives an empty String.
- `a + b` for any two of String, `const char*` (at least one String) and char.
- `copy(dest, count, pos = 0)`: copies up to count bytes from pos into dest (as many as there are)
  and returns the number copied; no terminating zero. Throws OutOfRange when pos > size(). StringView
  has the same.

`compare(other)`: negative, zero or positive as this string orders before, equal to or after
other by `<`. `compare(pos, count, other)`: the substring [pos, pos + count), count
clamped to what there is, compared so; OutOfRange when pos > size(). `compare(pos, count, text, n)` with
the n bytes at text, `compare(pos, count, other, pos2, n)` with up to n bytes of other from pos2
(OutOfRange when pos2 > other.size()). `swap(other)`: exchanges the contents.

Comparison: `==`, `!=` byte-wise with the same length; `<` byte-wise lexicographic (as unsigned
bytes), a proper prefix before the longer string; `<=>` orders the same way and gives `>`, `<=`
and `>=` (common.md). Comparison with a `const char*` treats it as its zero-terminated text, on
either side. A StringView compares the same way, and has `compare(other)`, `find_last_of`,
`find_last_not_of` and `find_first_not_of` (a set or a char, from a position) as String has.

Hashing: `hashOf(string)` depends on every byte.

Numbers from strings (core/StringConvert.kpp), std's functions with std's parameters:
`stoi`, `stol`, `stoul`, `stoll`, `stoull` take `(str, size_t* idx = nullptr, int base = 10)`,
`stof`, `stod`, `stold` take `(str, size_t* idx = nullptr)`. Each converts as the C runtime's
strto* function does (leading whitespace skipped, a sign, `0x` for base 16 or 0); idx, when not
null, gets the index of the first byte not converted. No conversion throws InvalidArgument
("invalid stoi argument"), a value the type cannot hold (the C runtime's ERANGE) throws OutOfRange
("stoi argument out of range"), the C++ runtime's messages; idx is then left as it was. long is
int's size, as MSVC's.

# WString

A sequence of `wchar_t` (UTF-16 code units on Windows, not checked), what `std::wstring` converts
to: the text the Windows API takes and gives (`CreateFileW`, command lines, paths). Always followed
by a zero unit, so `c_str()` is free; stored in a heap block (no short-string storage). Indices
are sizes and searches return an index or `npos` (the largest size), as for String; iterators are `wchar_t*`,
invalidated as String's are.

Members, as String's of the same name but in units of wchar_t:
- Construction: `WString()`, from a zero-terminated `const wchar_t*` (implicit), from
  `(const wchar_t*, count)`, from a `[first, last)` pair of `const wchar_t*`, `(count, c)`; copy
  and move; assignment from a WString or a `const wchar_t*`.
- Access: `size()`, `length()`, `empty()`, `operator[](i)`, `at(i)` (throws OutOfRange), `front()`,
  `back()`, `data()`, `c_str()`, `begin()`, `end()`, `rbegin()`, `rend()`.
- Changing: `append(text, count)`, `append(text)`, `append(other)`, `append(count, c)` (each
  returns the WString), `push_back(c)`, `pop_back()`, `<<` (a WString, a `const wchar_t*` or a
  wchar_t; no `+=`, as String), `resize(n)`, `resize(n, c)`, `reserve(n)`, `clear()`, `erase(index, count)` (returns
  the WString). A text argument may point into this WString.
- Searching: `find(text, from)`, `find(c, from)` (text: a WString or a `const wchar_t*`),
  `rfind(c, from)`, `find_first_of(set, from)`, `find_last_of(set)`, `find_last_of(c)`.
- `substr(start, count)`: as String's. `a + b` for any two of WString, `const wchar_t*` (at least
  one WString) and wchar_t.
- `compare(other)`, `==`, `!=` (also with a `const wchar_t*`), `<`, `>`, `<=`, `>=`: unit by unit,
  a proper prefix before the longer string.

`toWString(number)` (std::to_wstring) writes a number as String's toString does. Conversions
between WString and UTF-8 Strings are not part of WString (StringUtil's widen and narrow in the
game, and Filesystem's path).
