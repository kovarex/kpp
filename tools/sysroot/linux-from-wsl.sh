#!/bin/sh
# A glibc x86-64 sysroot for kppc --target linux, copied from a Linux system (WSL's Ubuntu on
# Windows): the C headers, the C runtime's start files and libraries, and gcc's runtime (crtbegin,
# libgcc), with every symbolic link resolved (a Windows linker does not follow them).
# Usage: sh linux-from-wsl.sh <output directory>, for example
#   wsl -e sh tools/sysroot/linux-from-wsl.sh /mnt/c/kpp-sandbox/sysroots/linux-x86_64
set -e
out="$1"
[ -n "$out" ] || { echo "usage: $0 <output directory>" >&2; exit 2; }
arch=x86_64-linux-gnu
gcc=$(ls -d /usr/lib/gcc/$arch/* | while read d; do [ -f "$d/crtbegin.o" ] && echo "$d"; done | sort -V | tail -n 1)
[ -n "$gcc" ] || { echo "no gcc runtime (crtbegin.o) under /usr/lib/gcc/$arch" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out/usr/lib/$arch" "$out/lib/$arch" "$out/lib64" "$out/usr/include" "$out/$gcc"
# (On a case-insensitive file system, as under /mnt/c, a few of Linux's netfilter headers differ
# from others only in case: those are skipped, and counted.)
clashes=$(cp -rL /usr/include/. "$out/usr/include/" 2>&1 | grep -c 'File exists' || true)
[ "$clashes" = 0 ] || echo "usr/include: $clashes header(s) that differ from others only in case skipped"
for f in /usr/lib/$arch/*crt1.o /usr/lib/$arch/crti.o /usr/lib/$arch/crtn.o \
         /usr/lib/$arch/libc.so /usr/lib/$arch/libc.so.6 /usr/lib/$arch/libc_nonshared.a /usr/lib/$arch/libc.a \
         /usr/lib/$arch/libm.so /usr/lib/$arch/libm.so.6 /usr/lib/$arch/libm.a /usr/lib/$arch/libm-*.a \
         /usr/lib/$arch/libmvec.so /usr/lib/$arch/libmvec.so.1 /usr/lib/$arch/libmvec.a \
         /usr/lib/$arch/libpthread.a /usr/lib/$arch/libpthread.so.0 /usr/lib/$arch/libdl.a /usr/lib/$arch/libdl.so.2 \
         /usr/lib/$arch/librt.a /usr/lib/$arch/librt.so.1 /usr/lib/$arch/libresolv.a /usr/lib/$arch/libresolv.so /usr/lib/$arch/libresolv.so.2 \
         /usr/lib/$arch/ld-linux-x86-64.so.2 /usr/lib/$arch/libgcc_s.so.1; do
  [ -e "$f" ] && cp -L "$f" "$out/usr/lib/$arch/"
done
# libc.so and libm.so (linker scripts) name /lib/$arch/libc.so.6, libm.so.6, libmvec.so.1 and
# /lib64/ld-linux-x86-64.so.2, read inside the sysroot: /lib is /usr/lib on a merged-/usr system.
cp "$out/usr/lib/$arch/"*.so.* "$out/lib/$arch/"
cp -L /usr/lib/$arch/ld-linux-x86-64.so.2 "$out/lib64/"
for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so; do
  [ -e "$gcc/$f" ] && cp -L "$gcc/$f" "$out/$gcc/"
done
echo "sysroot: $out (gcc runtime $gcc, $(ldd --version | head -n 1))"
