#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
set -eu
# Keep native tools on fork/exec; posix_spawn is not part of this port.
export ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnp=no
export ac_cv_func_sbrk=no
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
mkdir -p "$repo/build/toolchain-native-binutils"
cd "$repo/build/toolchain-native-binutils"
CC="$repo/tools/ld/belka-cc ${HOSTCC:-gcc}" CXX=false CFLAGS=-O2 \
AR=ar RANLIB=ranlib NM=nm AR_FOR_TARGET=ar RANLIB_FOR_TARGET=ranlib \
    "$repo/build/toolchain-src/binutils-2.43/configure" \
    --build="$(gcc -dumpmachine)" --host=x86_64-unknown-belka \
    --target=x86_64-unknown-belka --prefix=/usr \
    --disable-gdb --disable-gdbserver --disable-gold --disable-ld \
    --disable-nls --disable-werror --disable-shared --enable-static \
    --without-zstd --without-debuginfod --without-zlib
${MAKE:-make} -j"${JOBS:-4}" all-gas MAKEINFO=true AR=ar RANLIB=ranlib \
    AR_FOR_TARGET=ar RANLIB_FOR_TARGET=ranlib
