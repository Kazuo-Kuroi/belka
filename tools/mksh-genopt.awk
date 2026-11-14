#!/usr/bin/awk -f
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# Turn one of mksh's option tables (sh_flags.opt, ulimits.opt,
# rlimits.opt) into the header the C sources include, as mksh's own
# Build.sh would, without running that script.
#
# The input: a comment block; definition blocks, each "@SYM" and its
# lines, for the macro the includer defines; "@@"; then data: "<cX|..."
# entries that only add option letter X to the option string, ">cX|cond"
# entries followed by the line of the item, kept under the condition;
# and a closing "|SYM", the macro that selects the option string.
#
# usage: awk -f genopt.awk file.opt > file.gen

function cond_open(c) {
        if (c == "")
                return ""
        if (c ~ / /) {
                sub(/^ /, "", c)
                return "#if " c
        }
        if (c ~ /^!/)
                return "#ifndef " substr(c, 2)
        return "#ifdef " c
}

# An option letter for the option string: sorted by its lower case,
# with the lower case first.
function add_optc(line,    c, lo, sym) {
        c = substr(line, 2, 1)
        if (c == "|")
                return
        lo = tolower(c)
        sym = line
        sub(/^[<>]/, "|", sym)
        nopt++
        okey[nopt] = "<" lo (c == lo ? 1 : 0) sym
}

BEGIN {
        state = 0
        hdr = "/* +++ GENERATED FILE +++ DO NOT EDIT +++ */"
        gen = ""
        ddefs = ""
        nopt = 0
}

{
        line = $0
        if (state == 2 && line ~ /^\|/) {
                osym = substr(line, 2)
                gen = gen "\n#undef F0\n#undef FN" ddefs
                state = 3
                next
        }
        if (state == 1 && line == "@@") {
                gen = gen "\n#endif\n#ifndef F0\n#define F0 FN\n#endif"
                state = 2
                next
        }
        if (line ~ /^@@/) {
                print FILENAME ": bad line: " line > "/dev/stderr"
                exit 1
        }
        if (state == 0 && (line ~ /^\/\*-/ || line ~ /^ \*/ || line == "")) {
                hdr = hdr "\n" line
                next
        }
        if ((state == 0 || state == 1) && line ~ /^@/) {
                sym = substr(line, 2)
                gen = gen "\n" (state == 0 ? "#if" : "#elif") \
                    " defined(" sym ")"
                ddefs = ddefs "\n#undef " sym
                state = 1
                next
        }
        if (state == 0 || state == 3) {
                print FILENAME ": bad line: " line > "/dev/stderr"
                exit 1
        }
        if (state == 1) {
                gen = gen "\n" line
                next
        }
        # state 2: data
        if (line ~ /^<.*\|/) {
                add_optc(line)
                next
        }
        if (line ~ /^>.*\|/) {
                add_optc(line)
                c = substr(line, 2, 1)
                cond = line
                sub(/^[^|]*\|/, "", cond)
                co = cond_open(cond)
                optc = c == "|" ? "0" : "'" c "'"
                if ((getline item) <= 0) {
                        print FILENAME ": unexpected end" > "/dev/stderr"
                        exit 1
                }
                if (co != "")
                        gen = gen "\n" co
                gen = gen "\n" item ", " optc ")"
                if (co != "")
                        gen = gen "\n#endif"
        }
}

END {
        if (state != 3 || osym == "") {
                print FILENAME ": no end marker" > "/dev/stderr"
                exit 1
        }
        # Sort the option letters (a simple insertion sort, as sort(1)
        # in the C locale would order them).
        for (i = 2; i <= nopt; i++) {
                k = okey[i]
                for (j = i - 1; j >= 1 && okey[j] > k; j--)
                        okey[j + 1] = okey[j]
                okey[j + 1] = k
        }
        print hdr
        print "#ifndef " osym gen
        print "#else"
        for (i = 1; i <= nopt; i++) {
                n = split(okey[i], f, "|")
                opts = f[2]
                cond = n >= 3 ? f[3] : ""
                co = cond_open(cond)
                if (co != "")
                        print co
                print "\"" opts "\""
                if (co != "")
                        print "#endif"
        }
        print "#undef " osym
        print "#endif"
}
