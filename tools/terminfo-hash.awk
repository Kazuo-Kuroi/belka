#!/usr/bin/awk -f
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# Make libterminfo's hash.c from its term.h: for the flag, number and
# string capabilities (the TICODE_ names of enums TIFLAGS, TINUMS and
# TISTRS, in order), a table of their names and the functions
# _ti_flagid(), _ti_flagindex() and the rest that libterminfo calls.
# NetBSD makes the file with its genhash script and nbperf, a perfect
# hash generator; this makes the same functions with a plain search,
# which is quick enough for a few hundred names, without running them.
#
# usage: awk -f terminfo-hash.awk term.h > hash.c

function emit(kind, n, names,    i) {
        printf "\nstatic const char *const _ti_%sids[] = {\n", kind
        for (i = 1; i <= n; i++)
                printf "\t\"%s\",\n", names[i]
        printf "};\n\n"
        printf "const char *\n_ti_%sid(ssize_t idx)\n{\n", kind
        printf "\tif (idx < 0 || (size_t)idx >= __arraycount(_ti_%sids))\n", kind
        printf "\t\treturn NULL;\n\treturn _ti_%sids[idx];\n}\n\n", kind
        printf "ssize_t\n_ti_%sindex(const char *key)\n{\n", kind
        printf "\tfor (size_t i = 0; i < __arraycount(_ti_%sids); i++)\n", kind
        printf "\t\tif (strcmp(key, _ti_%sids[i]) == 0)\n", kind
        printf "\t\t\treturn (ssize_t)i;\n\treturn -1;\n}\n"
}

BEGIN {
        kind = ""
        print "/* Generated from term.h by tools/terminfo-hash.awk. */"
        print ""
        print "#include <stdint.h>"
        print "#include <stdlib.h>"
        print "#include <string.h>"
        print "#include <sys/endian.h>"
        print "#include <term_private.h>"
        print "#include <term.h>"
}

/^enum TIFLAGS/ { kind = "flag"; n = 0; next }
/^enum TINUMS/  { kind = "num"; n = 0; next }
/^enum TISTRS/  { kind = "str"; n = 0; next }

kind != "" && /};/ {
        if (kind == "flag") { nf = n; for (i = 1; i <= n; i++) f[i] = cur[i] }
        if (kind == "num")  { nn = n; for (i = 1; i <= n; i++) m[i] = cur[i] }
        if (kind == "str")  { ns = n; for (i = 1; i <= n; i++) s[i] = cur[i] }
        kind = ""
        next
}

kind != "" && /TICODE_/ {
        name = $0
        sub(/^.*TICODE_/, "", name)
        sub(/[ \t,].*$/, "", name)
        cur[++n] = name
}

END {
        if (nf == 0 || nn == 0 || ns == 0) {
                print "terminfo-hash.awk: capabilities not found" > "/dev/stderr"
                exit 1
        }
        emit("flag", nf, f)
        emit("num", nn, m)
        emit("str", ns, s)
}
