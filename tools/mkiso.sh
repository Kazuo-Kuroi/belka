#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Build a UEFI-bootable ISO image holding Limine and the kernel, with a
# Limine configuration generated for the given menu timeout and kernel
# command line.
#
# usage: mkiso.sh kernel limine-dir timeout cmdline output.iso
#

set -eu

if [ $# -ne 5 ]; then
        echo "usage: $0 kernel limine-dir timeout cmdline output.iso" >&2
        exit 1
fi

kernel=$1
limine=$2
timeout=$3
cmdline=$4
out=$5

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/boot/limine" "$root/EFI/BOOT"
cp "$kernel" "$root/boot/belka"
cp "$limine/limine-uefi-cd.bin" "$root/boot/limine/"
cp "$limine/BOOTX64.EFI" "$root/EFI/BOOT/"

{
        echo "timeout: $timeout"
        echo
        echo "/Belka"
        echo "    protocol: limine"
        echo "    path: boot():/boot/belka"
        if [ -n "$cmdline" ]; then
                echo "    cmdline: $cmdline"
        fi
} > "$root/boot/limine/limine.conf"

xorriso -report_about SORRY -as mkisofs -R -r -J \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$root" -o "$out"
