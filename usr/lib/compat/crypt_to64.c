/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * The base-64 encoding of crypt(3) results, which NetBSD's SHA1 crypt
 * uses, with nothing but <stdint.h>.
 */

#include <stdint.h>

void __crypt_to64(char *, uint32_t, int);

/* n characters of crypt's alphabet for v, the low six bits first. */
void
__crypt_to64(char *s, uint32_t v, int n)
{
        static const char itoa64[] =
            "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

        while (--n >= 0) {
                *s++ = itoa64[v & 0x3f];
                v >>= 6;
        }
}
