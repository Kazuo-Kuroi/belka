/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Boot-time self-tests. They check, on one processor, that the basic
 * primitives behave as documented. Multiprocessor stress tests are added
 * once other processors are started.
 */

#include <stdint.h>

#include <sys/systm.h>

#include <machine/atomic.h>

static int selftest_failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        selftest_failures++;                            \
                }                                                       \
        } while (0)

static void selftest_atomic(void);
static void selftest_prf(void);
static void selftest_string(void);

static void
selftest_atomic(void)
{
        volatile uint32_t w = 5;
        volatile uint64_t q = 5;
        int target = 0;
        int other = 0;
        int *volatile ptr = &target;

        CHECK(atomic_cas_32(&w, 5, 7) == 5 && w == 7);
        CHECK(atomic_cas_32(&w, 5, 9) == 7 && w == 7);
        CHECK(atomic_cas_64(&q, 5, UINT64_C(0x100000000)) == 5);
        CHECK(q == UINT64_C(0x100000000));
        CHECK(atomic_cas_64(&q, 5, 1) == UINT64_C(0x100000000));

        CHECK(atomic_swap_32(&w, 11) == 7 && w == 11);
        CHECK(atomic_swap_64(&q, 3) == UINT64_C(0x100000000) && q == 3);

        CHECK(atomic_add_32_nv(&w, 4) == 15);
        CHECK(atomic_add_32_nv(&w, -20) == UINT32_C(0xfffffffb));
        CHECK(atomic_inc_64_nv(&q) == 4);
        CHECK(atomic_dec_64_nv(&q) == 3);

        w = 0x0f;
        atomic_or_32(&w, 0xf0);
        CHECK(w == 0xff);
        atomic_and_32(&w, 0x3c);
        CHECK(w == 0x3c);
        q = 1;
        atomic_or_64(&q, UINT64_C(1) << 40);
        CHECK(q == (UINT64_C(1) | (UINT64_C(1) << 40)));
        atomic_and_64(&q, UINT64_C(1) << 40);
        CHECK(q == UINT64_C(1) << 40);

        CHECK(atomic_cas_ptr(&ptr, &target, &other) == &target);
        CHECK(ptr == &other);
        CHECK(atomic_swap_ptr(&ptr, &target) == &other && ptr == &target);

        atomic_store_32(&w, 42);
        CHECK(atomic_load_32(&w) == 42);

        membar_sync();
        membar_enter();
        membar_exit();
        membar_producer();
        membar_consumer();
}

static void
selftest_prf(void)
{
        char buf[64];

        snprintf(buf, sizeof(buf), "%d %i %u", -42, 7, 3000000000u);
        CHECK(strcmp(buf, "-42 7 3000000000") == 0);
        snprintf(buf, sizeof(buf), "%x %X %#x %#o %o", 0xbeef, 0xbeef, 0x1f,
            8, 0);
        CHECK(strcmp(buf, "beef BEEF 0x1f 010 0") == 0);
        snprintf(buf, sizeof(buf), "[%5d|%-5d|%05d]", 42, 42, -42);
        CHECK(strcmp(buf, "[   42|42   |-0042]") == 0);
        snprintf(buf, sizeof(buf), "[%6s|%-6s|%.2s|%*d]", "ab", "ab", "xyz",
            4, 9);
        CHECK(strcmp(buf, "[    ab|ab    |xy|   9]") == 0);
        snprintf(buf, sizeof(buf), "%jd %ju %zu %lld %hhd %hu",
            (intmax_t)INT64_MIN, (uintmax_t)UINT64_MAX, (size_t)17,
            -1LL, 300, 70000);
        CHECK(strcmp(buf, "-9223372036854775808 18446744073709551615 "
            "17 -1 44 4464") == 0);
        snprintf(buf, sizeof(buf), "%p %c%%%s", (void *)0x1234, 'z',
            (const char *)0);
        CHECK(strcmp(buf, "0x0000000000001234 z%(null)") == 0);
        CHECK(snprintf(buf, 4, "%s", "truncate") == 8);
        CHECK(strcmp(buf, "tru") == 0);
}

static void
selftest_string(void)
{
        char a[16];
        char b[16];

        memset(a, 'x', sizeof(a));
        CHECK(a[0] == 'x' && a[15] == 'x');
        memcpy(b, "0123456789", 11);
        CHECK(memcmp(b, "0123456789", 11) == 0);
        memmove(b + 2, b, 8);
        CHECK(memcmp(b, "0101234567", 10) == 0);
        memmove(b, b + 2, 8);
        CHECK(memcmp(b, "0123456767", 10) == 0);
        CHECK(strlen("belka") == 5);
        CHECK(strcmp("abc", "abd") < 0 && strcmp("abd", "abc") > 0);
        CHECK(strncmp("abcx", "abcy", 3) == 0);
}

/* Run all self-tests and return the number of failed checks. */
int
selftest_run(void)
{
        selftest_failures = 0;
        selftest_atomic();
        selftest_prf();
        selftest_string();
        return selftest_failures;
}
