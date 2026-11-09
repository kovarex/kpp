# rpmalloc 1.4.5

kpp's allocator (docs/build.md, "The allocator"): malloc, free and the rest for the whole kpp
process, compiled into compiler/src/allocator.c.

- Source: https://github.com/mjansson/rpmalloc, tag 1.4.5 (2024-04-01), from
  https://github.com/mjansson/rpmalloc/archive/refs/tags/1.4.5.tar.gz, SHA-256
  2513626697ef72a60957acc8caed17c39931a55c1a49202707de195742683d69.
- Kept unchanged: rpmalloc/rpmalloc.c, rpmalloc/rpmalloc.h, rpmalloc/malloc.c and LICENSE, the
  archive's own files byte for byte. The rest of the archive (tests, build scripts) is not here.
- Licence: public domain (the Unlicense), or the MIT License where public domain is not
  recognised (LICENSE). kpp takes it as public domain, which asks for no notice.
- The same version is in LLVM 19's lib/Support/rpmalloc (clang-formatted, unchanged otherwise),
  and LLVM's Windows release links it into LLVMSupport.lib. That copy gave kpp built with LLVM
  this allocator by accident before 2026-11-09.

To update: replace the four files with a new release's, change the version here, in allocator.c's
kppAllocatorName and in docs/build.md, then run the gate and `kpp measure`.
