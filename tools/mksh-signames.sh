#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# Write mksh's signames.inc, the table of signal names and numbers, from
# the C library's <signal.h> as the given compiler sees it: one
# { "NAME", number }, line per signal, the first name for each number.
# mksh's Build.sh makes the same table; this does it without that
# script.
#
# usage: mksh-signames.sh "cc flags..." > signames.inc
#

set -eu

cc=$1
echo '#include <signal.h>' | $cc -E -dM -x c - |
    awk '$1 == "#define" && $2 ~ /^SIG[A-Z0-9]+$/ && $3 ~ /^[0-9]+$/ &&
        $2 != "SIGSTKSZ" && $2 != "SIGRTMIN" && $2 != "SIGRTMAX" {
                if ($3 > 0 && !($3 in seen)) {
                        seen[$3] = 1
                        printf "\t\t{ \"%s\", %d },\n", substr($2, 4), $3
                }
        }'
