# The libraries on FreeBSD and Linux

How far the official libraries and kppsite (kppnet/site) are from running on FreeBSD 14.3 (kpp.net's
host) and Linux, 2026-11-04, kppc at master 6167873d (stage 3's exceptions and stage 4's POSIX
layer in). Each project's tests were built with `kpp --no-server --target T --sysroot S --test` in
its directory (the sysroots of tools\sysroot\freebsd.ps1 and linux-from-wsl.sh) and each test then
run on its own (`--list`, then `--filter NAME`): Linux in WSL (Ubuntu 24.04, glibc 2.39), FreeBSD in
the CI FreeBSD 14.3 VM (vmactions/freebsd-vm, as ci.yml's freebsd_run; run 41825053585 of a
temporary branch, deleted since).

The two targets give the same result, test for test: 11 of the 12 projects build, and all 67 of
their tests pass. httpclient does not build.

| Project | Builds | Tests on Linux and FreeBSD |
|---|---|---|
| json | yes | 13 pass |
| sha256 | yes | 6 pass |
| base64 | yes | 3 pass |
| html | yes | 6 pass |
| url | yes | 6 pass |
| markdown | yes | 10 pass |
| bcrypt | yes | 5 pass |
| net | yes | 3 pass |
| mariadb | yes | 7 pass (the live server test skipped: MARIADB_TEST_PORT not set) |
| http | yes | 5 pass |
| httpclient | no: a compile error, on purpose | - |
| kppsite | yes | 3 pass |

**httpclient (the library; G).** It has no client off Windows yet: HttpClientPosix.kpp is a
static_assert until one is written and tested (README.md).

On 2026-11-03 http and mariadb did not link (the stdlib's threads and system error text called
Win32, stage 4) and 12 tests ended at a throw (stage 3); both are in.
