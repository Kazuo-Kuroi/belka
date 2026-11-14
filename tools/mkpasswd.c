/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * A host tool for building images: print the password-file hash of a
 * password by NetBSD's SHA1 scheme, the one Belka's login checks. The
 * setting is "$sha1$iterations$salt$".
 *
 * usage: mkpasswd password setting
 */

#include <stdio.h>
#include <string.h>

char *__crypt_sha1(const char *, const char *);

/* crypt-sha1.c clears its buffers with NetBSD's explicit_memset. */
void *
explicit_memset(void *p, int c, size_t n)
{
        void *(*volatile fn)(void *, int, size_t) = memset;

        return fn(p, c, n);
}

int
main(int argc, char **argv)
{
        const char *h;

        if (argc != 3 || strncmp(argv[2], "$sha1$", 6) != 0) {
                fprintf(stderr, "usage: mkpasswd password '$sha1$n$salt$'\n");
                return 2;
        }
        if ((h = __crypt_sha1(argv[1], argv[2])) == NULL) {
                fprintf(stderr, "mkpasswd: bad setting\n");
                return 1;
        }
        printf("%s\n", h);
        return 0;
}
