#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-15
# SPDX-License-Identifier: MIT
#
# nvi's numbered tables, which NetBSD makes with nvi's own awk scripts:
# each entry of its option table (common/options.c), its ex command
# table (ex/ex_cmd.c), and its two tables of ipc messages (ipc/ipc_cmd.c
# and ip/ip_read.c) starts with a comment naming its index, as in
# "/* O_NAME ... */" or "/* C_NAME */", and the header numbers them in
# order: options and commands from 0, messages from 1 (0 is CODE_OOB).
# For the options it also gives the count.
#
#   awk -v kind=O -f tools/nvi-defs.awk options.c > options_def.h
#   awk -v kind=C -f tools/nvi-defs.awk ex_cmd.c > ex_def.h
#   (awk -v kind=SI -f tools/nvi-defs.awk ipc_cmd.c; \
#    awk -v kind=VI -f tools/nvi-defs.awk ip_read.c) > ipc_def.h
#

BEGIN {
        if (kind == "O" || kind == "C") {
                n = 0
        } else if (kind == "SI" || kind == "VI") {
                n = 1
        } else {
                print "nvi-defs.awk: kind must be O, C, SI or VI" \
                    > "/dev/stderr"
                exit 2
        }
        printf("/* Made by tools/nvi-defs.awk: the %s_ names. */\n", kind)
}

$1 == "/*" && $2 ~ ("^" kind "_[0-9A-Z_]+$") {
        printf("#define %s %d\n", $2, n++)
}

END {
        if (kind == "O")
                printf("#define O_OPTIONCOUNT %d\n", n)
}
