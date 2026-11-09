#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-08
# SPDX-License-Identifier: MIT
#
# Build a fresh ext2 test image with mke2fs, populated with the files
# the kernel self-tests expect:
#
#   /hello.txt          "Hello from mke2fs\n"
#   /big.bin            300000 bytes, byte i = (i * 7 + 3) mod 95 + 32
#   /dir/sub/deep.txt   "deep\n"
#   /link               -> hello.txt            (fast symlink)
#   /longlink           -> a 100-byte target    (slow symlink)
#   /many/f000..f199    each holding its own name, hash-indexed
#   /bin/*              the files of each bindir given (user test programs)
#
# usage: mkext2.sh image blocksize [bindir...]
#

set -eu

if [ $# -lt 2 ]; then
        echo "usage: $0 image blocksize [bindir...]" >&2
        exit 1
fi
img=$1
bsize=$2
shift 2
MKE2FS=${MKE2FS:-/sbin/mke2fs}
E2FSCK=${E2FSCK:-/sbin/e2fsck}

src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT

printf 'Hello from mke2fs\n' > "$src/hello.txt"
awk 'BEGIN { for (i = 0; i < 300000; i++) printf "%c", (i * 7 + 3) % 95 + 32 }' \
    > "$src/big.bin"
mkdir -p "$src/dir/sub" "$src/many"
printf 'deep\n' > "$src/dir/sub/deep.txt"
ln -s hello.txt "$src/link"
ln -s "$(printf '%0100d' 0 | tr 0 x)" "$src/longlink"
for bindir in "$@"; do
        mkdir -p "$src/bin"
        cp "$bindir"/* "$src/bin/"
done
i=0
while [ $i -lt 200 ]; do
        n=$(printf 'f%03d' $i)
        printf '%s' "$n" > "$src/many/$n"
        i=$((i + 1))
done

rm -f "$img"
"$MKE2FS" -q -F -t ext2 -b "$bsize" -N 1024 -L belkatest -d "$src" \
    "$img" 8M
# mke2fs -d does not index directories; e2fsck -D does, so /many becomes
# a hash-indexed directory. e2fsck exits 1 when it changed something.
"$E2FSCK" -fyD "$img" >/dev/null 2>&1 || [ $? -eq 1 ]
