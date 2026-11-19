#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
set -eu
work=${1:-/var/tmp/gcc-bootstrap}
work=$(CDPATH= cd -- "$work" && pwd)
prefix=$work/install/usr
kit=/usr/src/gcc-bootstrap
PATH=$prefix/bin:/bin:/sbin:/usr/bin:/usr/sbin
LC_ALL=C
export PATH LC_ALL

# Reject accidental use of the original compiler's private programs or CRT.
for program in cc1 cc1plus; do
        case $(gcc -print-prog-name="$program") in
        "$prefix"/*) ;;
        *) echo "native-gcc: $program is outside the staged installation" >&2
           exit 1 ;;
        esac
done
for file in libgcc.a libstdc++.a belka-crtbegin.o belka-crtend.o; do
        case $(g++ -print-file-name="$file") in
        "$prefix"/*) ;;
        *) echo "native-gcc: $file is outside the staged installation" >&2
           exit 1 ;;
        esac
done

pass=0
fail=0
check()
{
        name=$1
        want=$2
        shift 2
        if got=$("$@" 2>&1) && [ "$got" = "$want" ]; then
                pass=$((pass + 1))
        else
                fail=$((fail + 1))
                printf 'native-gcc: FAIL %s: want [%s], got [%s]\n' \
                    "$name" "$want" "$got"
        fi
}
status()
{
        name=$1
        want=$2
        shift 2
        if diagnostics=$("$@" 2>&1); then
                result=0
        else
                result=$?
        fi
        if [ "$result" -eq "$want" ]; then
                pass=$((pass + 1))
        else
                fail=$((fail + 1))
                printf 'native-gcc: FAIL %s: status %s, want %s\n%s\n' \
                    "$name" "$result" "$want" "$diagnostics"
        fi
}
mkdir -p "$work/check"
cd "$work/check"
COMPILER_EXEC_SOURCE=$kit/tests/exectest.c
. "$kit/tests/compiler.sh"
printf 'native-gcc: %s checks passed, %s failures\n' "$pass" "$fail"
test "$fail" -eq 0
