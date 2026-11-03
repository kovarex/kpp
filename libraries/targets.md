# The libraries on FreeBSD and Linux

How far the official libraries and kppsite (kppnet/site) are from running on FreeBSD 14.3 (kpp.net's
host) and Linux, 2026-11-03, kppc at master 36d44645 (stage 3's exceptions in). Each project's tests
were built with `kpp --no-server --target T --sysroot S --test` in its directory (the sysroots of
tools\sysroot\freebsd.ps1 and linux-from-wsl.sh) and each test then run on its own (`--list`, then
`--filter NAME`): Linux in WSL (Ubuntu 24.04, glibc 2.39), FreeBSD in the CI FreeBSD 14.3 VM
(vmactions/freebsd-vm, as ci.yml's freebsd_run; run 41722631558 of a temporary branch, deleted
since).

The two targets give the same result, test for test: 9 of the 12 projects build, and all 55 of
their tests pass. The other three do not build.

| Project | Builds | Tests on Linux and FreeBSD | What is missing |
|---|---|---|---|
| json | yes | 13 pass | nothing |
| sha256 | yes | 6 pass | nothing |
| base64 | yes | 3 pass | nothing |
| html | yes | 6 pass | nothing |
| url | yes | 6 pass | nothing |
| markdown | yes | 10 pass | nothing |
| bcrypt | yes | 5 pass | nothing |
| net | yes | 3 pass | nothing |
| mariadb | no: the link | - | the stdlib's threads and system error text (stage 4) |
| http | no: the link | - | the stdlib's threads and system error text (stage 4) |
| httpclient | no: a compile error, on purpose | - | its own POSIX client (fetch(3) on FreeBSD, libcurl on Linux) |
| kppsite | yes | 3 pass | nothing |

## Each failure, its cause and its owner

- **stdlib/core/Thread.kpp calls Win32 (stage 4; C).** `Thread`'s constructor, `join` and
  `thisThread::getId` call CreateThread, WaitForSingleObject, CloseHandle and GetCurrentThreadId:
  undefined symbols in the link of http (its server) and mariadb (its tests' fake server).
- **stdlib/core/Error.kpp calls Win32 (stage 4; C).** `ErrorCode::message()` for the system
  category calls FormatMessageW and WideCharToMultiByte (strerror is the POSIX one): undefined in
  http's and mariadb's link.
- **httpclient has no client off Windows (the library; G).** HttpClientPosix.kpp is a
  static_assert until one is written and tested (README.md): fetch(3) from FreeBSD's base system,
  libcurl on Linux.
- **The report of an exception nobody caught prints freed memory (stage 3; I).** "unhandled
  exception of type RuntimeError: " is followed by the allocator's bytes when the message is longer
  than a small string (23 bytes): `throw RuntimeError(String("a message longer than twenty-three
  characters"))` built with --target linux; a 5-character message prints right. Only that report:
  the program aborts next. Seen on Linux; FreeBSD printed the same before stage 3.

kpp.net's site needs kppsite (ready on both) and, for the registry and the CGI programs, http and
mariadb: once C's threads and error text are in, their builds should link and their tests can run
(not run yet: neither has linked off Windows).
