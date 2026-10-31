# Libraries

Kpp libraries written for the registry (docs/library-registry.md): first the ones that the
kpp.net website and registry need, in the order of "Hosting on kpp.net". Each directory is a
library project:

- `build.kpp`: `project.library(NAME)` with its version, license and roots (docs/build.md,
  "Kpp libraries");
- `src/`: the library, which is what a release contains, with `README.md`;
- `tests/`: native tests (`test "name" { check(...); }`, docs/design.md 8.6).

`kpp --test` in a library's directory runs its tests, as does `kppc --test libraries/NAME` (which
leaves the directory's build.kpp out); and `compiler\tests\run_tests.cmd` runs every library's.

| Library | What | Checked against |
|---|---|---|
| [json](json/README.md) | JSON parsing and writing, exact numbers | JSONTestSuite's 318 parsing cases; 200,000 random doubles round-trip |

**License**: every library's build.kpp says MIT, as a placeholder: the license of kovarex's
libraries is his to decide (asked 2026-11-01).
