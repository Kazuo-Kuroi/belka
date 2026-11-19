#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
sources=$repo/build/gcc-bootstrap-src
out=$repo/build/native-gcc-source-sdk
stage=$(mktemp -d "$repo/build/native-gcc-source-sdk.XXXXXX")
trap 'rm -rf "$stage"' 0
mkdir -p "$stage/usr/src/gcc-bootstrap"
tar -czf "$stage/usr/src/gcc-bootstrap/source.tar.gz" -C "$sources" \
    gcc-14.2.0 gmp-6.2.1 mpfr-4.1.0 mpc-1.2.1
cp "$repo/tools/toolchain/sources" "$stage/usr/src/gcc-bootstrap/"
cp "$repo/tools/toolchain/native-gcc-build.sh" "$stage/usr/src/gcc-bootstrap/"
cp "$repo/tools/toolchain/native-gcc-check.sh" "$stage/usr/src/gcc-bootstrap/"
mkdir -p "$stage/usr/src/gcc-bootstrap/tests"
cp "$repo/usr/tests/compiler.sh" "$repo/usr/tests/exectest.c" \
    "$stage/usr/src/gcc-bootstrap/tests/"
cp -R "$repo/contrib/gcc" "$stage/usr/src/gcc-bootstrap/port"
touch "$stage/gcc-source.stamp"
rm -rf "$out"
mv "$stage" "$out"
