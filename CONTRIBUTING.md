# Contributing to kpp

kpp is under the Apache License 2.0 with LLVM Exceptions ([LICENSE](LICENSE)): the compiler, the
standard library, projectlib, the tools, kppnet, the documentation and the official libraries in
`libraries/`.

## What a contribution may contain

A contribution is licensed under kpp's license (LICENSE), as GitHub's Terms of Service make every
contribution to a repository: there is no sign-off and no contributor license agreement.

- Code from elsewhere keeps its copyright and license notice in the file, and the commit message
  names where it came from and its license. Only licenses compatible with Apache 2.0 (MIT, BSD,
  zlib, Apache 2.0, public domain or equivalent): nothing under the GPL or another copyleft
  license, and nothing from sources whose license does not allow it.
- Code written with an AI assistant is fine; the assistant is named in a `Co-Authored-By` line.
- Factorio's source, and anything derived from it, is not open source and is never contributed
  here (see LICENSE, "Software from third parties").

## How contributions arrive

The repositories' home is kpp.net; GitHub mirrors them and takes pull requests and issues
([docs/library-registry.md](docs/library-registry.md), "Hosting on kpp.net"). A maintainer merges
a pull request by pushing it to kpp.net, and the mirror then shows it as merged.

Before a pull request: `compiler\build.cmd`, then `compiler\tests\run_tests.cmd` and
`compiler\tests\spec.cmd` pass. A language feature or fix starts with a snippet in
`compiler/tests/spec/` (see CLAUDE.md, "Rules").
