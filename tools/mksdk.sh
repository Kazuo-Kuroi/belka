#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-17
# SPDX-License-Identifier: MIT
# Stage headers, start files, and a native linker bootstrap kit.
set -eu
out=$1
sysroot=$2
native=$3
begin=$4
end=$5
rtlib=$6
rm -rf "$out"
mkdir -p "$out/usr/include" "$out/usr/lib64/illumos-ld" "$out/usr/src/illumos-ld"
cp -LR "$sysroot/usr/include/." "$out/usr/include/"
for object in crt1 crti crtn; do
        cp "$sysroot/usr/lib64/$object.o" "$out/usr/lib64/"
done
cp "$begin" "$out/usr/lib64/crtbegin.o"
cp "$end" "$out/usr/lib64/crtend.o"
cp "$sysroot/usr/lib64/libc.a" "$out/usr/lib64/"
cp "$rtlib" "$out/usr/lib64/libcompiler_rt.a"
ln -s libc.so.1 "$out/usr/lib64/libc.so"
for archive in libld liblddbg libelf libconv; do
        cp "$native/$archive.a" "$out/usr/lib64/illumos-ld/"
done
cp "$native/ld.d/ld.o" "$native/ld.d/msg.o" "$out/usr/lib64/illumos-ld/"
# Only archives and relocatable objects are safe to process with GNU strip.
${STRIP:-strip} --strip-debug "$out/usr/lib64/"*.a "$out/usr/lib64/"*.o \
    "$out/usr/lib64/illumos-ld/"*.a "$out/usr/lib64/illumos-ld/"*.o
cp -R contrib/illumos-ld/port contrib/illumos-ld/usr \
    contrib/illumos-ld/README.md "$out/usr/src/illumos-ld/"
cp usr/share/dev/relink.mk "$out/usr/src/illumos-ld/"
