/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/* strvis and strnvis; see <vis.h>. */

#include <stdio.h>
#include <string.h>
#include <vis.h>

/* One byte, encoded at dst (room for 4 and a NUL); its length. */
static size_t
vis1(char *dst, unsigned char c, int flags)
{
        int white = (c == ' ' && (flags & VIS_SP) != 0) ||
            (c == '\t' && (flags & VIS_TAB) != 0) ||
            (c == '\n' && (flags & VIS_NL) != 0);

        if (c == '\\') {
                dst[0] = dst[1] = '\\';
                dst[2] = '\0';
                return 2;
        }
        if (!white && ((c >= ' ' && c < 0x7f) || c == '\t' || c == '\n')) {
                dst[0] = (char)c;
                dst[1] = '\0';
                return 1;
        }
        snprintf(dst, 5, "\\%03o", c);
        return 4;
}

int
strnvis(char *dst, size_t len, const char *src, int flags)
{
        size_t n = 0;
        char buf[5];

        for (; *src != '\0'; src++) {
                size_t k = vis1(buf, (unsigned char)*src, flags);

                if (n + k >= len) {
                        if (len != 0)
                                dst[n] = '\0';
                        return -1;
                }
                memcpy(dst + n, buf, k);
                n += k;
        }
        if (len != 0)
                dst[n] = '\0';
        return (int)n;
}

/* dst has room for 4 times src's length and a NUL. */
int
strvis(char *dst, const char *src, int flags)
{
        return strnvis(dst, 4 * strlen(src) + 1, src, flags);
}

/* Decode src into dst (no longer than src); the length, or -1. */
int
strunvis(char *dst, const char *src)
{
        char *d = dst;

        while (*src != '\0') {
                if (*src != '\\') {
                        *d++ = *src++;
                        continue;
                }
                src++;
                if (*src >= '0' && *src <= '7') {
                        int v = 0;

                        for (int i = 0; i < 3 && *src >= '0' && *src <= '7';
                            i++)
                                v = v * 8 + (*src++ - '0');
                        *d++ = (char)v;
                        continue;
                }
                switch (*src) {
                case '\\':
                        *d++ = '\\';
                        break;
                case 'n':
                        *d++ = '\n';
                        break;
                case 't':
                        *d++ = '\t';
                        break;
                case 'r':
                        *d++ = '\r';
                        break;
                case 's':
                        *d++ = ' ';
                        break;
                default:
                        return -1;
                }
                src++;
        }
        *d = '\0';
        return (int)(d - dst);
}
