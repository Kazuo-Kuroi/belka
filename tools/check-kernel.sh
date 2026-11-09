#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-09
# SPDX-License-Identifier: MIT
#
# Check a linked kernel's layout, whichever linker made it: three
# loadable segments (text R-X, read-only data R--, data RW-), each
# page-aligned and in order, with physical addresses equal to virtual
# ones (Limine loads by them), and kernel_start, etext, erodata, and
# kernel_end at the segments' bounds, as hat.c maps them.
#
# usage: check-kernel.sh kernel [readelf [nm]]
#

set -eu

if [ $# -lt 1 ]; then
        echo "usage: $0 kernel [readelf [nm]]" >&2
        exit 1
fi
kernel=$1
READELF=${2:-readelf}
NM=${3:-nm}

fail() {
        echo "check-kernel: $kernel: $*" >&2
        exit 1
}

# Addresses are 64-bit and unsigned, beyond the shell's arithmetic: the
# kernel lies in the top 2 GB, so the upper 32 bits must all be equal
# and the arithmetic is done on the lower 32.
segs=$("$READELF" -lW "$kernel" | awk '$1 == "LOAD" {
        flags = ""
        for (i = 7; i < NF; i++)
                flags = flags $i
        print $3, $4, $6, flags
}')
[ "$(echo "$segs" | wc -l)" -eq 3 ] || fail "not three loadable segments"

# hex value: 16 hex digits, zero-padded
hex16() {
        echo "$1" | awk '{ s = tolower($1); sub(/^0x/, "", s)
                while (length(s) < 16) s = "0" s; print s }'
}

sym() {
        v=$("$NM" "$kernel" | awk -v s="$1" '$3 == s { print $1 }')
        [ -n "$v" ] || fail "no symbol $1"
        hex16 "$v"
}

# The upper 32 bits every address shares: the first segment's.
high=$(hex16 "$(echo "$segs" | awk 'NR == 1 { print $1 }')" | cut -c1-8)

# low32 hexvalue: the lower 32 bits, after checking the upper ones
low32() {
        [ "$(echo "$1" | cut -c1-8)" = "$high" ] ||
            fail "addresses span more than 4 GB"
        echo $((0x$(echo "$1" | cut -c9-16)))
}

n=0
prev_end=0
echo "$segs" | {
        while read -r vaddr paddr memsz flags; do
                n=$((n + 1))
                v=$(hex16 "$vaddr")
                [ "$v" = "$(hex16 "$paddr")" ] ||
                    fail "segment $n: physical address $paddr, not $vaddr"
                lo=$(low32 "$v")
                [ $((lo & 0xfff)) -eq 0 ] ||
                    fail "segment $n at $vaddr is not page-aligned"
                [ $n -eq 1 ] || [ "$lo" -ge "$prev_end" ] ||
                    fail "segment $n at $vaddr overlaps the one before"
                prev_end=$((lo + memsz))
                case $n in
                1)      want=RE; bound=kernel_start ;;
                2)      want=R;  bound=etext ;;
                3)      want=RW; bound=erodata ;;
                esac
                [ "$flags" = "$want" ] ||
                    fail "segment $n has flags $flags, not $want"
                [ "$(sym $bound)" = "$v" ] ||
                    fail "$bound is not at segment $n ($vaddr)"
        done
        [ "$(low32 "$(sym kernel_end)")" -eq "$prev_end" ] ||
            fail "kernel_end is not at the end of the image"
}
