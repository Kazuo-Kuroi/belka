/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The sleep and wakeup core, synchronisation variables, and counting
 * semaphores.
 *
 * Sleeping is two-phase, so that no sleeper ever holds a sync object's
 * lock and its own kt_lock together:
 *
 *  1. Holding the object's spin lock, the sleeper queues itself with
 *     sleepq_insert(), which sets kt_wchan, and drops the lock.
 *  2. sleep_finish() takes the sleeper's kt_lock. If a waker has already
 *     dequeued it, kt_wchan is NULL and it returns at once; otherwise it
 *     marks itself asleep and switches away.
 *
 * A waker holds the object's lock, dequeues the thread (clearing
 * kt_wchan), and calls wakeup_thread(), which makes it runnable only if
 * it actually went to sleep. Because the sleeper keeps its kt_lock until
 * it is off the processor, the waker cannot run it early.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kthread.h>
#include <sys/schedclass.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/cpu.h>
#include <machine/intr.h>

void
sleepq_init(struct sleepq *sq)
{
        sq->sq_head = NULL;
        sq->sq_tail = NULL;
        sq->sq_count = 0;
}

/*
 * The key a priority-ordered queue sorts by. As in IRIX, only
 * priority-scheduled threads are ordered; time-share threads queue in
 * FIFO order behind them.
 */
static int
sleepq_key(const struct kthread *kt)
{
        return kthread_is_ps(kt) ? kt->kt_pri : PRI_IDLE;
}

/* Queue kt on sq, in FIFO or priority order. The object's lock is held. */
void
sleepq_insert(struct sleepq *sq, struct kthread *kt, int order)
{
        struct kthread *after = sq->sq_tail;

        if (order == SQ_PRIO) {
                int key = sleepq_key(kt);

                /* After the last thread of equal or higher key. */
                while (after != NULL && sleepq_key(after) < key)
                        after = after->kt_prev;
        }
        kt->kt_prev = after;
        if (after != NULL) {
                kt->kt_next = after->kt_next;
                after->kt_next = kt;
        } else {
                kt->kt_next = sq->sq_head;
                sq->sq_head = kt;
        }
        if (kt->kt_next != NULL)
                kt->kt_next->kt_prev = kt;
        else
                sq->sq_tail = kt;
        sq->sq_count++;
        kt->kt_sleepq = sq;
        kt->kt_wchan = sq;
}

/* Dequeue the first thread, marking it woken. The object's lock is held. */
struct kthread *
sleepq_remove_first(struct sleepq *sq)
{
        struct kthread *kt = sq->sq_head;

        if (kt == NULL)
                return NULL;
        sq->sq_head = kt->kt_next;
        if (sq->sq_head != NULL)
                sq->sq_head->kt_prev = NULL;
        else
                sq->sq_tail = NULL;
        sq->sq_count--;
        kt->kt_next = NULL;
        kt->kt_prev = NULL;
        kt->kt_sleepq = NULL;
        kt->kt_wchan = NULL;
        return kt;
}

/* Phase 2 of a sleep; see the comment at the top of this file. */
void
sleep_finish(void)
{
        struct kthread *kt = curthread;
        struct cpu_info *ci;
        int s;

        s = thread_lock(kt);
        ci = curcpu();
        if (ci->ci_intr_depth != 0)
                panic("sleep_finish: %s sleeps in an interrupt", kt->kt_name);
        if (ci->ci_spinlocks != 1)
                panic("sleep_finish: %s sleeps holding a spin lock",
                    kt->kt_name);
        if (kt->kt_wchan == NULL) {
                thread_unlock(kt, s);
                return;
        }
        kt->kt_state = KT_SLEEP;
        swtch(s);
}

/*
 * Make a dequeued thread runnable if it went to sleep. The caller holds
 * the lock of the object it was dequeued from.
 */
void
wakeup_thread(struct kthread *kt)
{
        int s = thread_lock(kt);

        if (kt->kt_state == KT_SLEEP)
                setrun(kt);
        thread_unlock(kt, s);
}

void
sv_init(struct sv *sv, const char *name)
{
        spinlock_init(&sv->sv_lock, "sv");
        sleepq_init(&sv->sv_waiters);
        sv->sv_name = name;
}

/*
 * Wait on sv, releasing lk, which the caller took with cookie s. Returns
 * at the caller's original interrupt level, without lk.
 */
void
sv_wait(struct sv *sv, struct spinlock *lk, int s)
{
        (void)spin_lock(&sv->sv_lock);
        sleepq_insert(&sv->sv_waiters, curthread, SQ_FIFO);
        spin_unlock(&sv->sv_lock, 0);
        spin_unlock(lk, 0);
        sleep_finish();
        splx(s);
}

/* Wait on sv, releasing mutex m. Returns without m. */
void
sv_wait_mutex(struct sv *sv, struct mutex *m)
{
        int s = spin_lock(&sv->sv_lock);

        sleepq_insert(&sv->sv_waiters, curthread, SQ_FIFO);
        spin_unlock(&sv->sv_lock, s);
        mutex_unlock(m);
        sleep_finish();
}

/* Wake one waiter. Returns 1 if there was one. */
int
sv_signal(struct sv *sv)
{
        int s = spin_lock(&sv->sv_lock);
        struct kthread *kt = sleepq_remove_first(&sv->sv_waiters);

        if (kt != NULL)
                wakeup_thread(kt);
        spin_unlock(&sv->sv_lock, s);
        return kt != NULL;
}

/* Wake every waiter. Returns how many there were. */
int
sv_broadcast(struct sv *sv)
{
        int s = spin_lock(&sv->sv_lock);
        struct kthread *kt;
        int n = 0;

        while ((kt = sleepq_remove_first(&sv->sv_waiters)) != NULL) {
                wakeup_thread(kt);
                n++;
        }
        spin_unlock(&sv->sv_lock, s);
        return n;
}

void
sema_init(struct sema *sm, int count, const char *name)
{
        spinlock_init(&sm->s_lock, "sema");
        sm->s_count = count;
        sleepq_init(&sm->s_waiters);
        sm->s_name = name;
}

/*
 * Take a unit, waiting if there is none. A vsema() with waiters hands
 * its unit straight to the first, so a woken waiter already has it.
 */
void
psema(struct sema *sm)
{
        int s = spin_lock(&sm->s_lock);

        if (sm->s_count > 0) {
                sm->s_count--;
                spin_unlock(&sm->s_lock, s);
                return;
        }
        sleepq_insert(&sm->s_waiters, curthread, SQ_FIFO);
        spin_unlock(&sm->s_lock, s);
        sleep_finish();
}

/* Take a unit if one is free. Returns 1 if it did. */
int
cpsema(struct sema *sm)
{
        int s = spin_lock(&sm->s_lock);
        int got = 0;

        if (sm->s_count > 0) {
                sm->s_count--;
                got = 1;
        }
        spin_unlock(&sm->s_lock, s);
        return got;
}

void
vsema(struct sema *sm)
{
        int s = spin_lock(&sm->s_lock);
        struct kthread *kt = sleepq_remove_first(&sm->s_waiters);

        if (kt != NULL)
                wakeup_thread(kt);
        else
                sm->s_count++;
        spin_unlock(&sm->s_lock, s);
}
