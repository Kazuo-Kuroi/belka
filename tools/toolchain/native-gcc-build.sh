#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
# Build a native compiler without replacing the installed bootstrap compiler.
set -eu
work=${1:-/var/tmp/gcc-bootstrap}
jobs=${JOBS:-1}
mode=${MODE:-bootstrap-lean}
export ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnp=no ac_cv_func_sbrk=no
export CONFIG_SHELL=/bin/sh SHELL=/bin/sh
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
if [ ! -f "$work/.source-extracted" ]; then
        tar -xzf /usr/src/gcc-bootstrap/source.tar.gz -C "$work"
        touch "$work/.source-extracted"
fi
mkdir -p "$work/build"
cd "$work/build"
if [ ! -f Makefile ]; then
        CC=gcc CXX=g++ CFLAGS=-O2 CXXFLAGS=-O2 \
            sh "$work/gcc-14.2.0/configure" \
            --build=x86_64-unknown-belka --host=x86_64-unknown-belka \
            --target=x86_64-unknown-belka --prefix=/usr --with-sysroot=/ \
            --with-as=/usr/bin/as --with-ld=/usr/bin/ld \
            --enable-languages=c,c++ --enable-bootstrap --disable-multilib \
            --disable-shared --disable-nls --disable-lto --disable-libsanitizer \
            --disable-libquadmath --disable-libgomp --disable-libatomic \
            --disable-libssp --disable-libvtv --disable-libcc1 --disable-plugin \
            --without-isl --enable-threads=posix --enable-initfini-array \
            --enable-checking=release
fi
case $mode in
configure) exit 0 ;;
bootstrap-lean|all-stage1|all-stage2|all-stage3) ;;
*) echo "unsupported GCC build mode: $mode" >&2; exit 1 ;;
esac
gmake -j "$jobs" "$mode" MAKEINFO=true STAGE1_CFLAGS=-O2 BOOT_CFLAGS=-O2
if [ "$mode" = bootstrap-lean ]; then
        gmake install DESTDIR="$work/install" MAKEINFO=true
fi
