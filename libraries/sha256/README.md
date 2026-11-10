# sha256

SHA-256 ([FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final)) and HMAC-SHA-256
([RFC 2104](https://www.rfc-editor.org/rfc/rfc2104)) for kpp, with a constant-time compare for
MACs and tokens. The registry's tree hash (kppnet/registry/tree_hash.py) and its API tokens use
it.

```cpp
std::println(sha256("abc").hex());          // ba7816bf...f20015ad

Sha256 hash;                           // data in parts
hash.update(header);
hash.update(body);
Sha256Digest digest = hash.finish();

Sha256Digest mac = hmacSha256(secret, message);
if (Sha256::constantTimeEqual(mac.view(), given)) ...
```

## API

| | |
|---|---|
| `Sha256Digest sha256(StringView data)` | the digest of the bytes |
| `Sha256Digest hmacSha256(StringView key, StringView message)` | HMAC-SHA-256; a key over 64 bytes is hashed first, as RFC 2104 says |
| `Sha256` | `update(StringView)`, `update(const void*, i64)` any number of times, then `finish()` once; `reset()` starts again. Another `update` or `finish` after `finish` stops the program (`release_assert`) |
| `Sha256::constantTimeEqual(StringView a, StringView b)` | equal bytes, in a time that depends on the lengths only |
| `Sha256Digest` | `bytes[32]`; `hex()` (lowercase, 64 characters), `data()` (the 32 bytes as a String), `view()`; `==` compares in constant time; `str()` is `hex()` |

## Tests

`kpp --test` in this directory, or `kpp --test libraries/sha256` anywhere:
- the five examples of FIPS 180-2's appendix, the million `a` included;
- RFC 4231's seven HMAC-SHA-256 test cases (case 5 truncated to 128 bits, as it is there);
- messages of every length from 0 to 299 bytes, HMACs over 20 key lengths and 11 message lengths
  (block boundaries and keys longer than a block), against Python's `hashlib` and `hmac`
  (OpenSSL);
- data given in parts of 1 to 130 bytes, and a `Sha256` reused after `reset()`.

## Not yet

- Speed: one block at a time in plain kpp, no SHA-NI instructions.
