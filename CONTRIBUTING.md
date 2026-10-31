# Contributing to Kpp

Kpp is under the Apache License 2.0 with LLVM Exceptions ([LICENSE](LICENSE)): the compiler, the
standard library, projectlib, the tools, kppnet, the documentation and the official libraries in
`libraries/`. A contribution is accepted under the same license.

## Sign-off (Developer Certificate of Origin)

Every commit carries a `Signed-off-by` line with the author's real name and e-mail:

```
Signed-off-by: Jane Doe <jane@example.com>
```

`git commit -s` adds it from `user.name` and `user.email`. With that line the author certifies the
Developer Certificate of Origin below, which is what keeps every line of Kpp licensable under
LICENSE: either it is the author's own work, or it comes from code under a compatible open
source license that the author may submit. There is no contributor license agreement to sign.

- A pull request whose commits lack the line is not merged until they have it
  (`git rebase --signoff` adds it to a branch's commits).
- Code from elsewhere keeps its copyright and license notice in the file, and the commit message
  names where it came from and its license. Only licenses compatible with Apache 2.0 (MIT, BSD,
  zlib, Apache 2.0, public domain or equivalent): nothing under the GPL or another copyleft
  license, and nothing from sources whose license does not allow it.
- Code written with an AI assistant is fine; the person who signs off certifies it as above, and
  the assistant is named in a `Co-Authored-By` line.
- Factorio's source, and anything derived from it, is not open source and is never contributed
  here (see LICENSE, "Software from third parties").

## How contributions arrive

The repositories' home is kpp.net; GitHub mirrors them and takes pull requests and issues
([docs/library-registry.md](docs/library-registry.md), "Hosting on kpp.net"). A maintainer merges
a pull request by pushing it to kpp.net, and the mirror then shows it as merged.

Before a pull request: `compiler\build.cmd`, then `compiler\tests\run_tests.cmd` and
`compiler\tests\spec.cmd` pass. A language feature or fix starts with a snippet in
`compiler/tests/spec/` (see CLAUDE.md, "Rules").

## The certificate

As published at https://developercertificate.org/:

```
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```
