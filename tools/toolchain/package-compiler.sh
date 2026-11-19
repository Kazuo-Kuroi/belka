#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cross=$repo/build/toolchain-cross
native=$repo/build/toolchain-native-gcc
src=$repo/build/toolchain-src
out=$repo/build/native-compiler-sdk
stage=$(mktemp -d "$repo/build/native-compiler-sdk.XXXXXX")
trap 'rm -rf "$stage"' 0
cp -R "$repo/build/native-gcc-install/usr" "$stage/"
mkdir -p "$stage/usr/bin" "$stage/usr/lib64" "$stage/usr/src/toolchain" \
    "$stage/usr/share/licenses"
cp "$repo/build/toolchain-native-binutils/gas/as-new" "$stage/usr/bin/as"
ln -sf gcc "$stage/usr/bin/cc"
ln -sf g++ "$stage/usr/bin/c++"
rt=$stage/usr/lib/gcc/x86_64-unknown-belka/14.2.0
mkdir -p "$rt"
cp "$cross/lib/gcc/x86_64-unknown-belka/14.2.0/"*.a \
    "$cross/lib/gcc/x86_64-unknown-belka/14.2.0/"*.o "$rt/"
cp "$cross/x86_64-unknown-belka/lib/"*.a "$stage/usr/lib64/"
mkdir -p "$stage/usr/include/c++"
cp -R "$cross/x86_64-unknown-belka/include/c++/14.2.0" \
    "$stage/usr/include/c++/"
# mlibc implements these interfaces in libc; its compatibility DSOs let
# conventional compiler and configure command lines use their library names.
for lib in dl m pthread rt resolv util ssp ssp_nonshared; do
        cp "$repo/build/sysroot-gcc/usr/lib64/lib$lib.a" \
            "$repo/build/sysroot-gcc/usr/lib64/lib$lib.so" "$stage/usr/lib64/"
done
cp "$native/gmp/.libs/libgmp.a" "$native/mpfr/src/.libs/libmpfr.a" \
    "$native/mpc/src/.libs/libmpc.a" "$stage/usr/lib64/"
cp "$native/gmp/gmp.h" "$src/mpfr-4.1.0/src/mpfr.h" \
    "$src/mpc-1.2.1/src/mpc.h" "$stage/usr/include/"
# Only relocatable archives are stripped; leave illumos dynamic ELF intact.
strip --strip-debug "$stage/usr/lib64/"*.a "$rt/"*.a "$rt/"*.o
for project in gcc-14.2.0 binutils-2.43 gmp-6.2.1 mpfr-4.1.0 mpc-1.2.1; do
        mkdir -p "$stage/usr/share/licenses/$project"
        cp "$src/$project/"COPYING* "$stage/usr/share/licenses/$project/"
done
cp -R "$repo/contrib/gcc" "$repo/contrib/binutils" "$stage/usr/src/toolchain/"
cp "$repo/tools/toolchain/sources" "$stage/usr/src/toolchain/"
touch "$stage/assembler.stamp" "$stage/compiler.stamp"
rm -rf "$out"
mv "$stage" "$out"
