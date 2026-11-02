# Libraries

kpp libraries written for the registry (docs/library-registry.md): first the ones that the
kpp.net website and registry need, in the order of "Hosting on kpp.net". Each directory is a
library project:

- `build.kpp`: `project.library(NAME)` with its version, license and roots (docs/build.md,
  "kpp libraries");
- `src/`: the library, which is what a release contains, with `README.md`;
- `tests/`: native tests (`test "name" { check(...); }`, docs/design.md 8.6).

`kpp --test` in a library's directory runs its tests, as does `kppc --test libraries/NAME` (which
leaves the directory's build.kpp out); and `compiler\tests\run_tests.cmd` runs every library's.

| Library | What | Checked against |
|---|---|---|
| [json](json/README.md) | JSON parsing and writing, exact numbers | JSONTestSuite's 318 parsing cases; 200,000 random doubles round-trip |
| [sha256](sha256/README.md) | SHA-256, HMAC-SHA-256, constant-time compare | FIPS 180-2's examples, RFC 4231's HMAC cases, Python's hashlib |
| [base64](base64/README.md) | Base64 and base64url, strict decoding | RFC 4648's vectors, Python's base64 |
| [html](html/README.md) | escaping; templates escaped by context, checked at compile time | its own tests (no published suite) |
| [url](url/README.md) | URLs (RFC 3986): parsing, resolving, percent-encoding, query strings | RFC 3986's 42 resolution examples, Python's urllib.parse |
| [markdown](markdown/README.md) | CommonMark 0.31.2 to HTML, GitHub's tables and heading ids | all 655 CommonMark spec examples and the GFM spec's 8 table examples (conformance/run.cmd, fetched: CC-BY-SA) |
| [bcrypt](bcrypt/README.md) | the password hash of PHP's `password_hash()`: `$2y$`/`$2b$` hash and verify | crypt_blowfish's vectors, 38 hashes from PHP 8.3's `crypt()` |
| [net](net/README.md) | TCP connections and listeners (Winsock, BSD sockets) | loopback tests on Windows |

**License**: Apache-2.0 WITH LLVM-exception, as all of kpp ([LICENSE](../LICENSE),
[CONTRIBUTING.md](../CONTRIBUTING.md)); each build.kpp says so. Third-party code and data keep
their license: markdown is a port of commonmark.js (BSD-2-Clause) with mdurl's encoding (MIT), so
its expression names those too (their texts are next to it); JSONTestSuite's (MIT) in json's
tests; the CommonMark and GFM specs' examples (CC-BY-SA) are fetched by markdown's conformance
check, never committed.
