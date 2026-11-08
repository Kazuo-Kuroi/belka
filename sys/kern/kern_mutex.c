/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Sleeping mutexes with priority inheritance, after IRIX.
 *
 * m_owner holds the owning thread, with MUTEX_WAITERS set in its low bit
 * while anyone waits. Uncontended lock and unlock are a single
 * compare-and-swap each. A waiter sets MUTEX_WAITERS under m_lock, so
 * the owner's fast unlock then fails and it takes the slow path, which
 * hands the mutex straight to the highest-priority waiter.
 *
 * Inheritance: a waiter raises the owner to its own priority if higher.
 * Each thread keeps a list of the contended mutexes it owns; when it
 * releases one, its priority drops back to the highest of its base
 * priority and the first waiters of the mutexes it still holds. The new
 * owner inherits from the waiters that remain. Inheritance is one level
 * deep; see <sys/synch.h>.
 *
 * Lock order: m_lock, then the owner's or waiter's kt_lock, then a run
 * queue lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kthread.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#define OWNER(v)        ((struct kthread *)((v) & ~MUTEX_WAITERS))

static void mutex_lock_slow(struct mutex *);
static void mutex_unlock_slow(struct mutex *);
static void mutex_list_remove(struct kthread *, struct mutex *);

void
mutex_init(struct mutex *m, const char *name)
{
        m->m_owner = 0;
        spinlock_init(&m->m_lock, "mutex");
        sleepq_init(&m->m_waiters);
        m->m_ownnext = NULL;
        m->m_onlist = 0;
        m->m_name = name;
}

/*
 * The highest priority kt inherits through the mutexes it owns, or
 * PRI_IDLE. The caller holds kt's lock. The waiter queues are read
 * without their locks; a stale answer is corrected at the next change.
 */
int
mutex_inherited_pri(const struct kthread *kt)
{
        int best = PRI_IDLE;

        for (const struct mutex *m = kt->kt_mutexes; m != NULL;
            m = m->m_ownnext) {
                const struct kthread *w = m->m_waiters.sq_head;

                if (w != NULL && w->kt_pri > best)
                        best = w->kt_pri;
        }
        return best;
}

/* Take m off kt's list of contended mutexes. kt's lock is held. */
static void
mutex_list_remove(struct kthread *kt, struct mutex *m)
{
        struct mutex **link;

        for (link = &kt->kt_mutexes; *link != NULL;
            link = &(*link)->m_ownnext) {
                if (*link == m) {
                        *link = m->m_ownnext;
                        break;
                }
        }
        m->m_ownnext = NULL;
        m->m_onlist = 0;
}

void
mutex_lock(struct mutex *m)
{
        uintptr_t self = (uintptr_t)curthread;

        if (atomic_cas_64((volatile uint64_t *)&m->m_owner, 0, self) == 0) {
                membar_enter();
                return;
        }
        mutex_lock_slow(m);
}

static void
mutex_lock_slow(struct mutex *m)
{
        struct kthread *self = curthread;
        struct kthread *owner;
        uintptr_t v;
        int s, os;

        if (OWNER(m->m_owner) == self)
                panic("mutex_lock: %s already held by %s", m->m_name,
                    self->kt_name);

        s = spin_lock(&m->m_lock);
        for (;;) {
                v = m->m_owner;
                if (v == 0) {
                        if (atomic_cas_64((volatile uint64_t *)&m->m_owner,
                            0, (uintptr_t)self) == 0) {
                                spin_unlock(&m->m_lock, s);
                                membar_enter();
                                return;
                        }
                        continue;
                }
                if ((v & MUTEX_WAITERS) != 0 ||
                    atomic_cas_64((volatile uint64_t *)&m->m_owner, v,
                    v | MUTEX_WAITERS) == v)
                        break;
        }
        owner = OWNER(v);

        self->kt_blockedon = m;
        sleepq_insert(&m->m_waiters, self, SQ_PRIO);

        /*
         * The owner cannot release m without m_lock, which we hold, so
         * it stays valid here.
         */
        os = thread_lock(owner);
        if (!m->m_onlist) {
                m->m_ownnext = owner->kt_mutexes;
                owner->kt_mutexes = m;
                m->m_onlist = 1;
        }
        if (self->kt_pri > owner->kt_pri)
                sched_setpri(owner, self->kt_pri);
        thread_unlock(owner, os);
        spin_unlock(&m->m_lock, s);

        sleep_finish();
        if (OWNER(m->m_owner) != self)
                panic("mutex_lock: %s woken without %s", self->kt_name,
                    m->m_name);
        membar_enter();
}

int
mutex_trylock(struct mutex *m)
{
        if (atomic_cas_64((volatile uint64_t *)&m->m_owner, 0,
            (uintptr_t)curthread) == 0) {
                membar_enter();
                return 1;
        }
        return 0;
}

void
mutex_unlock(struct mutex *m)
{
        uintptr_t self = (uintptr_t)curthread;

        membar_exit();
        if (atomic_cas_64((volatile uint64_t *)&m->m_owner, self, 0) ==
            self)
                return;
        mutex_unlock_slow(m);
}

static void
mutex_unlock_slow(struct mutex *m)
{
        struct kthread *self = curthread;
        struct kthread *next;
        int more, s, ks, inherited;

        if (OWNER(m->m_owner) != self)
                panic("mutex_unlock: %s not held by %s", m->m_name,
                    self->kt_name);

        s = spin_lock(&m->m_lock);
        next = sleepq_remove_first(&m->m_waiters);
        if (next == NULL)
                panic("mutex_unlock: %s marked contended with no waiters",
                    m->m_name);
        more = m->m_waiters.sq_count != 0;
        next->kt_blockedon = NULL;
        atomic_store_64((volatile uint64_t *)&m->m_owner,
            (uintptr_t)next | (more ? MUTEX_WAITERS : 0));

        /* Give up what we inherited through m. */
        ks = thread_lock(self);
        mutex_list_remove(self, m);
        inherited = mutex_inherited_pri(self);
        sched_setpri(self, self->kt_basepri > inherited ?
            self->kt_basepri : inherited);
        thread_unlock(self, ks);

        /* The new owner inherits from the waiters still queued. */
        ks = thread_lock(next);
        if (more) {
                int pri = m->m_waiters.sq_head->kt_pri;

                m->m_ownnext = next->kt_mutexes;
                next->kt_mutexes = m;
                m->m_onlist = 1;
                if (pri > next->kt_pri)
                        sched_setpri(next, pri);
        }
        thread_unlock(next, ks);

        wakeup_thread(next);
        spin_unlock(&m->m_lock, s);
}

int
mutex_owned(const struct mutex *m)
{
        return OWNER(m->m_owner) == curthread;
}
