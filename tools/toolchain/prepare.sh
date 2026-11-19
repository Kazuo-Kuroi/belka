#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
# Fetch pinned source archives, verify them, and apply the Belka port.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cache=$repo/build/distfiles
sources=${TOOLCHAIN_SOURCE_DIR:-$repo/build/toolchain-src}
mkdir -p "$cache" "$sources"
while read -r digest archive url; do
        case $digest in ''|'#'*) continue ;; esac
        if [ ! -f "$cache/$archive" ]; then
                curl --fail --location --connect-timeout 15 --max-time 300 \
                    --output "$cache/$archive.tmp" "$url"
                mv "$cache/$archive.tmp" "$cache/$archive"
        fi
        printf '%s  %s\n' "$digest" "$cache/$archive" | sha256sum -c -
done < "$repo/tools/toolchain/sources"
# The caller serializes source preparation against builds using these trees.
for source in gcc-14.2.0 gmp-6.2.1 mpfr-4.1.0 mpc-1.2.1 binutils-2.43; do
        rm -rf "$sources/$source"
        case $source in
        gcc-*|binutils-*) suffix=tar.xz ;;
        gmp-*|mpfr-*) suffix=tar.bz2 ;;
        mpc-*) suffix=tar.gz ;;
        esac
        tar -xf "$cache/$source.$suffix" -C "$sources"
done
for port_patch in "$repo/contrib/gcc/patches/"*.patch; do
        patch -d "$sources/gcc-14.2.0" -p1 < "$port_patch"
done
cp "$repo/contrib/gcc/belka.h" "$sources/gcc-14.2.0/gcc/config/belka.h"
patch -d "$sources/binutils-2.43" -p1 < "$repo/contrib/binutils/patches/0001-belka-target.patch"
for dep in gmp-6.2.1 mpfr-4.1.0 mpc-1.2.1; do
        ln -s "../$dep" "$sources/gcc-14.2.0/${dep%%-*}"
        for config in config.sub config.guess; do
                find "$sources/$dep" -type f -name "$config" \
                    -exec cp "$sources/gcc-14.2.0/$config" {} \;
        done
done
touch "$sources/prepared.stamp"
