#!/bin/sh
# ThreadSanitizer's and AddressSanitizer's runtimes for Linux x86-64, LLVM 19.1.7's, on a Linux host (llvm-linux.ps1
# is Windows' and says why kpp brings them): the archive streamed through sha256sum, xz and tar, with nothing of
# it written but the six files, each checked against its pinned SHA-256, into OUT, and OUT printed: kpp's
# place for them, lib/clang/19/lib/x86_64-unknown-linux-gnu beside kpp (compiler/build's for a build of kpp,
# a release's, which tools/release/package-linux.sh copies). Needs curl, xz and GNU tar.
#   sh tools/sysroot/llvm-linux.sh [OUT]   (OUT: compiler/build/lib/clang/19/lib/x86_64-unknown-linux-gnu)
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../compiler/build/lib/clang/19/lib/x86_64-unknown-linux-gnu}
name=LLVM-19.1.7-Linux-X64
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/$name.tar.xz
sha256=4a5ec53951a584ed36f80240f6fbf8fdd46b4cf6c7ee87cc2d5018dc37caf679
runtimes="libclang_rt.tsan.a=c31e2457e7389691bee42a04637d8b69211ea9f88471903b671c6cf45abd9ed3
libclang_rt.tsan.a.syms=717915ff75e775a40e2dece6c60a06845e7b190da4e2ffb35c1eb86f5d7a3256
libclang_rt.asan.a=fdebdb409e72f542be566fe47806612f8648dbc466eda2f97041effa76e1187a
libclang_rt.asan.a.syms=8ab0595dcef4f27cc853c7e44afbeeb7e837e70544365b1956ad445d268791b8
libclang_rt.asan_static.a=88e6b3165ba95eebc7577e913b9367987a9958a477d7ef3f47a0df1c696b4b13
libclang_rt.asan-preinit.a=668906f73a80793641f8cfd14e767ad0e47eaa4e037743ac253d9bc4503b3dab"
mkdir -p "$out"
out=$(cd "$out" && pwd)
complete() # every file there with its pinned SHA-256
{
  for pair in $runtimes; do
    [ -f "$1/${pair%%=*}" ] && [ "$(sha256sum "$1/${pair%%=*}" | cut -d' ' -f1)" = "${pair#*=}" ] || return 1
  done
}
if complete "$out"; then echo "$out"; exit 0; fi
# (in the system's temporary directory: a FIFO cannot be made on every file system, WSL's /mnt/c not)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
prefix=$name/lib/clang/19/lib/x86_64-unknown-linux-gnu
members=
for pair in $runtimes; do members="$members $prefix/${pair%%=*}"; done
# (the archive's SHA-256 from the same stream as the files: a file out of an archive that is not the pinned one is
# never moved into place)
mkfifo "$work/archive.pipe"
curl -fsSL "$url" | tee "$work/archive.pipe" | xz -dc | tar -xf - -C "$work" $members &
got=$(sha256sum < "$work/archive.pipe" | cut -d' ' -f1)
wait $! || { echo "downloading or unpacking $url failed" >&2; exit 1; }
if [ "$got" != "$sha256" ]; then echo "$url had SHA-256 $got, not the pinned $sha256" >&2; exit 1; fi
if ! complete "$work/$prefix"; then echo "$name.tar.xz gave other files than the pinned ones" >&2; exit 1; fi
for pair in $runtimes; do mv -f "$work/$prefix/${pair%%=*}" "$out/"; done
echo "$out"
