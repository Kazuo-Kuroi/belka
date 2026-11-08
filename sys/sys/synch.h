/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Sleeping synchronisation, after IRIX:
 *
 * - struct mutex: a sleeping lock with one owner and priority
 *   inheritance. An unlock with waiters hands ownership straight to the
 *   highest-priority waiter.
 * - struct sv: a synchronisation variable. sv_wait() queues the caller
 *   and releases the caller's lock as one step, so no wakeup is lost.
 * - struct sema: a counting semaphore (psema, vsema, cpsema).
 * - struct mrlock: a multi-reader lock. Waiting writers hold off new
 *   readers; releasing a writer admits every waiting reader at once.
 *
 * All may be used only from thread context, never from an interrupt
 * handler and never while holding a spin lock, except that vsema(),
 * sv_signal(), and sv_broadcast() may be called from anywhere.
 *
 * Priority inheritance is one level deep: a mutex owner is raised to its
 * highest waiter's priority, but the boost does not pass on to the owner
 * of a mutex that owner is itself waiting for. As in IRIX, only
 * priority-scheduled (RR and FIFO) waiters are ordered by priority and
 * cause inheritance; a time-share thread handed a mutex is instead
 * raised to PRI_TS_BOOST until it releases it.
 *
 * Lock order: a sync object's spin lock is taken before kt_lock
 * (sync object -> kthread -> run queue). Sleepers never hold both.
 */

#ifndef _SYS_SYNCH_H_
#define _SYS_SYNCH_H_

#include <stdint.h>

#include <sys/spinlock.h>

struct kthread;

/* A queue of sleeping threads, FIFO or by priority. */
struct sleepq {
        struct kthread  *sq_head;
        struct kthread  *sq_tail;
        unsigned int    sq_count;
};

#define SQ_FIFO         0
#define SQ_PRIO         1

struct mutex {
        volatile uintptr_t m_owner;     /* owner | MUTEX_WAITERS */
        struct spinlock m_lock;         /* protects m_waiters */
        struct sleepq   m_waiters;      /* by priority */
        struct mutex    *m_ownnext;     /* owner's contended mutexes */
        int             m_onlist;       /* on the owner's list */
        int             m_boosted;      /* owner holds a time-share boost */
        const char      *m_name;
};

#define MUTEX_WAITERS   ((uintptr_t)1)

struct sv {
        struct spinlock sv_lock;
        struct sleepq   sv_waiters;
        const char      *sv_name;
};

struct sema {
        struct spinlock s_lock;
        int             s_count;
        struct sleepq   s_waiters;
        const char      *s_name;
};

struct mrlock {
        struct spinlock mr_lock;
        int             mr_readers;     /* readers holding it */
        int             mr_writer;      /* a writer holds it */
        struct sleepq   mr_rwait;
        struct sleepq   mr_wwait;
        const char      *mr_name;
};

void mutex_init(struct mutex *, const char *);
void mutex_lock(struct mutex *);
int mutex_trylock(struct mutex *);
void mutex_unlock(struct mutex *);
int mutex_owned(const struct mutex *);

void sv_init(struct sv *, const char *);
void sv_wait(struct sv *, struct spinlock *, int);
void sv_wait_mutex(struct sv *, struct mutex *);
int sv_signal(struct sv *);
int sv_broadcast(struct sv *);

void sema_init(struct sema *, int, const char *);
void psema(struct sema *);
int cpsema(struct sema *);
void vsema(struct sema *);

void mrinit(struct mrlock *, const char *);
void mraccess(struct mrlock *);
void mrupdate(struct mrlock *);
int mrtryaccess(struct mrlock *);
int mrtryupdate(struct mrlock *);
void mrunlock(struct mrlock *);

/* kern_synch.c: the sleep and wakeup core. */
void sleepq_init(struct sleepq *);
void sleepq_insert(struct sleepq *, struct kthread *, int);
struct kthread *sleepq_remove_first(struct sleepq *);
void sleep_finish(void);
void wakeup_thread(struct kthread *);

#endif /* _SYS_SYNCH_H_ */
