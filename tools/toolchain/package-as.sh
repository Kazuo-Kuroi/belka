#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=$repo/build/native-compiler-sdk
mkdir -p "$out/usr/bin" "$out/usr/share/licenses/binutils" \
    "$out/usr/src/toolchain"
cp "$repo/build/toolchain-native-binutils/gas/as-new" "$out/usr/bin/as"
cp "$repo/build/toolchain-src/binutils-2.43/"COPYING* \
    "$out/usr/share/licenses/binutils/"
cp -R "$repo/contrib/binutils" "$out/usr/src/toolchain/"
cp "$repo/tools/toolchain/sources" "$out/usr/src/toolchain/"
touch "$out/assembler.stamp"
