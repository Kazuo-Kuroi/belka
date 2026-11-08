#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Build a UEFI-bootable ISO image holding Limine and the kernel.
#
# usage: mkiso.sh kernel limine-dir limine.conf output.iso
#

set -eu

if [ $# -ne 4 ]; then
        echo "usage: $0 kernel limine-dir limine.conf output.iso" >&2
        exit 1
fi

kernel=$1
limine=$2
conf=$3
out=$4

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/boot/limine" "$root/EFI/BOOT"
cp "$kernel" "$root/boot/belka"
cp "$conf" "$root/boot/limine/limine.conf"
cp "$limine/limine-uefi-cd.bin" "$root/boot/limine/"
cp "$limine/BOOTX64.EFI" "$root/EFI/BOOT/"

xorriso -as mkisofs -quiet -R -r -J \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$root" -o "$out"
