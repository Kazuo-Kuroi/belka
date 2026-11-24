/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * arc4random and arc4random_buf for dhcpcd, from getentropy(): the
 * kernel's generator, one call per GETENTROPY_MAX bytes. dhcpcd asks
 * for little (transaction ids, jitter), so there is no buffering, and
 * nothing to stir again after a fork. getentropy cannot fail for want
 * of entropy; if it fails at all, dhcpcd stops rather than go on with
 * numbers that are not random.
 */

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "compat/arc4random.h"

void
arc4random_buf(void *buf, size_t len)
{
        unsigned char *p = buf;

        while (len > 0) {
                size_t n = len < GETENTROPY_MAX ? len : GETENTROPY_MAX;

                if (getentropy(p, n) != 0)
                        abort();
                p += n;
                len -= n;
        }
}

uint32_t
arc4random(void)
{
        uint32_t v;

        arc4random_buf(&v, sizeof(v));
        return v;
}
