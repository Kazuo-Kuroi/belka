/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The dispatcher: per-processor run queues, context switching, wakeup
 * placement, time slicing, preemption, and idle-time work stealing.
 *
 * The design follows IRIX where it fits x86-64:
 *
 * - One run queue per processor, by priority. A thread preempted by a
 *   higher-priority one goes back at the head of its priority (IRIX
 *   sorts it as pri+1); one that yields or uses up its slice goes to
 *   the tail.
 * - The kernel is preemptible: when an interrupt arrives at spl0 and a
 *   reschedule is pending, the interrupted thread is switched out on
 *   the way back from the interrupt. Spin locks raise to splhi, so a
 *   thread holding one is never preempted.
 * - A woken thread goes back to the processor it last ran on if that
 *   one is idle, else to any idle processor, else to its last one.
 * - Idle processors steal runnable threads from the others, scanning
 *   from their right-hand neighbour. So does a busy one whose thread's
 *   time slice ends with nothing as good queued locally, taking a thread
 *   of at least the same priority: otherwise a processor could keep
 *   running a thread of low standing while better or equal ones wait
 *   behind another processor's work indefinitely.
 *
 * Scheduling classes (IRIX's fair-share time-share tree, real-time,
 * gang, batch, and the frame scheduler) are not here yet: every thread
 * has a fixed priority and threads of equal priority share by time
 * slice.
 *
 * Context switch locking: swtch() is entered holding the current
 * thread's kt_lock. It takes the next thread's kt_lock (with a try-lock,
 * because run queue locks rank below kt_lock), switches, and the next
 * thread releases both locks in sched_finish(). So a thread is never
 * picked up by another processor while it is still on its old stack.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kthread.h>
#include <sys/sched.h>
#include <sys/schedclass.h>
#include <sys/smp.h>
#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/intr.h>

volatile int sched_running;

static void runq_insert(struct runq *, struct kthread *, int);
static void runq_remove(struct runq *, struct kthread *);
static int runq_best_pri(const struct runq *);
static struct kthread *runq_take(struct runq *, int, struct cpu_info *,
    int *);
static struct cpu_info *sched_choose_cpu(struct kthread *);
static int sched_steal(struct cpu_info *, int);
static void sched_finish(void);

int
thread_lock(struct kthread *kt)
{
        return spin_lock(&kt->kt_lock);
}

void
thread_unlock(struct kthread *kt, int s)
{
        spin_unlock(&kt->kt_lock, s);
}

/* Queue kt at the head or tail of its priority. rq_lock is held. */
static void
runq_insert(struct runq *rq, struct kthread *kt, int at_head)
{
        int pri = kt->kt_pri;

        if (kt->kt_rq != NULL)
                panic("runq_insert: %s is already queued", kt->kt_name);

        if (at_head && rq->rq_head[pri] != NULL) {
                kt->kt_prev = NULL;
                kt->kt_next = rq->rq_head[pri];
                rq->rq_head[pri]->kt_prev = kt;
                rq->rq_head[pri] = kt;
        } else if (!kthread_is_ps(kt) && rq->rq_tail[pri] != NULL) {
                /*
                 * Time-share threads share two levels, and whole jobs
                 * move between them at once, so within a level they are
                 * ordered by when they last ran, least recent first.
                 * Otherwise the order of a job's thread list would decide
                 * which of its threads ran, and could starve one.
                 */
                struct kthread *after = rq->rq_tail[pri];

                while (after != NULL && after->kt_lastrun > kt->kt_lastrun)
                        after = after->kt_prev;
                kt->kt_prev = after;
                if (after != NULL) {
                        kt->kt_next = after->kt_next;
                        after->kt_next = kt;
                } else {
                        kt->kt_next = rq->rq_head[pri];
                        rq->rq_head[pri] = kt;
                }
                if (kt->kt_next != NULL)
                        kt->kt_next->kt_prev = kt;
                else
                        rq->rq_tail[pri] = kt;
        } else {
                kt->kt_next = NULL;
                kt->kt_prev = rq->rq_tail[pri];
                if (rq->rq_tail[pri] != NULL)
                        rq->rq_tail[pri]->kt_next = kt;
                else
                        rq->rq_head[pri] = kt;
                rq->rq_tail[pri] = kt;
        }
        rq->rq_bitmap[pri / 64] |= UINT64_C(1) << (pri % 64);
        rq->rq_count++;
        kt->kt_rq = rq;
}

static void
runq_remove(struct runq *rq, struct kthread *kt)
{
        int pri = kt->kt_pri;

        if (kt->kt_rq != rq ||
            (kt->kt_prev == NULL && rq->rq_head[pri] != kt) ||
            (kt->kt_next == NULL && rq->rq_tail[pri] != kt))
                panic("runq_remove: %s is not queued at priority %d",
                    kt->kt_name, pri);

        if (kt->kt_prev != NULL)
                kt->kt_prev->kt_next = kt->kt_next;
        else
                rq->rq_head[pri] = kt->kt_next;
        if (kt->kt_next != NULL)
                kt->kt_next->kt_prev = kt->kt_prev;
        else
                rq->rq_tail[pri] = kt->kt_prev;
        if (rq->rq_head[pri] == NULL)
                rq->rq_bitmap[pri / 64] &= ~(UINT64_C(1) << (pri % 64));
        kt->kt_next = NULL;
        kt->kt_prev = NULL;
        kt->kt_rq = NULL;
        rq->rq_count--;
}

/* The highest priority queued, or PRI_IDLE. Safe to call unlocked. */
static int
runq_best_pri(const struct runq *rq)
{
        for (int w = RQ_WORDS - 1; w >= 0; w--) {
                uint64_t bits = rq->rq_bitmap[w];

                for (int b = 63; b >= 0 && bits != 0; b--) {
                        if ((bits & (UINT64_C(1) << b)) != 0)
                                return w * 64 + b;
                }
        }
        return PRI_IDLE;
}

/*
 * Remove and return the best thread of at least priority minpri whose
 * lock can be taken without waiting, with that lock held; skip threads
 * bound to another processor than for. *busyp is set if a thread was
 * passed over only because its lock was held, typically by a waker
 * still inside setrun(). rq_lock is held.
 */
static struct kthread *
runq_take(struct runq *rq, int minpri, struct cpu_info *for_ci, int *busyp)
{
        for (int pri = PRI_MAX; pri >= minpri && pri >= PRI_MIN; pri--) {
                if ((rq->rq_bitmap[pri / 64] & (UINT64_C(1) << (pri % 64)))
                    == 0)
                        continue;
                for (struct kthread *kt = rq->rq_head[pri]; kt != NULL;
                    kt = kt->kt_next) {
                        int dummy;

                        if (kt->kt_bindcpu != KT_NOCPU &&
                            (unsigned int)kt->kt_bindcpu != for_ci->ci_cpuid)
                                continue;
                        if (!spin_trylock(&kt->kt_lock, &dummy)) {
                                *busyp = 1;
                                continue;
                        }
                        runq_remove(rq, kt);
                        return kt;
                }
        }
        return NULL;
}

/*
 * Switch away from the current thread. The caller holds its kt_lock,
 * taken with cookie s, and has set its state: KT_RUN to stay runnable
 * (yield or preemption), KT_SLEEP, or KT_ZOMB. Returns, with the lock
 * released and the interrupt level restored, when the thread next runs.
 */
void
swtch(int s)
{
        struct cpu_info *ci = curcpu();
        struct schedcpu *sc = &ci->ci_sched;
        struct runq *rq = &sc->sc_rq;
        struct kthread *old = ci->ci_curthread;
        struct kthread *new;
        struct cpu_info *bound = NULL;
        int busy = 0;
        int runnable = old->kt_state == KT_RUN;
        int idle = (old->kt_flags & KTF_IDLE) != 0;
        int minpri;

        sc->sc_want_resched = 0;

        /*
         * A runnable thread keeps the processor unless something better
         * waits: strictly higher priority after a preemption, equal or
         * higher after a yield. The idle thread gives way to anything.
         */
        /* A thread bound elsewhere must leave this processor. */
        if (runnable && !idle && old->kt_bindcpu != KT_NOCPU &&
            (unsigned int)old->kt_bindcpu != ci->ci_cpuid)
                bound = cpu_lookup((unsigned int)old->kt_bindcpu);

        if (!runnable || idle || bound != NULL)
                minpri = PRI_MIN;
        else if ((old->kt_flags & KTF_YIELD) != 0)
                minpri = old->kt_pri;
        else
                minpri = old->kt_pri + 1;

        (void)spin_lock(&rq->rq_lock);
        new = runq_take(rq, minpri, ci, &busy);
        if (busy) {
                /*
                 * A better thread is queued but its lock is held for a
                 * moment. Try again as soon as interrupts are enabled,
                 * rather than at the end of the time slice.
                 */
                sc->sc_want_resched = 1;
                cpu_need_resched(ci);
        }
        if (new == NULL && runnable && bound == NULL) {
                spin_unlock(&rq->rq_lock, 0);
                old->kt_state = KT_ONPROC;
                old->kt_flags &= ~(KTF_YIELD | KTF_PREEMPTED);
                thread_unlock(old, s);
                return;
        }
        if (runnable && !idle && bound == NULL) {
                runq_insert(rq, old,
                    (old->kt_flags & KTF_PREEMPTED) != 0);
                old->kt_state = KT_RUN;
        }
        spin_unlock(&rq->rq_lock, 0);
        old->kt_flags &= ~(KTF_YIELD | KTF_PREEMPTED);

        /*
         * Queue a migrating thread on its own processor. Its lock stays
         * held until we are off its stack, so that processor cannot run
         * it before then.
         */
        if (bound != NULL) {
                struct runq *brq = &bound->ci_sched.sc_rq;

                old->kt_cpu = bound;
                (void)spin_lock(&brq->rq_lock);
                runq_insert(brq, old, 0);
                spin_unlock(&brq->rq_lock, 0);
                bound->ci_sched.sc_want_resched = 1;
                cpu_need_resched(bound);
        }

        if (new == NULL) {
                int dummy;

                new = sc->sc_idle;
                if (!spin_trylock(&new->kt_lock, &dummy))
                        panic("swtch: idle thread lock held");
        }

        old->kt_lastrun = lbolt;
        new->kt_state = KT_ONPROC;
        new->kt_cpu = ci;
        new->kt_slice = SCHED_SLICE;
        new->kt_nswitch++;
        sc->sc_prev = old;
        sc->sc_nswitch++;
        ci->ci_curthread = new;

        cpu_switch_md(old, new);
        cpu_switch(&old->kt_rsp, new->kt_rsp);

        sched_finish();
        splx(s);
}

/*
 * Complete a switch on the new thread: release the previous thread's
 * lock and our own, and pass an exited thread to the reaper.
 */
static void
sched_finish(void)
{
        struct cpu_info *ci = curcpu();
        struct kthread *prev = ci->ci_sched.sc_prev;
        int zombie = prev->kt_state == KT_ZOMB;

        ci->ci_sched.sc_prev = NULL;
        spin_unlock(&prev->kt_lock, 0);
        spin_unlock(&ci->ci_curthread->kt_lock, 0);
        if (zombie)
                kthread_reap(prev);
}

/* First code of every new thread, from thread_start in locore.S. */
void
kthread_start(struct kthread *kt)
{
        sched_finish();
        spl0();
        kt->kt_fn(kt->kt_arg);
        kthread_exit();
}

/*
 * Where should runnable thread kt go? Its bound processor; else its last
 * processor if idle; else any idle processor; else its last processor.
 */
static struct cpu_info *
sched_choose_cpu(struct kthread *kt)
{
        struct cpu_info *last;
        unsigned int n = ncpus;

        if (kt->kt_bindcpu != KT_NOCPU)
                return cpu_lookup((unsigned int)kt->kt_bindcpu);
        last = kt->kt_cpu != NULL ? kt->kt_cpu : curcpu();
        if ((last->ci_curthread->kt_flags & KTF_IDLE) != 0 &&
            last->ci_sched.sc_rq.rq_count == 0)
                return last;
        for (unsigned int i = 1; i < n; i++) {
                struct cpu_info *ci = cpu_lookup((last->ci_cpuid + i) % n);

                if (ci != NULL &&
                    (ci->ci_curthread->kt_flags & KTF_IDLE) != 0 &&
                    ci->ci_sched.sc_rq.rq_count == 0)
                        return ci;
        }
        return last;
}

/*
 * Make kt runnable. The caller holds kt's lock; kt is new or asleep.
 * If kt should run before what its processor is running now, that
 * processor is told to reschedule.
 */
void
setrun(struct kthread *kt)
{
        struct cpu_info *ci = sched_choose_cpu(kt);
        struct runq *rq = &ci->ci_sched.sc_rq;
        struct kthread *running;

        kt->kt_state = KT_RUN;
        kt->kt_cpu = ci;
        (void)spin_lock(&rq->rq_lock);
        runq_insert(rq, kt, 0);
        spin_unlock(&rq->rq_lock, 0);

        running = ci->ci_curthread;
        if ((running->kt_flags & KTF_IDLE) != 0 ||
            kt->kt_pri > running->kt_pri) {
                ci->ci_sched.sc_want_resched = 1;
                cpu_need_resched(ci);
        }
}

/*
 * Change kt's current priority, moving it within its run queue and
 * rescheduling as needed. The caller holds kt's lock.
 */
void
sched_setpri(struct kthread *kt, int pri)
{
        if (pri < PRI_MIN)
                pri = PRI_MIN;
        if (pri > PRI_MAX)
                pri = PRI_MAX;
        if (pri == kt->kt_pri)
                return;

        if (kt->kt_state == KT_RUN && kt->kt_rq != NULL) {
                struct runq *rq = kt->kt_rq;
                struct cpu_info *ci = kt->kt_cpu;

                (void)spin_lock(&rq->rq_lock);
                runq_remove(rq, kt);
                kt->kt_pri = pri;
                runq_insert(rq, kt, 0);
                spin_unlock(&rq->rq_lock, 0);
                if (pri > ci->ci_curthread->kt_pri) {
                        ci->ci_sched.sc_want_resched = 1;
                        cpu_need_resched(ci);
                }
        } else if (kt->kt_state == KT_ONPROC) {
                struct cpu_info *ci = kt->kt_cpu;
                int lower = pri < kt->kt_pri;

                if (kt->kt_rq != NULL)
                        panic("sched_setpri: %s running but queued",
                            kt->kt_name);
                kt->kt_pri = pri;
                if (lower && runq_best_pri(&ci->ci_sched.sc_rq) > pri) {
                        ci->ci_sched.sc_want_resched = 1;
                        cpu_need_resched(ci);
                }
        } else {
                if (kt->kt_rq != NULL)
                        panic("sched_setpri: %s queued in state %d",
                            kt->kt_name, kt->kt_state);
                kt->kt_pri = pri;
        }
}

/*
 * Recompute kt's effective priority: its class's base priority, raised
 * by priority inheritance and, for a time-share thread holding a mutex
 * it was handed, by the time-share boost. The caller holds kt's lock.
 */
void
sched_recompute(struct kthread *kt)
{
        int pri = kt->kt_basepri;
        int inherited = mutex_inherited_pri(kt);

        if (inherited > pri)
                pri = inherited;
        if (kt->kt_tsboost > 0 && pri < PRI_TS_BOOST)
                pri = PRI_TS_BOOST;
        sched_setpri(kt, pri);
}

/*
 * Clock tick on every processor, from the timer interrupt: let the
 * running thread's class account for the tick, charge its time slice
 * unless the class is unsliced, and on one processor per tick, in
 * rotation, redistribute time-share credit.
 */
void
sched_tick(void)
{
        struct cpu_info *ci = curcpu();
        struct schedcpu *sc = &ci->ci_sched;
        struct kthread *kt = ci->ci_curthread;
        const struct sched_class *cl;
        int steal = PRI_IDLE;

        if (!sched_running || kt == NULL)
                return;
        if (ci->ci_cpuid == lbolt % ncpus)
                ts_redistribute();
        if ((kt->kt_flags & KTF_IDLE) != 0) {
                if (sc->sc_rq.rq_count != 0)
                        sc->sc_want_resched = 1;
                return;
        }

        kt->kt_ticks++;
        (void)thread_lock(kt);
        cl = kt->kt_class;
        if (cl != NULL && cl->cl_tick != NULL)
                cl->cl_tick(kt);
        if ((cl == NULL || cl->cl_sliced) && --kt->kt_slice <= 0) {
                kt->kt_slice = SCHED_SLICE;
                if (cl != NULL && cl->cl_expire != NULL)
                        cl->cl_expire(kt);
                if (runq_best_pri(&sc->sc_rq) >= kt->kt_pri) {
                        sc->sc_slice_expired = 1;
                        sc->sc_want_resched = 1;
                } else if (ncpus > 1) {
                        steal = kt->kt_pri;
                }
        }
        thread_unlock(kt, 0);
        if (steal != PRI_IDLE && sched_steal(ci, steal)) {
                sc->sc_slice_expired = 1;
                sc->sc_want_resched = 1;
        }
}

/*
 * Switch if a reschedule is pending. Called on the way out of an
 * interrupt that arrived at spl0, with interrupts disabled, and on the
 * way back to user mode (userret), with them enabled. The processor and
 * its current thread are read at splhi: with interrupts enabled the
 * caller could be preempted and moved between the two reads, and would
 * then lock another processor's thread and switch itself out unlocked.
 */
void
sched_preempt_check(void)
{
        int s = splhi();
        struct cpu_info *ci = curcpu();
        struct schedcpu *sc = &ci->ci_sched;
        struct kthread *kt = ci->ci_curthread;

        if (!sched_running || !sc->sc_want_resched || kt == NULL) {
                splx(s);
                return;
        }
        (void)thread_lock(kt);          /* at splhi already */
        kt->kt_state = KT_RUN;
        if (sc->sc_slice_expired)
                kt->kt_flags |= KTF_YIELD;
        else
                kt->kt_flags |= KTF_PREEMPTED;
        sc->sc_slice_expired = 0;
        swtch(s);
}

/*
 * Take one runnable thread of at least priority minpri from another
 * processor's queue onto ours. Returns nonzero if one was moved. Called
 * at splhi, by the idle thread or at the end of a time slice.
 */
static int
sched_steal(struct cpu_info *self, int minpri)
{
        unsigned int n = ncpus;

        for (unsigned int i = 1; i < n; i++) {
                struct cpu_info *ci = cpu_lookup((self->ci_cpuid + i) % n);
                struct runq *rq;
                struct kthread *kt;

                if (ci == NULL || ci->ci_sched.sc_rq.rq_count == 0 ||
                    runq_best_pri(&ci->ci_sched.sc_rq) < minpri)
                        continue;
                rq = &ci->ci_sched.sc_rq;
                int busy = 0;

                (void)spin_lock(&rq->rq_lock);
                kt = runq_take(rq, minpri, self, &busy);
                spin_unlock(&rq->rq_lock, 0);
                if (kt == NULL)
                        continue;

                kt->kt_cpu = self;
                rq = &self->ci_sched.sc_rq;
                (void)spin_lock(&rq->rq_lock);
                runq_insert(rq, kt, 0);
                spin_unlock(&rq->rq_lock, 0);
                spin_unlock(&kt->kt_lock, 0);
                return 1;
        }
        return 0;
}

/* Each processor's idle thread. */
void
sched_idle(void)
{
        for (;;) {
                struct cpu_info *ci;
                int s = splhi();

                ci = curcpu();
                if (ci->ci_sched.sc_rq.rq_count == 0)
                        (void)sched_steal(ci, PRI_MIN);
                if (ci->ci_sched.sc_rq.rq_count != 0) {
                        struct kthread *kt = ci->ci_curthread;

                        (void)thread_lock(kt);
                        kt->kt_state = KT_RUN;
                        swtch(s);
                        continue;
                }
                cpu_idle_wait();
                splx(s);
        }
}

void
kthread_yield(void)
{
        struct kthread *kt = curthread;
        int s = thread_lock(kt);

        kt->kt_state = KT_RUN;
        kt->kt_flags |= KTF_YIELD;
        swtch(s);
}

/*
 * Set up the scheduler state of processor ci, whose idle thread is idle.
 * For the bootstrap processor, the current context becomes thread0.
 */
void
sched_cpu_init(struct cpu_info *ci, struct kthread *idle)
{
        struct schedcpu *sc = &ci->ci_sched;

        spinlock_init(&sc->sc_rq.rq_lock, "runq");
        sc->sc_idle = idle;
        idle->kt_cpu = ci;
}
