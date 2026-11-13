/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * SCHED_TS: fair-share time sharing, after the IRIX weighted tree.
 *
 * Every time-share thread belongs to a job, alone by default; threads
 * joined with ts_join() share one. A job has a weight from ts_weights[],
 * where each step up is about ten percent more CPU, and a signed balance
 * in microseconds.
 *
 * - Charge: each tick a job's thread runs takes USEC_PER_TICK from the
 *   job's balance and puts the job on the active list.
 * - Redistribution: once per tick, on a processor chosen by rotation,
 *   the total debit of all active jobs is handed back to them in
 *   proportion to their weights, so the balances sum to about zero.
 *   Each thread is then placed at PRI_TS_CREDIT if its job's balance is
 *   not negative, or at PRI_TS_OVER if it is. A job's credit is capped
 *   at two slices per thread, and a job in credit with nothing runnable
 *   leaves the active list.
 * - Slice end: a thread whose job is in debt when its slice runs out
 *   drops to PRI_TS_OVER at once. Redistribution does not lower a running
 *   thread; that waits for its slice to end, as in IRIX, so a running
 *   thread is not preempted by a level change of equal standing.
 * - Within a level, the run queue orders time-share threads by when they
 *   last ended a turn, so threads of one job take turns.
 *
 * Locking: wt_lock protects the active list and every job's thread
 * list; balances change atomically. The tick takes a thread's kt_lock
 * and then wt_lock (kthread -> wt_lock). Redistribution, holding
 * wt_lock, therefore only try-locks threads, and leaves a busy thread's
 * level for the next tick.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/schedclass.h>
#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#define TS_SHIFT        24              /* fixed-point fraction bits */
#define TS_ONE          ((int64_t)1 << TS_SHIFT)

static int ts_enter(struct kthread *, int);
static void ts_leave(struct kthread *);
static void ts_tick(struct kthread *);
static void ts_expire(struct kthread *);
static void ts_link(struct tsjob *, struct kthread *);
static int ts_unlink(struct kthread *);
static void ts_activate(struct tsjob *);

const struct sched_class ts_class = {
        "TS", 0, 1, ts_enter, ts_leave, ts_tick, ts_expire
};

static int64_t ts_weights[TS_NWEIGHT + 1];
static struct spinlock wt_lock = SPINLOCK_INITIALIZER("wt");
static struct tsjob *ts_active;
static struct kmem_zone *tsjob_zone;

void
ts_init(void)
{
        ts_weights[0] = 10;
        ts_weights[1] = 1000;
        for (int i = 1; i < TS_NWEIGHT; i++)
                ts_weights[i + 1] = ts_weights[i] + ts_weights[i] / 10;
        tsjob_zone = kmem_zone_init(sizeof(struct tsjob), "tsjob");
}

int64_t
ts_weight(int index)
{
        if (index < 0 || index > TS_NWEIGHT)
                return 0;
        return ts_weights[index];
}

/* Add kt to job. wt_lock is held. */
static void
ts_link(struct tsjob *job, struct kthread *kt)
{
        kt->kt_jobnext = job->j_threads;
        job->j_threads = kt;
        job->j_nthreads++;
}

/*
 * Take kt out of its job. Returns nonzero if the job is now empty, in
 * which case it is off the active list and the caller frees it. The
 * caller holds kt_lock and wt_lock.
 */
static int
ts_unlink(struct kthread *kt)
{
        struct tsjob *job = kt->kt_job;
        struct kthread **link;

        for (link = &job->j_threads; *link != NULL;
            link = &(*link)->kt_jobnext) {
                if (*link == kt) {
                        *link = kt->kt_jobnext;
                        break;
                }
        }
        kt->kt_jobnext = NULL;
        kt->kt_job = NULL;
        if (--job->j_nthreads > 0)
                return 0;
        if (job->j_active) {
                struct tsjob **jl;

                for (jl = &ts_active; *jl != NULL; jl = &(*jl)->j_activenext) {
                        if (*jl == job) {
                                *jl = job->j_activenext;
                                break;
                        }
                }
                job->j_active = 0;
        }
        return 1;
}

static int
ts_enter(struct kthread *kt, int windex)
{
        struct tsjob *job = kmem_zone_zalloc(tsjob_zone, KM_SLEEP);
        int s;

        job->j_windex = windex;
        job->j_weight = ts_weights[windex];
        s = thread_lock(kt);
        (void)spin_lock(&wt_lock);
        ts_link(job, kt);
        kt->kt_job = job;
        spin_unlock(&wt_lock, 0);
        kt->kt_basepri = PRI_TS_CREDIT;
        sched_recompute(kt);
        thread_unlock(kt, s);
        return 0;
}

static void
ts_leave(struct kthread *kt)
{
        struct tsjob *job;
        int s, empty;

        s = thread_lock(kt);
        job = kt->kt_job;
        (void)spin_lock(&wt_lock);
        empty = job != NULL && ts_unlink(kt);
        spin_unlock(&wt_lock, 0);
        thread_unlock(kt, s);
        if (empty)
                kmem_zone_free(tsjob_zone, job);
}

/*
 * Move time-share thread kt into the job of time-share thread leader, so
 * that they share one balance, as the threads of one process will.
 */
int
ts_join(struct kthread *kt, struct kthread *leader)
{
        struct tsjob *old;
        int s, empty;

        if (kt->kt_class != &ts_class || leader->kt_class != &ts_class ||
            kt == leader)
                return -1;
        s = thread_lock(kt);
        (void)spin_lock(&wt_lock);
        old = kt->kt_job;
        empty = ts_unlink(kt);
        kt->kt_job = leader->kt_job;
        ts_link(kt->kt_job, kt);
        spin_unlock(&wt_lock, 0);
        thread_unlock(kt, s);
        if (empty)
                kmem_zone_free(tsjob_zone, old);
        return 0;
}

/* Put job on the active list. kt_lock of one of its threads is held. */
static void
ts_activate(struct tsjob *job)
{
        (void)spin_lock(&wt_lock);
        if (!job->j_active) {
                job->j_activenext = ts_active;
                ts_active = job;
                job->j_active = 1;
        }
        spin_unlock(&wt_lock, 0);
}

static void
ts_tick(struct kthread *kt)
{
        struct tsjob *job = kt->kt_job;

        if (job == NULL)
                return;
        atomic_add_64((volatile uint64_t *)&job->j_acctime, -USEC_PER_TICK);
        if (!job->j_active)
                ts_activate(job);
}

static void
ts_expire(struct kthread *kt)
{
        struct tsjob *job = kt->kt_job;

        if (job != NULL && job->j_acctime < 0 &&
            kt->kt_basepri != PRI_TS_OVER) {
                kt->kt_basepri = PRI_TS_OVER;
                sched_recompute(kt);
        }
}

/* Once per tick, on one processor at a time, at splhi. */
void
ts_redistribute(void)
{
        int64_t twt = 0, bal = 0, mul;
        struct tsjob **jl;
        int s;

        if (tsjob_zone == NULL || !spin_trylock(&wt_lock, &s))
                return;
        for (struct tsjob *job = ts_active; job != NULL;
            job = job->j_activenext) {
                twt += job->j_weight;
                bal -= job->j_acctime;
        }
        if (twt == 0) {
                spin_unlock(&wt_lock, s);
                return;
        }
        mul = bal * TS_ONE / twt;

        for (jl = &ts_active; *jl != NULL; ) {
                struct tsjob *job = *jl;
                int64_t acc = job->j_acctime + job->j_weight * mul / TS_ONE;
                int64_t cap = 2 * (int64_t)job->j_nthreads * USEC_PER_TICK *
                    SCHED_SLICE;
                int level, runnable = 0;

                if (acc > cap)
                        acc = cap;
                atomic_store_64((volatile uint64_t *)&job->j_acctime,
                    (uint64_t)acc);
                level = acc >= 0 ? PRI_TS_CREDIT : PRI_TS_OVER;

                for (struct kthread *kt = job->j_threads; kt != NULL;
                    kt = kt->kt_jobnext) {
                        int ks;

                        if (kt->kt_state == KT_RUN ||
                            kt->kt_state == KT_ONPROC)
                                runnable = 1;
                        /*
                         * As in IRIX, a running thread is not lowered
                         * here; its debt takes effect when its slice ends
                         * (ts_expire). Raising is always allowed.
                         */
                        if (kt->kt_basepri == level ||
                            (level < kt->kt_basepri &&
                            kt->kt_state == KT_ONPROC) ||
                            !spin_trylock(&kt->kt_lock, &ks))
                                continue;
                        kt->kt_basepri = level;
                        sched_recompute(kt);
                        spin_unlock(&kt->kt_lock, ks);
                }

                if (acc >= 0 && !runnable) {
                        *jl = job->j_activenext;
                        job->j_active = 0;
                } else {
                        jl = &job->j_activenext;
                }
        }
        spin_unlock(&wt_lock, s);
}
