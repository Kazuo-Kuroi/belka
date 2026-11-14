/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * crypt(3) for login: only NetBSD's "$sha1$" scheme (HMAC-SHA1,
 * iterated; contrib/netbsd/lib/libcrypt/crypt-sha1.c). Other schemes
 * fail, which login takes as a wrong password. NetBSD's bcrypt and
 * MD5 code are not used (the former has an advertising clause), and
 * neither is DES.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "compat.h"
#include "crypt.h"

char *
crypt(const char *pw, const char *salt)
{
        if (strncmp(salt, SHA1_MAGIC, sizeof(SHA1_MAGIC) - 1) == 0)
                return __crypt_sha1(pw, salt);
        errno = EINVAL;
        return NULL;
}
