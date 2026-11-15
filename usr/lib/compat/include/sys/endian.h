/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/* mlibc's <sys/endian.h>, with BSD's byte-string encoders and decoders. */

#ifndef _COMPAT_SYS_ENDIAN_H_
#define _COMPAT_SYS_ENDIAN_H_

#include_next <sys/endian.h>
#include <stdint.h>

static inline uint16_t
le16dec(const void *p)
{
        const uint8_t *b = p;

        return (uint16_t)(b[0] | b[1] << 8);
}

static inline uint32_t
le32dec(const void *p)
{
        const uint8_t *b = p;

        return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
            (uint32_t)b[3] << 24;
}

static inline void
le16enc(void *p, uint16_t v)
{
        uint8_t *b = p;

        b[0] = (uint8_t)v;
        b[1] = (uint8_t)(v >> 8);
}

static inline void
le32enc(void *p, uint32_t v)
{
        uint8_t *b = p;

        b[0] = (uint8_t)v;
        b[1] = (uint8_t)(v >> 8);
        b[2] = (uint8_t)(v >> 16);
        b[3] = (uint8_t)(v >> 24);
}

#endif /* _COMPAT_SYS_ENDIAN_H_ */
