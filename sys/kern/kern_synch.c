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
 *     let it go, kt_wchan is NULL and it returns at once; otherwise it
 *     marks itself asleep and switches away.
 *
 * A waker holds the object's lock and dequeues the thread, which leaves
 * kt_wchan set, so the sleeper cannot get past sleep_finish() yet. It
 * drops the object's lock, keeping interrupts blocked so that it is not
 * preempted before the wakeup, and calls wakeup_thread(). That clears
 * kt_wchan under the sleeper's kt_lock and makes the sleeper runnable if
 * it actually went to sleep. So:
 *
 *  - the sleeper keeps its kt_lock until it is off the processor, so the
 *    waker cannot run it early;
 *  - clearing kt_wchan and waking happen in one step, so a slow waker can
 *    never wake the thread out of some later, unrelated sleep;
 *  - the waker is done with the object before the sleeper can return, so
 *    the sleeper may free the object, or let it go out of scope, at once.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kthread.h>
#include <sys/schedclass.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>
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
                        after = after->kt_sqprev;
        }
        kt->kt_sqprev = after;
        if (after != NULL) {
                kt->kt_sqnext = after->kt_sqnext;
                after->kt_sqnext = kt;
        } else {
                kt->kt_sqnext = sq->sq_head;
                sq->sq_head = kt;
        }
        if (kt->kt_sqnext != NULL)
                kt->kt_sqnext->kt_sqprev = kt;
        else
                sq->sq_tail = kt;
        sq->sq_count++;
        kt->kt_sleepq = sq;
        kt->kt_wchan = sq;
}

/*
 * Dequeue the first thread. The object's lock is held. The thread stays
 * blocked until the caller passes it to wakeup_thread().
 */
struct kthread *
sleepq_remove_first(struct sleepq *sq)
{
        struct kthread *kt = sq->sq_head;

        if (kt == NULL)
                return NULL;
        sq->sq_head = kt->kt_sqnext;
        if (sq->sq_head != NULL)
                sq->sq_head->kt_sqprev = NULL;
        else
                sq->sq_tail = NULL;
        sq->sq_count--;
        kt->kt_sqnext = NULL;
        kt->kt_sqprev = NULL;
        kt->kt_sleepq = NULL;
        return kt;
}

/* Take kt off sq, wherever it is. The object's lock is held. */
void
sleepq_remove(struct sleepq *sq, struct kthread *kt)
{
        if (kt->kt_sqprev != NULL)
                kt->kt_sqprev->kt_sqnext = kt->kt_sqnext;
        else
                sq->sq_head = kt->kt_sqnext;
        if (kt->kt_sqnext != NULL)
                kt->kt_sqnext->kt_sqprev = kt->kt_sqprev;
        else
                sq->sq_tail = kt->kt_sqprev;
        sq->sq_count--;
        kt->kt_sqnext = NULL;
        kt->kt_sqprev = NULL;
        kt->kt_sleepq = NULL;
}

/*
 * Dequeue every thread, returning them chained through kt_sqnext. The
 * object's lock is held. Pass the chain to wakeup_threads().
 */
struct kthread *
sleepq_remove_all(struct sleepq *sq)
{
        struct kthread *kt = sq->sq_head;

        for (struct kthread *t = kt; t != NULL; t = t->kt_sqnext) {
                t->kt_sqprev = NULL;
                t->kt_sleepq = NULL;
        }
        sq->sq_head = NULL;
        sq->sq_tail = NULL;
        sq->sq_count = 0;
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
 * Let a dequeued thread go: mark it woken, and make it runnable if it
 * went to sleep. The caller has dropped the object's lock, and must not
 * touch the object afterwards.
 */
void
wakeup_thread(struct kthread *kt)
{
        int s = thread_lock(kt);

        kt->kt_wchan = NULL;
        if (kt->kt_state == KT_SLEEP)
                setrun(kt);
        thread_unlock(kt, s);
}

/*
 * Let go every thread of a chain from sleepq_remove_all(). Returns how
 * many there were.
 */
int
wakeup_threads(struct kthread *kt)
{
        int n = 0;

        while (kt != NULL) {
                /* Once let go, the thread may reuse kt_sqnext. */
                struct kthread *next = kt->kt_sqnext;

                kt->kt_sqnext = NULL;
                wakeup_thread(kt);
                kt = next;
                n++;
        }
        return n;
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

/*
 * Interruptible waits. The sleeper records its object's lock in
 * kt_sqlock while queued; sleep_interrupt() takes that lock, takes the
 * thread off the queue, and wakes it with kt_interrupted set.
 *
 * A signal that arrives while the thread is falling asleep is not lost:
 * the poster sets kt_sigpend before it looks at kt_sqlock, and the
 * sleeper sets kt_sqlock before it looks at kt_sigpend, each with a full
 * barrier in between, so at least one of them sees the other.
 *
 * Only objects that live as long as the sleeper's process may be slept
 * on this way, since sleep_interrupt() may take the lock of an object
 * the thread has just left.
 */
static int
sv_sleep_sig(struct sv *sv, struct spinlock *lk, struct mutex *m, int s)
{
        struct kthread *kt = curthread;
        int interrupted;

        (void)spin_lock(&sv->sv_lock);
        sleepq_insert(&sv->sv_waiters, kt, SQ_FIFO);
        kt->kt_interrupted = 0;
        kt->kt_sqlock = &sv->sv_lock;
        membar_sync();
        if (kt->kt_sigpend) {
                sleepq_remove(&sv->sv_waiters, kt);
                kt->kt_wchan = NULL;
                kt->kt_sqlock = NULL;
                spin_unlock(&sv->sv_lock, 0);
                if (lk != NULL)
                        spin_unlock(lk, 0);
                splx(s);
                if (m != NULL)
                        mutex_unlock(m);
                return EINTR;
        }
        spin_unlock(&sv->sv_lock, 0);
        if (lk != NULL)
                spin_unlock(lk, 0);
        if (m != NULL) {
                splx(s);
                mutex_unlock(m);
                s = splhi();
        }
        sleep_finish();
        (void)spin_lock(&sv->sv_lock);
        kt->kt_sqlock = NULL;
        interrupted = kt->kt_interrupted;
        spin_unlock(&sv->sv_lock, s);
        return interrupted ? EINTR : 0;
}

/*
 * sv_wait(), but a signal for the thread (kt_sigpend) ends the wait with
 * EINTR. Returns 0 if woken normally.
 */
int
sv_wait_sig(struct sv *sv, struct spinlock *lk, int s)
{
        return sv_sleep_sig(sv, lk, NULL, s);
}

int
sv_wait_mutex_sig(struct sv *sv, struct mutex *m)
{
        return sv_sleep_sig(sv, NULL, m, splhi());
}

/*
 * End kt's interruptible wait, if it is in one. Callable from any
 * context; the caller may hold spin locks that rank above sync object
 * locks (a process's signal lock).
 */
void
sleep_interrupt(struct kthread *kt)
{
        struct spinlock *lk;
        int s;

        membar_sync();
        if ((lk = kt->kt_sqlock) == NULL)
                return;
        s = spin_lock(lk);
        if (kt->kt_sqlock == lk && kt->kt_sleepq != NULL) {
                sleepq_remove(kt->kt_sleepq, kt);
                kt->kt_interrupted = 1;
                spin_unlock(lk, 0);
                wakeup_thread(kt);
                splx(s);
                return;
        }
        spin_unlock(lk, s);
}

/* Wake one waiter. Returns 1 if there was one. */
int
sv_signal(struct sv *sv)
{
        int s = spin_lock(&sv->sv_lock);
        struct kthread *kt = sleepq_remove_first(&sv->sv_waiters);

        spin_unlock(&sv->sv_lock, 0);
        if (kt != NULL)
                wakeup_thread(kt);
        splx(s);
        return kt != NULL;
}

/* Wake every waiter. Returns how many there were. */
int
sv_broadcast(struct sv *sv)
{
        int s = spin_lock(&sv->sv_lock);
        struct kthread *kt = sleepq_remove_all(&sv->sv_waiters);
        int n;

        spin_unlock(&sv->sv_lock, 0);
        n = wakeup_threads(kt);
        splx(s);
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

        if (kt == NULL)
                sm->s_count++;
        spin_unlock(&sm->s_lock, 0);
        if (kt != NULL)
                wakeup_thread(kt);
        splx(s);
}
