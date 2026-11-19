#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
set -eu
# Keep native tools on fork/exec; posix_spawn is not part of this port.
export ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnp=no
export ac_cv_func_sbrk=no
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cross=$repo/build/toolchain-cross
export PATH="$cross/bin:$PATH"
mkdir -p "$repo/build/toolchain-native-gcc" "$cross/x86_64-unknown-belka/bin"
ln -sf "$(command -v as)" "$cross/x86_64-unknown-belka/bin/as"
ln -sf "$repo/build/tools/illumos-ld/ld" "$cross/x86_64-unknown-belka/bin/ld"
cd "$repo/build/toolchain-native-gcc"
CC="$cross/bin/x86_64-unknown-belka-gcc" \
CXX="$cross/bin/x86_64-unknown-belka-g++" \
CC_FOR_BUILD=gcc CXX_FOR_BUILD=g++ AR=ar RANLIB=ranlib NM=nm \
CFLAGS=-O2 CXXFLAGS=-O2 \
    "$repo/build/toolchain-src/gcc-14.2.0/configure" \
    --build="$(gcc -dumpmachine)" --host=x86_64-unknown-belka \
    --target=x86_64-unknown-belka --prefix=/usr --with-sysroot=/ \
    --with-build-sysroot="$repo/build/sysroot-gcc" \
    --with-build-time-tools="$cross/x86_64-unknown-belka/bin" \
    --with-as=/usr/bin/as --with-ld=/usr/bin/ld \
    --enable-languages=c,c++ --disable-bootstrap --disable-multilib \
    --disable-shared --disable-nls --disable-lto --disable-libsanitizer \
    --disable-libquadmath --disable-libgomp --disable-libatomic \
    --disable-libssp --disable-libvtv --disable-libcc1 --disable-plugin \
    --without-isl --enable-threads=posix --enable-initfini-array \
    --enable-checking=release
${MAKE:-make} -j"${JOBS:-8}" all-gcc AR=ar RANLIB=ranlib NM=nm MAKEINFO=true
${MAKE:-make} install-gcc DESTDIR="$repo/build/native-gcc-install" \
    AR=ar RANLIB=ranlib NM=nm MAKEINFO=true
