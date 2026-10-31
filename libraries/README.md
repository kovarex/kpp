# Libraries

Kpp libraries written for the registry (docs/library-registry.md): first the ones that the
kpp.net website and registry need, in the order of "Hosting on kpp.net". Each directory is a
library project:

- `build.kpp`: the project (its tests as an executable, until projectlib has `project.library()`)
  and, in its comment, the registry fields;
- `src/`: the library, which is what a release contains, with `README.md`;
- `tests/`: native tests (`test "name" { check(...); }`, docs/design.md 8.6).

`kppc --test libraries/NAME` runs a library's tests (kppc leaves the directory's build.kpp out),
and `compiler\tests\run_tests.cmd` runs every library's.

| Library | What | Checked against |
|---|---|---|
| [json](json/README.md) | JSON parsing and writing, exact numbers | JSONTestSuite's 318 parsing cases; 200,000 random doubles round-trip |
