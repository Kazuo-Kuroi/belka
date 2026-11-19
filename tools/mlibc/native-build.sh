#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
# Build and stage a native libc without replacing the running system's libc.
set -eu
work=${1:-/var/tmp/mlibc-native}
jobs=${JOBS:-2}
kit=/usr/src/mlibc-build
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
if [ ! -f "$work/.source-extracted" ]; then
        tar -xzf /usr/src/mlibc.tar.gz -C "$work"
        touch "$work/.source-extracted"
fi
libgcc=$(gcc -print-libgcc-file-name)
sed "s|@LIBGCC@|$libgcc|g" "$kit/native-shared.ini" > "$work/shared.ini"
for kind in static shared; do
        case $kind in
        static) machine=$kit/native-static.ini ;;
        shared) machine=$work/shared.ini ;;
        esac
        if [ ! -f "$work/$kind/build.ninja" ]; then
                muon -C "$work/mlibc" setup -pcross:"$machine" \
                    -Dprefix=/usr -Dlibdir=lib64 -Dbuildtype=release \
                    -Duse_freestnd_hdrs=enabled -Dlibgcc_dependency=false \
                    -Dsysdeps=belka -Ddefault_library_paths=/usr/lib64 \
                    -Dwrap_mode=nodownload -Ddefault_library="$kind" \
                    -Dsoversion=1 "$work/$kind"
        fi
        samu -C "$work/$kind" -j "$jobs"
        muon -C "$work/$kind" install -d "$work/sysroot"
done
printf 'native-mlibc: built and staged in %s/sysroot\n' "$work"
