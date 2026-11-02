# bcrypt

The bcrypt password hash ([Provos and Mazières, 1999](https://www.usenix.org/legacy/events/usenix99/provos/provos.pdf)),
in the form PHP's `password_hash()` writes by default: `$2y$10$` then 22 characters of salt and 31
of hash. Written for gorating.com's move from PHP to Kpp, whose users' passwords are such hashes.

```cpp
if (bcryptVerify(password, user.passwordHash))   // throws BcryptError on a hash it cannot read
  logIn(user);

String hash = bcryptHash(password, 10, salt);     // salt: 16 bytes from a secure random source
```

## API

| | |
|---|---|
| `String bcryptHash(StringView password, int cost, StringView salt)` | `$2y$` hash at 2^cost rounds (cost 4 to 31; PHP's default is 10); salt is 16 bytes |
| `bool bcryptVerify(StringView password, StringView hash)` | whether password matches; the hash comparison takes the same time wherever they differ |
| `int bcryptCost(StringView hash)` | the hash's cost, to decide whether to hash again with a higher one |
| `BcryptError` | a `RuntimeError`: an argument out of range, or a hash that is not one this library reads |

**Passwords.** The key is the password's bytes and a zero byte, at most 72 bytes: what is past
the 72nd byte does not count, as in every bcrypt. A password with a zero byte throws (C's
`crypt()` would cut it there, PHP 8.4 refuses it).

**Variants.** `$2y$` (PHP) and `$2b$` (OpenBSD since 2014) are the same algorithm and both
verify. `$2a$` verifies for passwords of ASCII bytes only: for other passwords its
implementations differ (crypt_blowfish's sign-extension countermeasure), so it throws rather than
guess. `$2x$` and anything else throw. The salt's 22nd character carries 2 bits; the other 4 are
ignored on reading, as PHP does.

**Salt.** The caller gives it, from a cryptographically secure generator. The standard library
has none yet (its `RandomDevice` is not one); `bcryptHash(password, cost)` comes when it does.

## Tests

`kpp --test` in this directory, or `kppc --test libraries/bcrypt` anywhere:
- Solar Designer's crypt_blowfish vectors (`U*U`, the 72-byte limit, the empty password), as
  `$2a$`, `$2b$` and `$2y$`, and a non-canonical last salt character;
- 38 hashes PHP 8.3.27's `crypt()` wrote (tests/BcryptPhpVectors.kpp, from
  `php generate_vectors.php`): passwords of 0 to 255 bytes, ASCII and any byte, costs 4 and 5;
  each verifies, is rewritten byte for byte by `bcryptHash` from its salt, and fails with a byte
  added;
- one cost-10 hash, PHP's default;
- the errors: bad variants, costs, characters, lengths, zero bytes, `$2a$` with non-ASCII bytes.

Blowfish's tables are pi's hexadecimal digits, computed by `python generate_tables.py`
(src/BlowfishTables.kpp).

**Speed** (one hash at cost 10, 10 verifications each, three interleaved rounds on PC-PRACOVNA):
Kpp `--release` 35, 39, 36 ms; Kpp debug 49, 48, 43 ms; PHP 8.3.27's `password_verify` 40, 44,
37 ms.
