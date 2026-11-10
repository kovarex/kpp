# base64

Base64 and base64url ([RFC 4648](https://www.rfc-editor.org/rfc/rfc4648) sections 4 and 5) for
kpp. Decoding is strict, so one byte string has exactly one encoding that decodes.

```cpp
std::String text = base64Encode("foobar");      // Zm9vYmFy
std::String bytes = base64Decode("Zm9vYg==");   // foob
std::String token = base64UrlEncode(random32);  // '-' and '_', no padding
```

## API

| | |
|---|---|
| `String base64Encode(StringView data)` | `+` and `/`, padded with `=` |
| `String base64UrlEncode(StringView data, bool pad = false)` | `-` and `_`, unpadded unless `pad` |
| `String base64Decode(StringView text)` | padded base64; anything else throws `Base64Error` |
| `String base64UrlDecode(StringView text)` | base64url, padded or not |
| `bool tryBase64Decode(StringView, String& out)`, `tryBase64UrlDecode` | false instead of a throw |

`Base64Error` has `offset`, the byte where the problem is, and a message that starts with it:
`4: a character that is not base64`.

## What decoding rejects

- characters outside the alphabet, whitespace and line breaks included (RFC 4648 3.3);
- a base64 text whose length is not a multiple of 4: padding is required. base64url takes text
  with or without it, but padded text must be complete;
- `=` anywhere but the end, or the wrong number of them;
- a last character with bits set that no byte uses (`Zh==` for `f`), which RFC 4648 3.5 allows a
  decoder to reject: without the rule, several texts would decode to the same bytes.

## Tests

`kpp --test` in this directory, or `kpp --test libraries/base64` anywhere: RFC 4648's section 10
vectors in both alphabets, padded and not; the 256 byte values and a 40-byte pattern against Python's
`base64` module; round trips of every length from 0 to 299; each rejection above, with its message.
