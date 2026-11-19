#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
mkdir -p "$repo/build/toolchain-cross-gcc"
rt=$repo/build/toolchain-cross/lib/gcc/x86_64-unknown-belka/14.2.0
mkdir -p "$rt"
# Keep mlibc's array boundaries alongside GCC's exception registration CRT.
gcc -DBELKA_CRT_ARRAYS_ONLY -c "$repo/usr/lib/crt/crtbegin.S" \
    -o "$rt/belka-crtbegin.o.tmp"
gcc -c "$repo/usr/lib/crt/crtend.S" \
    -o "$rt/belka-crtend.o.tmp"
mv "$rt/belka-crtbegin.o.tmp" "$rt/belka-crtbegin.o"
mv "$rt/belka-crtend.o.tmp" "$rt/belka-crtend.o"
cd "$repo/build/toolchain-cross-gcc"
"$repo/build/toolchain-src/gcc-14.2.0/configure" \
    --target=x86_64-unknown-belka --prefix="$repo/build/toolchain-cross" \
    --with-sysroot="$repo/build/sysroot-gcc" --with-as="$(command -v as)" \
    --with-ld="$repo/build/tools/illumos-ld/ld" \
    --enable-languages=c,c++ --disable-bootstrap --disable-multilib \
    --disable-shared --disable-nls --disable-lto --disable-libsanitizer \
    --disable-libquadmath --disable-libgomp --disable-libatomic \
    --disable-libssp --disable-libvtv --without-isl --enable-threads=posix \
    --enable-initfini-array
${MAKE:-make} -j"${JOBS:-8}" all-gcc
${MAKE:-make} install-gcc
