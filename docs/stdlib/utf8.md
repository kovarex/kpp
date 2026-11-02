# UTF-8

Functions over UTF-8 text in a `StringView` (a `String` converts to one): validating it, decoding
and iterating its code points, encoding a code point, counting code points. C++ has no
equivalent worth keeping (std::codecvt is deprecated, char8_t brings no functions), so these are
kpp's own. json, html, markdown and url use them.

## Terms

- A *code point* is a `u32`. A *scalar value* is a code point that UTF-8 can encode:
  U+0000 to U+10FFFF, except the surrogates U+D800 to U+DFFF. `isScalarValue(c)` tells.
- *Valid UTF-8* is what the Unicode Standard (3.9, table 3-7) calls well-formed. Every scalar
  value is written in its shortest form of 1 to 4 bytes. There are no overlong forms, no surrogates,
  nothing past U+10FFFF, no stray continuation bytes and no sequence cut short.
- Positions and lengths are byte offsets and byte counts, sizes as everywhere in the library
  (`ContainerSize`, common.md).

## Bytes that are not UTF-8

Decoding never fails and never skips anything. Where the bytes at a position are not valid UTF-8,
the decoder takes the *maximal subpart* there and gives U+FFFD for it, the replacement character.
That is the longest prefix of a valid sequence that the bytes start (at least one byte). Every
decoding function below shares this rule, so a text decodes the same way forwards, backwards and
through the iterator. It is Unicode's recommended practice (3.9, "U+FFFD Substitution of Maximal
Subparts") and what the WHATWG Encoding Standard's decoder does, so a browser shows the same
characters.

Each decoding function also says whether the code point was valid. A caller that must reject
such bytes (json: "bytes that are not UTF-8 in a string") checks that, or validates first.

## Functions

- `bool isUtf8(StringView text)`: whether all of text is valid UTF-8.
- `ContainerSize utf8InvalidAt(StringView text)`: the position of the first byte that starts no
  valid sequence, or `npos` when text is valid (an error message's column).
- `Utf8Decoded utf8Decode(StringView text, ContainerSize at)`: the code point that starts at `at`
  (`at < text.size()`). It gives:
  - `.codePoint`: the scalar value, or U+FFFD;
  - `.length`: the bytes it takes, 1 to 4 (a maximal subpart's for U+FFFD);
  - `.valid`: false where U+FFFD replaced bytes that are not UTF-8 (a U+FFFD the text spells
    validly is valid).
- `Utf8Decoded utf8DecodeBefore(StringView text, ContainerSize at)`: the code point that ends at `at`
  (`0 < at <= text.size()`). It is the one utf8Decode gives at the start of the last sequence or
  maximal subpart before `at`, so decoding backwards meets the same code points as forwards.
  `.length` tells how far back it starts.
- `Utf8CodePoints utf8CodePoints(StringView text)`: the code points of text in order, for a
  range-for (`for (u32 c : utf8CodePoints(text))`). Bytes that are not UTF-8 come out as U+FFFD,
  as utf8Decode decodes them. Its iterators also have `.position()`, the byte offset of the
  current code point, and `.valid()`. They are forward iterators, invalidated when the text's
  bytes change.
- `ContainerSize utf8Length(StringView text)`: how many code points text has. A maximal subpart that
  is not UTF-8 counts as one, as utf8CodePoints gives it.
- `ContainerSize utf8Encode(u32 codePoint, char* out)`: writes the shortest UTF-8 form of a scalar
  value into out (room for 4 bytes) and returns how many bytes it wrote (1 to 4).
- `void appendUtf8(String& out, u32 codePoint)`: appends that form to out.
- `bool isScalarValue(u32 codePoint)`.

utf8Encode and appendUtf8 throw InvalidArgument("not a Unicode scalar value") for a code point that
is not a scalar value: a surrogate, or past U+10FFFF. Nothing is written then; they never write a
replacement themselves. (A caller with an unpaired surrogate from JSON's `\uD800` decides what it
means and passes U+FFFD itself.)

## Complexity

Each function is linear in the bytes it reads. utf8Decode and utf8DecodeBefore read at most 4,
and the iterator's step reads the bytes of one code point. Nothing allocates, except appendUtf8 as
String's append does.
