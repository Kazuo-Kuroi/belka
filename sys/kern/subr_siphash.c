/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * SipHash-2-4, as Aumasson and Bernstein describe it ("SipHash: a fast
 * short-input PRF", 2012): a 128-bit key, a message of any length, and
 * a 64-bit hash, with two rounds a word and four to finish. For what
 * must not be guessed without the key, as TCP's initial sequence
 * numbers (RFC 6528).
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/siphash.h>

#define ROTL(x, b)      (((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND do {                                                   \
        v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32);       \
        v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2;                          \
        v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0;                          \
        v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32);       \
} while (0)

/* Eight bytes, little-endian. */
static uint64_t
le64(const uint8_t *p)
{
        uint64_t v = 0;

        for (int i = 7; i >= 0; i--)
                v = v << 8 | p[i];
        return v;
}

uint64_t
siphash24(const uint8_t key[SIPHASH_KEYLEN], const void *msg, size_t len)
{
        const uint8_t *p = msg;
        uint64_t k0 = le64(key), k1 = le64(key + 8), m, b;
        uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
        uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
        uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
        uint64_t v3 = k1 ^ 0x7465646279746573ULL;
        size_t left = len;

        for (; left >= 8; p += 8, left -= 8) {
                m = le64(p);
                v3 ^= m;
                SIPROUND;
                SIPROUND;
                v0 ^= m;
        }
        /* The last bytes, and the length's low byte at the top. */
        b = (uint64_t)len << 56;
        for (size_t i = 0; i < left; i++)
                b |= (uint64_t)p[i] << (8 * i);
        v3 ^= b;
        SIPROUND;
        SIPROUND;
        v0 ^= b;
        v2 ^= 0xff;
        SIPROUND;
        SIPROUND;
        SIPROUND;
        SIPROUND;
        return v0 ^ v1 ^ v2 ^ v3;
}
