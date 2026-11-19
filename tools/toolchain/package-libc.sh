#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
# An offline source kit: subprojects are pinned by mlibc's wrap files.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=$repo/build/native-libc-sdk
stage=$(mktemp -d "$repo/build/native-libc-sdk.XXXXXX")
trap 'rm -rf "$stage"' 0
mkdir -p "$stage/usr/src/mlibc-build"
for project in frigg freestnd-c-hdrs freestnd-cxx-hdrs libsmarter; do
        expected=$(sed -n 's/^revision = //p' "$repo/contrib/mlibc/subprojects/$project.wrap")
        actual=$(git -C "$repo/contrib/mlibc/subprojects/$project" rev-parse HEAD)
        test "$expected" = "$actual" || {
                echo "mlibc subproject $project is not at its pinned revision" >&2
                exit 1
        }
done
tar -czf "$stage/usr/src/mlibc.tar.gz" --exclude=.git \
    --exclude=.meson-subproject-wrap-hash.txt -C "$repo/contrib" mlibc
cp "$repo/tools/ld/illumos-cc" "$stage/usr/src/mlibc-build/"
cp "$repo/tools/mlibc/native-static.ini" "$repo/tools/mlibc/native-shared.ini" \
    "$repo/tools/mlibc/native-build.sh" "$repo/tools/mlibc/native-check.sh" \
    "$stage/usr/src/mlibc-build/"
{ git -C "$repo/contrib/mlibc" rev-parse HEAD
  for project in frigg freestnd-c-hdrs freestnd-cxx-hdrs libsmarter; do
        printf '%s ' "$project"
        git -C "$repo/contrib/mlibc/subprojects/$project" rev-parse HEAD
  done
} > "$stage/usr/src/mlibc-build/REVISIONS"
mkdir -p "$stage/usr/src/mlibc-build/tests"
cp "$repo/usr/tests/libctest.c" "$repo/usr/tests/hello.c" \
    "$stage/usr/src/mlibc-build/tests/"
touch "$stage/libc.stamp"
rm -rf "$out"
mv "$stage" "$out"
