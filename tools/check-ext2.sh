#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-08
# SPDX-License-Identifier: MIT
#
# After a test boot, check with debugfs that each ext2 image holds the
# files the kernel self-tests left there for the host to read back.
#
# usage: check-ext2.sh debugfs image...
#

set -eu

if [ $# -lt 2 ]; then
        echo "usage: $0 debugfs image..." >&2
        exit 1
fi
debugfs=$1
shift

for img in "$@"; do
        got=$("$debugfs" -R 'cat /persist.txt' "$img" 2>/dev/null)
        if [ "$got" != "written by belka" ]; then
                echo "ext2: $img: /persist.txt holds \"$got\"" >&2
                exit 1
        fi
        got=$("$debugfs" -R 'cat /pdir/sub/inner.txt' "$img" 2>/dev/null)
        if [ "$got" != "inner" ]; then
                echo "ext2: $img: /pdir/sub/inner.txt holds \"$got\"" >&2
                exit 1
        fi
        got=$("$debugfs" -R 'cat /many/zz-belka' "$img" 2>/dev/null)
        if [ "$got" != "kept" ]; then
                echo "ext2: $img: /many/zz-belka holds \"$got\"" >&2
                exit 1
        fi
done
echo "ext2: host fsck clean, data written by the kernel present"
