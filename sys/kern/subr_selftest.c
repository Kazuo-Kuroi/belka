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
#include <machine/param.h>

#include <vm/page.h>

static int selftest_failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        selftest_failures++;                            \
                }                                                       \
        } while (0)

#define PAGE_STRESS_SLOTS       256
#define PAGE_STRESS_ROUNDS      20000

static void selftest_atomic(void);
static void selftest_page(void);
static int page_stats_equal(const struct page_stats *,
    const struct page_stats *);
static uint32_t selftest_random(void);
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

static uint32_t selftest_seed = 12345;

/* A linear congruential generator; good enough to vary test patterns. */
static uint32_t
selftest_random(void)
{
        selftest_seed = selftest_seed * 1103515245u + 12345u;
        return selftest_seed >> 8;
}

static int
page_stats_equal(const struct page_stats *a, const struct page_stats *b)
{
        if (a->ps_managed != b->ps_managed || a->ps_free != b->ps_free)
                return 0;
        for (int o = 0; o < PAGE_NORDERS; o++) {
                if (a->ps_blocks[o] != b->ps_blocks[o])
                        return 0;
        }
        return 1;
}

/*
 * Page allocator tests. Each phase returns every page it took, and the
 * free lists must then be exactly as before, block for block, which
 * shows that freed buddies are merged again.
 */
static void
selftest_page(void)
{
        static struct page *slot[PAGE_STRESS_SLOTS];
        static unsigned int slot_order[PAGE_STRESS_SLOTS];
        struct page_stats before, after;
        struct page *pp;
        struct page *chain;
        uint64_t *word;
        size_t count;

        page_get_stats(&before);
        CHECK(before.ps_free > 0 && before.ps_free <= before.ps_managed);

        /* One page: identity, alignment, and the direct map. */
        pp = page_alloc(0, 0);
        CHECK(pp != NULL);
        if (pp != NULL) {
                CHECK((pp->p_flags & PG_ALLOC) != 0);
                CHECK((page_to_pa(pp) & PAGEOFFSET) == 0);
                CHECK(page_to_pa(pp) != 0);
                CHECK(pa_to_page(page_to_pa(pp)) == pp);
                CHECK(pa_to_page(page_to_pa(pp) + 123) == pp);
                word = page_to_kva(pp);
                word[0] = UINT64_C(0x0123456789abcdef);
                word[PAGESIZE / sizeof(*word) - 1] = ~word[0];
                CHECK(word[0] == UINT64_C(0x0123456789abcdef));
                page_free(pp, 0);
        }

        /* Every order is aligned to its own size. */
        for (unsigned int o = 0; o < PAGE_NORDERS; o++) {
                pp = page_alloc(o, 0);
                if (pp == NULL)
                        continue;       /* memory may be fragmented */
                CHECK((pp->p_pfn & (((pfn_t)1 << o) - 1)) == 0);
                page_free(pp, o);
        }
        CHECK(page_alloc(PAGE_NORDERS, 0) == NULL);

        /* PA_ZERO clears a block that was dirty. */
        pp = page_alloc(2, 0);
        CHECK(pp != NULL);
        if (pp != NULL) {
                memset(page_to_kva(pp), 0xa5, 4 * PAGESIZE);
                page_free(pp, 2);
                pp = page_alloc(2, PA_ZERO);
                CHECK(pp != NULL);
        }
        if (pp != NULL) {
                const unsigned char *b = page_to_kva(pp);
                size_t nonzero = 0;

                for (size_t i = 0; i < 4 * PAGESIZE; i++)
                        nonzero += (b[i] != 0);
                CHECK(nonzero == 0);
                page_free(pp, 2);
        }

        /*
         * Random allocations and frees. Each block is stamped with its
         * own address in its first and last words, and the stamps are
         * checked on free, which catches blocks handed out twice.
         */
        memset(slot, 0, sizeof(slot));
        for (int round = 0; round < PAGE_STRESS_ROUNDS; round++) {
                unsigned int i = selftest_random() % PAGE_STRESS_SLOTS;

                if (slot[i] == NULL) {
                        unsigned int o = selftest_random() % 5;

                        if ((slot[i] = page_alloc(o, 0)) == NULL)
                                continue;
                        slot_order[i] = o;
                        word = page_to_kva(slot[i]);
                        word[0] = page_to_pa(slot[i]);
                        word[(PAGESIZE << o) / sizeof(*word) - 1] =
                            page_to_pa(slot[i]);
                } else {
                        unsigned int o = slot_order[i];

                        word = page_to_kva(slot[i]);
                        CHECK(word[0] == page_to_pa(slot[i]));
                        CHECK(word[(PAGESIZE << o) / sizeof(*word) - 1] ==
                            page_to_pa(slot[i]));
                        page_free(slot[i], o);
                        slot[i] = NULL;
                }
        }
        for (unsigned int i = 0; i < PAGE_STRESS_SLOTS; i++) {
                if (slot[i] != NULL) {
                        page_free(slot[i], slot_order[i]);
                        slot[i] = NULL;
                }
        }
        page_get_stats(&after);
        CHECK(page_stats_equal(&before, &after));

        /*
         * Take every free page one at a time, chaining them through
         * their own first words, which writes to every free page; then
         * return them all.
         */
        chain = NULL;
        count = 0;
        while ((pp = page_alloc(0, 0)) != NULL) {
                word = page_to_kva(pp);
                word[0] = (uint64_t)(uintptr_t)chain;
                chain = pp;
                count++;
        }
        CHECK(count == before.ps_free);
        page_get_stats(&after);
        CHECK(after.ps_free == 0);
        while (chain != NULL) {
                pp = chain;
                word = page_to_kva(pp);
                chain = (struct page *)(uintptr_t)word[0];
                page_free(pp, 0);
        }
        page_get_stats(&after);
        CHECK(page_stats_equal(&before, &after));
}

/* Run all self-tests and return the number of failed checks. */
int
selftest_run(void)
{
        selftest_failures = 0;
        selftest_atomic();
        selftest_prf();
        selftest_string();
        selftest_page();
        return selftest_failures + md_selftest();
}
