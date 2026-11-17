/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-16
 * SPDX-License-Identifier: MIT
 */

/*
 * parsedate(3), for find's -newerXt and -Xsince: a small subset of
 * NetBSD's, which takes nearly any written date. This one takes
 *
 *   @N                         N seconds since 1970 (UTC)
 *   YYYY-MM-DD                 midnight
 *   YYYY-MM-DD HH:MM[:SS]      (or with a T between them)
 *
 * all in UTC, and for anything else returns -1 with errno EINVAL. The
 * time and zone arguments are ignored (they may be NULL).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Days from 1970-01-01 to y-m-d, by the proleptic Gregorian calendar. */
static long long
days_from_civil(long long y, int m, int d)
{
        long long era, yoe, doy, doe;

        y -= m <= 2;
        era = (y >= 0 ? y : y - 399) / 400;
        yoe = y - era * 400;
        doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
}

/* n digits at *sp, as a number; -1 if they are not there. */
static long
digits(const char **sp, int n)
{
        long v = 0;

        for (int i = 0; i < n; i++, (*sp)++) {
                if (**sp < '0' || **sp > '9')
                        return -1;
                v = v * 10 + (**sp - '0');
        }
        return v;
}

time_t
parsedate(const char *s, const time_t *now, const int *zone)
{
        long y, mo, d, h = 0, mi = 0, sec = 0;
        char *end;

        (void)now;
        (void)zone;
        if (s == NULL)
                goto bad;
        if (*s == '@') {
                long long v;

                errno = 0;
                v = strtoll(s + 1, &end, 10);
                if (end == s + 1 || *end != '\0' || errno != 0)
                        goto bad;
                return (time_t)v;
        }
        if ((y = digits(&s, 4)) < 0 || *s++ != '-' ||
            (mo = digits(&s, 2)) < 1 || mo > 12 || *s++ != '-' ||
            (d = digits(&s, 2)) < 1 || d > 31)
                goto bad;
        if (*s == ' ' || *s == 'T') {
                s++;
                if ((h = digits(&s, 2)) < 0 || h > 23 || *s++ != ':' ||
                    (mi = digits(&s, 2)) < 0 || mi > 59)
                        goto bad;
                if (*s == ':' && (s++, (sec = digits(&s, 2)) < 0 ||
                    sec > 60))
                        goto bad;
        }
        if (*s != '\0')
                goto bad;
        return (time_t)(days_from_civil(y, (int)mo, (int)d) * 86400 +
            h * 3600 + mi * 60 + sec);
bad:
        errno = EINVAL;
        return (time_t)-1;
}
