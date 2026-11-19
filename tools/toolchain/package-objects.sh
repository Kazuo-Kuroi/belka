#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=$repo/build/native-object-sdk
stage=$(mktemp -d "$repo/build/native-object-sdk.XXXXXX")
trap 'rm -rf "$stage"' 0
mkdir -p "$stage/usr/bin" "$stage/usr/share/licenses/binutils"
for program in objcopy objdump readelf size strings; do
        cp "$repo/build/toolchain-native-binutils/binutils/$program" "$stage/usr/bin/"
done
cp "$repo/build/toolchain-native-binutils/binutils/strip-new" "$stage/usr/bin/strip"
cp "$repo/build/toolchain-src/binutils-2.43/"COPYING* "$stage/usr/share/licenses/binutils/"
touch "$stage/objects.stamp"
rm -rf "$out"
mv "$stage" "$out"
