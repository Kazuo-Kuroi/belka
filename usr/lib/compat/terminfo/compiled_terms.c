/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * libterminfo's built-in descriptions, which term.c includes and
 * searches when the database has no entry. NetBSD makes this file by
 * running tic at build time; Belka compiles its database on the system
 * itself instead (/etc/rc), so there are none built in: the one entry
 * has an empty name, which no terminal has.
 */

struct compiled_term {
        const char      *name;
        const char      *cap;
        size_t          caplen;
};

static const struct compiled_term compiled_terms[] = {
        { "", NULL, 0 },
};
