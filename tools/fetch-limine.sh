#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Download a pinned Limine binary release and verify its checksum.
#
# usage: fetch-limine.sh version destdir
#

set -eu

if [ $# -ne 2 ]; then
        echo "usage: $0 version destdir" >&2
        exit 1
fi

version=$1
dest=$2

case $version in
11.2.1)
        sha256=6e8c300161e41445dda0d05dc86902e80e3aeb0578ac1f9ef6afec33f3f61ff1
        ;;
*)
        echo "$0: no checksum recorded for Limine $version" >&2
        exit 1
        ;;
esac

url="https://codeberg.org/Limine/Limine/archive/v${version}-binary.tar.gz"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "fetching $url"
curl -fsSL -o "$tmp/limine.tar.gz" "$url"

actual=$(sha256sum "$tmp/limine.tar.gz" | cut -d' ' -f1)
if [ "$actual" != "$sha256" ]; then
        echo "$0: checksum mismatch for Limine $version" >&2
        echo "  expected $sha256" >&2
        echo "  got      $actual" >&2
        exit 1
fi

tar -xzf "$tmp/limine.tar.gz" -C "$tmp"
rm -rf "$dest"
mkdir -p "$(dirname "$dest")"
mv "$tmp/limine" "$dest"
