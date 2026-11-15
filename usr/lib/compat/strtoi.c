/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * NetBSD functions that its libutil and tic use and other C libraries
 * lack; built for Belka (in the compat library) and for the host (the
 * host's tic):
 *
 *   strtoi, strtou   strtoimax and strtoumax that also check a range
 *                    and report how the conversion went in *rstatus:
 *                    0, ECANCELED (nothing converted), ENOTSUP (more
 *                    after the number), ERANGE (out of [lo, hi])
 *   hdestroy1        hdestroy, freeing the keys and data with the
 *                    functions given: here, only the table is freed,
 *                    which leaks what they would have freed
 */

#include <errno.h>
#include <inttypes.h>
#include <search.h>
#include <stddef.h>
#include <stdint.h>

intmax_t strtoi(const char *, char **, int, intmax_t, intmax_t, int *);
uintmax_t strtou(const char *, char **, int, uintmax_t, uintmax_t, int *);
void hdestroy1(void (*)(void *), void (*)(void *));

intmax_t
strtoi(const char *s, char **endp, int base, intmax_t lo, intmax_t hi,
    int *rstatus)
{
        int saved = errno, st = 0;
        char *end;
        intmax_t v;

        errno = 0;
        v = strtoimax(s, &end, base);
        if (end == s)
                st = ECANCELED;
        else if (errno == ERANGE || v < lo || v > hi)
                st = ERANGE;
        else if (*end != '\0')
                st = ENOTSUP;
        if (v < lo)
                v = lo;
        if (v > hi)
                v = hi;
        if (endp != NULL)
                *endp = end;
        if (rstatus != NULL)
                *rstatus = st;
        errno = saved;
        return v;
}

uintmax_t
strtou(const char *s, char **endp, int base, uintmax_t lo, uintmax_t hi,
    int *rstatus)
{
        int saved = errno, st = 0;
        char *end;
        uintmax_t v;

        errno = 0;
        v = strtoumax(s, &end, base);
        if (end == s)
                st = ECANCELED;
        else if (errno == ERANGE || v < lo || v > hi)
                st = ERANGE;
        else if (*end != '\0')
                st = ENOTSUP;
        if (v < lo)
                v = lo;
        if (v > hi)
                v = hi;
        if (endp != NULL)
                *endp = end;
        if (rstatus != NULL)
                *rstatus = st;
        errno = saved;
        return v;
}

void
hdestroy1(void (*freekey)(void *), void (*freedata)(void *))
{
        (void)freekey;
        (void)freedata;
        hdestroy();
}
