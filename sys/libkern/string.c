/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Freestanding string and memory routines. Compilers may emit calls to
 * memcpy, memmove, memset, and memcmp even in freestanding code, so
 * these must exist under those names. They are simple byte loops; tuned
 * machine-dependent versions can replace them later.
 */

#include <stddef.h>

#include <sys/systm.h>

void *
memcpy(void *dst, const void *src, size_t len)
{
        unsigned char *d = dst;
        const unsigned char *s = src;

        while (len-- > 0)
                *d++ = *s++;
        return dst;
}

void *
memmove(void *dst, const void *src, size_t len)
{
        unsigned char *d = dst;
        const unsigned char *s = src;

        if (d == s || len == 0)
                return dst;
        if (d < s) {
                while (len-- > 0)
                        *d++ = *s++;
        } else {
                d += len;
                s += len;
                while (len-- > 0)
                        *--d = *--s;
        }
        return dst;
}

void *
memset(void *dst, int c, size_t len)
{
        unsigned char *d = dst;

        while (len-- > 0)
                *d++ = (unsigned char)c;
        return dst;
}

int
memcmp(const void *a, const void *b, size_t len)
{
        const unsigned char *p = a;
        const unsigned char *q = b;

        for (; len > 0; len--, p++, q++) {
                if (*p != *q)
                        return (*p < *q) ? -1 : 1;
        }
        return 0;
}

size_t
strlen(const char *s)
{
        const char *p = s;

        while (*p != '\0')
                p++;
        return (size_t)(p - s);
}

int
strcmp(const char *a, const char *b)
{
        while (*a != '\0' && *a == *b) {
                a++;
                b++;
        }
        return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t len)
{
        for (; len > 0; len--, a++, b++) {
                if (*a != *b)
                        return (unsigned char)*a - (unsigned char)*b;
                if (*a == '\0')
                        break;
        }
        return 0;
}
