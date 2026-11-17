/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * crypt(3) for login: only NetBSD's "$sha1$" scheme (HMAC-SHA1,
 * iterated; contrib/netbsd/lib/libcrypt/crypt-sha1.c). For any other
 * setting, such as a locked account's "*LK*", it fails as libxcrypt
 * does: with "*0" (or "*1" if the setting begins with "*0"), which no
 * hash equals, so that login and su, which compare without looking for
 * NULL, find a wrong password; so does a "$sha1$" setting that is not
 * well formed. NetBSD's bcrypt and MD5 code are not used (the former
 * has an advertising clause), and neither is DES.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "compat.h"
#include "crypt.h"

char *
crypt(const char *pw, const char *salt)
{
        char *hash;

        if (strncmp(salt, SHA1_MAGIC, sizeof(SHA1_MAGIC) - 1) == 0 &&
            (hash = __crypt_sha1(pw, salt)) != NULL)
                return hash;
        errno = EINVAL;
        return (char *)(salt[0] == '*' && salt[1] == '0' ? "*1" : "*0");
}
