/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Self-tests for threads, the scheduler, callouts, and the sleeping
 * synchronisation primitives. They run in thread0, which is bound to
 * processor 0. Tests that need a contended processor use processor 1
 * when there is one, so that thread0 can keep watching.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/smp.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/intr.h>

#include <vm/kva.h>

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        failures++;                                     \
                }                                                       \
        } while (0)

int selftest_sched(void);

/* Every test thread posts this when it finishes. */
static struct sema done;
static volatile uint32_t stop;

static void
wait_done(int n)
{
        for (int i = 0; i < n; i++)
                psema(&done);
}

/* The processor contended tests run on. */
static int
test_cpu(void)
{
        return ncpus > 1 ? 1 : 0;
}

/* --- Thread creation, exit, and reaping. --- */

static volatile uint32_t life_count;

static void
life_thread(void *arg __unused)
{
        atomic_inc_32_nv(&life_count);
        vsema(&done);
}

static void
test_lifecycle(void)
{
        unsigned int base = kthread_count();
        struct kva_stats kb, ka;
        int waited;

        kva_get_stats(&kb);
        life_count = 0;
        for (int i = 0; i < 64; i++)
                (void)kthread_create(life_thread, NULL, PRI_KERNEL, "life");
        wait_done(64);
        CHECK(life_count == 64);
        for (waited = 0; waited < 100 && kthread_count() != base; waited++)
                delay(1);
        CHECK(kthread_count() == base);
        kva_get_stats(&ka);
        CHECK(ka.kvs_free_pages == kb.kvs_free_pages);
}

/* --- delay(), timeout(), untimeout(). --- */

static void
set_flag(void *arg)
{
        *(volatile int *)arg = 1;
}

static int callout_a, callout_b;

static void
test_callouts(void)
{
        uint64_t start, ticks;
        toid_t id;

        start = lbolt;
        delay(10);
        ticks = lbolt - start;
        CHECK(ticks >= 10 && ticks <= 12);

        callout_a = callout_b = 0;
        id = timeout(set_flag, &callout_a, 5);
        CHECK(untimeout(id) == 1);
        id = timeout(set_flag, &callout_b, 3);
        delay(8);
        CHECK(*(volatile int *)&callout_a == 0);
        CHECK(*(volatile int *)&callout_b == 1);
        CHECK(untimeout(id) == 0);
}

/* --- Semaphore ping-pong. --- */

#define PINGPONG        20000

static struct sema ping, pong;
static volatile uint32_t pongs;

static void
ping_thread(void *arg __unused)
{
        for (int i = 0; i < PINGPONG; i++) {
                vsema(&ping);
                psema(&pong);
        }
        vsema(&done);
}

static void
pong_thread(void *arg __unused)
{
        for (int i = 0; i < PINGPONG; i++) {
                psema(&ping);
                pongs++;
                vsema(&pong);
        }
        vsema(&done);
}

static void
test_pingpong(int cpu)
{
        sema_init(&ping, 0, "ping");
        sema_init(&pong, 0, "pong");
        pongs = 0;
        (void)kthread_create_bound(ping_thread, NULL, PRI_KERNEL, "ping",
            cpu);
        (void)kthread_create_bound(pong_thread, NULL, PRI_KERNEL, "pong",
            cpu);
        wait_done(2);
        CHECK(pongs == PINGPONG);
}

/* --- Mutex contention. --- */

#define MUTEX_THREADS   8
#define MUTEX_ROUNDS    20000

static struct mutex test_mutex;
static volatile uint64_t mutex_counter;

static void
mutex_thread(void *arg __unused)
{
        for (int i = 0; i < MUTEX_ROUNDS; i++) {
                uint64_t v;

                mutex_lock(&test_mutex);
                v = mutex_counter;
                for (volatile int j = 0; j < 20; j++)
                        continue;
                mutex_counter = v + 1;
                mutex_unlock(&test_mutex);
        }
        vsema(&done);
}

static void
test_mutex_contention(void)
{
        mutex_init(&test_mutex, "selftest");
        mutex_counter = 0;
        for (int i = 0; i < MUTEX_THREADS; i++)
                (void)kthread_create(mutex_thread, NULL, PRI_KERNEL, "mtx");
        wait_done(MUTEX_THREADS);
        CHECK(mutex_counter == (uint64_t)MUTEX_THREADS * MUTEX_ROUNDS);
        CHECK(mutex_trylock(&test_mutex) == 1);
        CHECK(mutex_owned(&test_mutex));
        CHECK(mutex_trylock(&test_mutex) == 0);
        mutex_unlock(&test_mutex);
        CHECK(!mutex_owned(&test_mutex));
}

/* --- Producers and consumers on a mutex and two sync variables. --- */

#define PC_ITEMS        5000    /* per producer */
#define PC_SLOTS        8

static struct mutex pc_lock;
static struct sv pc_notempty, pc_notfull;
static uint64_t pc_buf[PC_SLOTS];
static unsigned int pc_head, pc_count, pc_consumed;
static volatile uint64_t pc_sum;

static void
producer_thread(void *arg __unused)
{
        for (uint64_t i = 1; i <= PC_ITEMS; i++) {
                mutex_lock(&pc_lock);
                while (pc_count == PC_SLOTS) {
                        sv_wait_mutex(&pc_notfull, &pc_lock);
                        mutex_lock(&pc_lock);
                }
                pc_buf[(pc_head + pc_count) % PC_SLOTS] = i;
                pc_count++;
                sv_signal(&pc_notempty);
                mutex_unlock(&pc_lock);
        }
        vsema(&done);
}

static void
consumer_thread(void *arg __unused)
{
        for (;;) {
                mutex_lock(&pc_lock);
                while (pc_count == 0 && pc_consumed < 2 * PC_ITEMS) {
                        sv_wait_mutex(&pc_notempty, &pc_lock);
                        mutex_lock(&pc_lock);
                }
                if (pc_consumed == 2 * PC_ITEMS) {
                        mutex_unlock(&pc_lock);
                        break;
                }
                pc_sum += pc_buf[pc_head];
                pc_head = (pc_head + 1) % PC_SLOTS;
                pc_count--;
                if (++pc_consumed == 2 * PC_ITEMS)
                        sv_broadcast(&pc_notempty);
                sv_signal(&pc_notfull);
                mutex_unlock(&pc_lock);
        }
        vsema(&done);
}

static void
test_producer_consumer(void)
{
        mutex_init(&pc_lock, "pc");
        sv_init(&pc_notempty, "pc_notempty");
        sv_init(&pc_notfull, "pc_notfull");
        pc_head = pc_count = pc_consumed = 0;
        pc_sum = 0;
        for (int i = 0; i < 2; i++) {
                (void)kthread_create(producer_thread, NULL, PRI_KERNEL,
                    "producer");
                (void)kthread_create(consumer_thread, NULL, PRI_KERNEL,
                    "consumer");
        }
        wait_done(4);
        CHECK(pc_consumed == 2 * PC_ITEMS);
        CHECK(pc_sum == 2 * ((uint64_t)PC_ITEMS * (PC_ITEMS + 1) / 2));
}

/* --- Readers and writers on an mrlock. --- */

#define MR_ROUNDS       5000

static struct mrlock test_mr;
static volatile uint32_t mr_readers_in, mr_writers_in, mr_bad;
static volatile uint32_t mr_max_readers;

static void
mr_reader(void *arg __unused)
{
        for (int i = 0; i < MR_ROUNDS; i++) {
                uint32_t n;

                mraccess(&test_mr);
                n = atomic_inc_32_nv(&mr_readers_in);
                if (mr_writers_in != 0)
                        atomic_inc_32_nv(&mr_bad);
                if (n > mr_max_readers)
                        mr_max_readers = n;
                for (volatile int j = 0; j < 50; j++)
                        continue;
                atomic_dec_32_nv(&mr_readers_in);
                mrunlock(&test_mr);
        }
        vsema(&done);
}

static void
mr_writer(void *arg __unused)
{
        for (int i = 0; i < MR_ROUNDS; i++) {
                mrupdate(&test_mr);
                if (atomic_inc_32_nv(&mr_writers_in) != 1 ||
                    mr_readers_in != 0)
                        atomic_inc_32_nv(&mr_bad);
                for (volatile int j = 0; j < 50; j++)
                        continue;
                atomic_dec_32_nv(&mr_writers_in);
                mrunlock(&test_mr);
        }
        vsema(&done);
}

static void
test_mrlock(void)
{
        mrinit(&test_mr, "selftest");
        mr_readers_in = mr_writers_in = mr_bad = mr_max_readers = 0;
        for (int i = 0; i < 4; i++)
                (void)kthread_create(mr_reader, NULL, PRI_KERNEL, "reader");
        for (int i = 0; i < 2; i++)
                (void)kthread_create(mr_writer, NULL, PRI_KERNEL, "writer");
        wait_done(6);
        CHECK(mr_bad == 0);
        CHECK(mrtryupdate(&test_mr) == 1);
        CHECK(mrtryaccess(&test_mr) == 0);
        mrunlock(&test_mr);
        CHECK(mrtryaccess(&test_mr) == 1);
        CHECK(mrtryaccess(&test_mr) == 1);
        CHECK(mrtryupdate(&test_mr) == 0);
        mrunlock(&test_mr);
        mrunlock(&test_mr);
}

/* --- Time slicing between equal priorities. --- */

static uint64_t spin_count[2];

static void
rr_thread(void *arg)
{
        volatile uint64_t *count = arg;

        while (!stop)
                (*count)++;
        vsema(&done);
}

static void
test_round_robin(void)
{
        uint64_t a, b;

        stop = 0;
        spin_count[0] = spin_count[1] = 0;
        (void)kthread_create_bound(rr_thread, &spin_count[0], 30, "rr0",
            test_cpu());
        (void)kthread_create_bound(rr_thread, &spin_count[1], 30, "rr1",
            test_cpu());
        delay(40);
        a = *(volatile uint64_t *)&spin_count[0];
        b = *(volatile uint64_t *)&spin_count[1];
        stop = 1;
        wait_done(2);
        CHECK(a > 0 && b > 0);
}

/* --- Preemption latency. --- */

static volatile uint32_t late_wakeups;
static volatile uint64_t worst_wakeup;

static void
low_spinner(void *arg __unused)
{
        while (!stop)
                continue;
        vsema(&done);
}

static void
high_sleeper(void *arg __unused)
{
        for (int i = 0; i < 20; i++) {
                uint64_t start = lbolt;

                delay(1);
                if (lbolt - start > worst_wakeup)
                        worst_wakeup = lbolt - start;
                if (lbolt - start > 3)
                        late_wakeups++;
        }
        vsema(&done);
}

static void
test_preemption(void)
{
        stop = 0;
        late_wakeups = 0;
        worst_wakeup = 0;
        (void)kthread_create_bound(low_spinner, NULL, 10, "low",
            test_cpu());
        delay(2);
        (void)kthread_create_bound(high_sleeper, NULL, 90, "high",
            test_cpu());
        psema(&done);
        stop = 1;
        psema(&done);
        printf("selftest: preemption: worst wakeup %ju ticks, %u late\n",
            (uintmax_t)worst_wakeup, late_wakeups);
        CHECK(late_wakeups == 0);
}

/* --- Priority inheritance. --- */

static struct mutex pi_mutex;
static struct sema pi_low_has_it;
static volatile uint32_t pi_high_done;
static volatile uint64_t pi_high_at;
static volatile int pi_low_pri_after;

static void
pi_low(void *arg __unused)
{
        mutex_lock(&pi_mutex);
        vsema(&pi_low_has_it);
        drv_usecwait(50000);            /* busy, but preemptible */
        mutex_unlock(&pi_mutex);
        pi_low_pri_after = curthread->kt_pri;
        vsema(&done);
}

static void
pi_medium(void *arg __unused)
{
        while (!stop)
                continue;
        vsema(&done);
}

static void
pi_high(void *arg __unused)
{
        mutex_lock(&pi_mutex);
        pi_high_at = lbolt;
        pi_high_done = 1;
        mutex_unlock(&pi_mutex);
        vsema(&done);
}

/*
 * A low-priority thread holds a mutex that a high-priority thread needs,
 * while a medium-priority thread spins on the same processor. Without
 * inheritance the medium thread starves the low one forever; with it the
 * low one is raised, finishes, and hands over the mutex.
 */
static void
test_priority_inheritance(void)
{
        uint64_t start;
        int waited;

        mutex_init(&pi_mutex, "pi");
        sema_init(&pi_low_has_it, 0, "pi_low");
        stop = 0;
        pi_high_done = 0;

        (void)kthread_create_bound(pi_low, NULL, 20, "pi_low", test_cpu());
        psema(&pi_low_has_it);
        (void)kthread_create_bound(pi_medium, NULL, 40, "pi_med",
            test_cpu());
        delay(2);
        start = lbolt;
        (void)kthread_create_bound(pi_high, NULL, 80, "pi_high", test_cpu());
        for (waited = 0; waited < 200 && !pi_high_done; waited++)
                delay(1);
        CHECK(pi_high_done);
        if (pi_high_done)
                CHECK(pi_high_at - start < 50);
        stop = 1;
        wait_done(3);
        CHECK(pi_low_pri_after == 20);
}

/* --- Load spreads over every processor. --- */

static volatile uint64_t spread_seen[MAXCPUS / 64];

static void
spread_thread(void *arg __unused)
{
        for (int i = 0; i < 50; i++) {
                int s = splhi();
                unsigned int me = cpu_number();

                splx(s);
                atomic_or_64(&spread_seen[me / 64], UINT64_C(1) << (me % 64));
                drv_usecwait(1000);
        }
        vsema(&done);
}

static void
test_spread(void)
{
        unsigned int n = ncpus, seen = 0;

        for (unsigned int i = 0; i < MAXCPUS / 64; i++)
                spread_seen[i] = 0;
        for (unsigned int i = 0; i < 2 * n; i++)
                (void)kthread_create(spread_thread, NULL, 30, "spread");
        wait_done((int)(2 * n));
        for (unsigned int i = 0; i < n; i++)
                seen += (spread_seen[i / 64] >> (i % 64)) & 1;
        CHECK(seen == n);
}

/* --- A thread moving itself between processors. --- */

static volatile uint32_t bind_bad;

static void
bind_thread(void *arg __unused)
{
        for (unsigned int i = 0; i < ncpus; i++) {
                int s;

                kthread_bind((int)i);
                s = splhi();
                if (cpu_number() != i)
                        bind_bad++;
                splx(s);
        }
        kthread_bind(KT_NOCPU);
        vsema(&done);
}

static void
test_bind(void)
{
        bind_bad = 0;
        (void)kthread_create(bind_thread, NULL, PRI_KERNEL, "bind");
        wait_done(1);
        CHECK(bind_bad == 0);
}

int
selftest_sched(void)
{
        uint64_t start = lbolt;

        failures = 0;
        sema_init(&done, 0, "selftest_done");

        CHECK(curthread != NULL && sched_running);
        test_lifecycle();
        test_callouts();
        test_pingpong(KT_NOCPU);
        test_pingpong(test_cpu());
        test_mutex_contention();
        test_producer_consumer();
        test_mrlock();
        test_round_robin();
        test_preemption();
        test_priority_inheritance();
        test_spread();
        test_bind();

        printf("selftest: scheduler and sync tests: %ju ticks, "
            "%u threads left, %u max concurrent readers\n",
            (uintmax_t)(lbolt - start), kthread_count(), mr_max_readers);
        return failures;
}
