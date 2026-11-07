#!/bin/sh
# The bundled toolchain for a Linux host (design.md 17; llvm-mingw.ps1 is Windows'): llvm-mingw, clang
# and lld of LLVM 19.1.7 with MinGW-w64's headers and import libraries for the UCRT, built to run on
# Linux (glibc 2.31 or later), so that kpp on Linux builds Windows programs in the GNU dialect
# (kpp --target windows-gnu) with nothing of Visual Studio's. The same release as llvm-mingw.ps1's.
# Downloads the pinned archive once, checks its SHA-256, unpacks it into the output directory and
# prints the toolchain's root (bin/clang under it), which kpp takes as KPP_CLANG=ROOT/bin/clang, or
# as an installation's own toolchain at bin/ beside kpp.
#   sh tools/sysroot/llvm-mingw.sh [OUT]   (OUT: build/llvm-mingw-linux in the repository)
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../build/llvm-mingw-linux}
name=llvm-mingw-20250114-ucrt-ubuntu-20.04-x86_64
url=https://github.com/mstorsjo/llvm-mingw/releases/download/20250114/$name.tar.xz
sha256=a16f52dee819797248e6c7d63b8b1e50a92119f45767ecd8e9633d1733b896e2
mkdir -p "$out"
out=$(cd "$out" && pwd)
root=$out/$name
if [ -x "$root/bin/clang" ]; then echo "$root"; exit 0; fi
archive=$out/$name.tar.xz
if [ ! -f "$archive" ] || [ "$(sha256sum "$archive" | cut -d' ' -f1)" != "$sha256" ]; then
  curl -fsSL -o "$archive" "$url"
fi
got=$(sha256sum "$archive" | cut -d' ' -f1)
if [ "$got" != "$sha256" ]; then echo "$archive has SHA-256 $got, not the pinned $sha256" >&2; exit 1; fi
tar xJf "$archive" -C "$out"
rm "$archive"
if [ ! -x "$root/bin/clang" ]; then echo "$archive held no $name/bin/clang" >&2; exit 1; fi
echo "$root"
