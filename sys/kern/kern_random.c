/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * The kernel's random numbers (doc/design/decisions.md, "Random
 * numbers"): a ChaCha20 keystream (RFC 8439's block function, with the
 * original 64-bit counter and nonce), its key replaced after every
 * request by keystream of its own, so that what was given out before
 * cannot be found again from the state. What the machine offers goes
 * into a pool: RDSEED and RDRAND when the processor has them, the
 * time stamp counter at boot and at each device interrupt, and what is
 * written to /dev/random. A request first folds a pool that has had
 * anything added into the key, through two ChaCha20 blocks. Nothing
 * waits: the generator is seeded before the first process runs.
 * /dev/random and /dev/urandom (devfs) read it.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/random.h>
#include <sys/spinlock.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include <machine/cpufunc.h>

#define RND_CHUNK       256     /* bytes made under the lock at once */

static struct spinlock rnd_lock = SPINLOCK_INITIALIZER("rnd_lock");
static uint32_t rnd_key[8];
static uint64_t rnd_nonce;              /* counts the requests */
static uint32_t rnd_pool[16];
static unsigned rnd_pos;
static unsigned rnd_added;              /* adds since the last fold */
static int rnd_hwrand;                  /* 1: RDRAND, 2: RDSEED too */

/* Clear what held key or keystream; not to be optimised away. */
static void
rnd_zero(void *p, size_t n)
{
        volatile uint8_t *v = p;

        while (n-- > 0)
                *v++ = 0;
}

static uint32_t
rotl32(uint32_t v, int n)
{
        return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d) do {                                             \
                a += b; d ^= a; d = rotl32(d, 16);                      \
                c += d; b ^= c; b = rotl32(b, 12);                      \
                a += b; d ^= a; d = rotl32(d, 8);                       \
                c += d; b ^= c; b = rotl32(b, 7);                       \
        } while (0)

/* One ChaCha20 block of key, at counter, with nonce, as 16 words. */
static void
chacha_block(const uint32_t key[8], uint64_t counter, uint64_t nonce,
    uint32_t out[16])
{
        uint32_t in[16], x[16];

        in[0] = 0x61707865;             /* "expand 32-byte k" */
        in[1] = 0x3320646e;
        in[2] = 0x79622d32;
        in[3] = 0x6b206574;
        for (int i = 0; i < 8; i++)
                in[4 + i] = key[i];
        in[12] = (uint32_t)counter;
        in[13] = (uint32_t)(counter >> 32);
        in[14] = (uint32_t)nonce;
        in[15] = (uint32_t)(nonce >> 32);
        for (int i = 0; i < 16; i++)
                x[i] = in[i];
        for (int i = 0; i < 10; i++) {
                QR(x[0], x[4], x[8], x[12]);
                QR(x[1], x[5], x[9], x[13]);
                QR(x[2], x[6], x[10], x[14]);
                QR(x[3], x[7], x[11], x[15]);
                QR(x[0], x[5], x[10], x[15]);
                QR(x[1], x[6], x[11], x[12]);
                QR(x[2], x[7], x[8], x[13]);
                QR(x[3], x[4], x[9], x[14]);
        }
        for (int i = 0; i < 16; i++)
                out[i] = x[i] + in[i];
}

/* A word into the pool (rnd_lock held). */
static void
rnd_mix(uint32_t v)
{
        uint32_t *w = &rnd_pool[rnd_pos];

        *w = rotl32(*w ^ v, 7) + rnd_pool[(rnd_pos + 5) & 15];
        rnd_pos = (rnd_pos + 1) & 15;
        rnd_added++;
}

/*
 * The pool into the key (rnd_lock held): a block of the key and the
 * pool's first half, and a block of that and its second half, whose
 * first eight words are the new key. The pool is emptied.
 */
static void
rnd_fold(void)
{
        uint32_t k[8], b[16];

        for (int i = 0; i < 8; i++)
                k[i] = rnd_key[i] ^ rnd_pool[i];
        chacha_block(k, 0, ~rnd_nonce, b);
        for (int i = 0; i < 8; i++)
                k[i] = b[i] ^ rnd_pool[8 + i];
        chacha_block(k, 1, ~rnd_nonce, b);
        for (int i = 0; i < 8; i++)
                rnd_key[i] = b[i];
        for (int i = 0; i < 16; i++)
                rnd_pool[i] = 0;
        rnd_added = 0;
        rnd_zero(k, sizeof(k));
        rnd_zero(b, sizeof(b));
}

/*
 * n bytes (RND_CHUNK at most) of keystream into buf, and a new key
 * (rnd_lock held).
 */
static void
rnd_make(uint8_t *buf, size_t n)
{
        uint32_t b[16];
        uint64_t ctr = 0;

        if (rnd_added != 0)
                rnd_fold();
        rnd_nonce++;
        while (n > 0) {
                size_t m = n < sizeof(b) ? n : sizeof(b);

                chacha_block(rnd_key, ++ctr, rnd_nonce, b);
                memcpy(buf, b, m);
                buf += m;
                n -= m;
        }
        chacha_block(rnd_key, 0, rnd_nonce, b);
        for (int i = 0; i < 8; i++)
                rnd_key[i] = b[i];
        rnd_zero(b, sizeof(b));
}

/* RDSEED's (seed) or RDRAND's next word; 0 and *ok clear if none. */
static uint64_t
rnd_hw(int seed, int *ok)
{
        uint64_t v = 0;
        int got = 0;

        for (int tries = 0; tries < 10 && !got; tries++)
                got = seed ? cpu_rdseed(&v) : cpu_rdrand(&v);
        *ok = got;
        return got ? v : 0;
}

/* The pool's first fill, from the processor, before any process runs. */
void
rnd_init(void)
{
        uint32_t regs[4];
        int s, ok;

        cpuid(1, 0, regs);
        if ((regs[2] & (1u << 30)) != 0)
                rnd_hwrand = 1;
        cpuid(0, 0, regs);
        if (regs[0] >= 7) {
                cpuid(7, 0, regs);
                if (rnd_hwrand && (regs[1] & (1u << 18)) != 0)
                        rnd_hwrand = 2;
        }
        s = spin_lock(&rnd_lock);
        for (int i = 0; i < 32; i++) {
                uint64_t t = rdtsc();

                rnd_mix((uint32_t)t);
                rnd_mix((uint32_t)(t >> 32));
                if (rnd_hwrand != 0) {
                        uint64_t v = rnd_hw(rnd_hwrand == 2, &ok);

                        if (!ok && rnd_hwrand == 2)
                                v = rnd_hw(0, &ok);
                        rnd_mix((uint32_t)v);
                        rnd_mix((uint32_t)(v >> 32));
                }
        }
        rnd_fold();
        spin_unlock(&rnd_lock, s);
        printf("random: seeded from the TSC%s\n", rnd_hwrand == 2 ?
            ", RDSEED and RDRAND" : rnd_hwrand == 1 ? " and RDRAND" : "");
}

/*
 * A device interrupt on vector: its time into the pool. Skipped when
 * another processor holds the lock; this is never worth waiting for.
 */
void
rnd_intr(int vector)
{
        uint64_t t = rdtsc();
        int s;

        if (!spin_trylock(&rnd_lock, &s))
                return;
        rnd_mix((uint32_t)t ^ ((uint32_t)vector << 24));
        spin_unlock(&rnd_lock, s);
}

/* len bytes into the pool. */
void
rnd_add(const void *data, size_t len)
{
        const uint8_t *p = data;
        int s = spin_lock(&rnd_lock);

        while (len > 0) {
                uint32_t v = 0;
                size_t n = len < 4 ? len : 4;

                memcpy(&v, p, n);
                rnd_mix(v);
                p += n;
                len -= n;
        }
        rnd_mix((uint32_t)rdtsc());
        spin_unlock(&rnd_lock, s);
}

/* len bytes of random numbers into buf, a kernel buffer. */
void
rnd_get(void *buf, size_t len)
{
        uint8_t *p = buf;

        while (len > 0) {
                size_t n = len < RND_CHUNK ? len : RND_CHUNK;
                int s = spin_lock(&rnd_lock);

                rnd_make(p, n);
                spin_unlock(&rnd_lock, s);
                p += n;
                len -= n;
        }
}

/* A read of /dev/random or /dev/urandom. */
int
rnd_read(struct uio *uio)
{
        uint8_t buf[RND_CHUNK];
        int error = 0;

        while (uio->uio_resid > 0 && error == 0) {
                size_t n = uio->uio_resid < (ssize_t)sizeof(buf) ?
                    (size_t)uio->uio_resid : sizeof(buf);

                rnd_get(buf, n);
                error = uiomove(buf, n, UIO_READ, uio);
        }
        rnd_zero(buf, sizeof(buf));
        return error;
}

/* A write of /dev/random or /dev/urandom: into the pool. */
int
rnd_write(struct uio *uio)
{
        uint8_t buf[RND_CHUNK];
        int error = 0;

        while (uio->uio_resid > 0 && error == 0) {
                size_t n = uio->uio_resid < (ssize_t)sizeof(buf) ?
                    (size_t)uio->uio_resid : sizeof(buf);

                if ((error = uiomove(buf, n, UIO_WRITE, uio)) == 0)
                        rnd_add(buf, n);
        }
        rnd_zero(buf, sizeof(buf));
        return error;
}


/*
 * The self-test's (subr_selftest_dev.c): ChaCha20's block function
 * against RFC 8439's test vector (2.3.2), its 32-bit counter and 96-bit
 * nonce laid over this one's 64-bit pair. Nonzero if it matches.
 */
int
rnd_selftest(void)
{
        static const uint32_t want[16] = {
                0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3,
                0xc7f4d1c7, 0x0368c033, 0x9aaa2204, 0x4e6cd4c3,
                0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
                0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2,
        };
        uint32_t key[8], out[16];

        for (int i = 0; i < 8; i++)
                key[i] = (uint32_t)(4 * i) | (uint32_t)(4 * i + 1) << 8 |
                    (uint32_t)(4 * i + 2) << 16 | (uint32_t)(4 * i + 3) << 24;
        chacha_block(key, 1 | (uint64_t)0x09000000 << 32, 0x4a000000, out);
        return memcmp(out, want, sizeof(want)) == 0;
}
