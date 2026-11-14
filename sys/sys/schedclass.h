/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Scheduling classes.
 *
 * Each class maps its threads onto the dispatcher's global priority
 * scale, after the UnixWare class-vector organisation, with IRIX's
 * policies:
 *
 *   SCHED_TS    fair-share time sharing. Threads occupy only two
 *               dispatch levels, PRI_TS_CREDIT for jobs owed CPU time and
 *               PRI_TS_OVER for jobs over their share. Each job has a
 *               weight; every tick debits the running job's balance and
 *               once per tick the debits are handed back to all active
 *               jobs in proportion to their weights, which moves threads
 *               between the two levels. Threads of one job share a
 *               balance.
 *   SCHED_RR    fixed priority, time-sliced among equals.
 *   SCHED_FIFO  fixed priority, never time-sliced.
 *
 * RR and FIFO threads are "priority scheduled" (IRIX PS): only they are
 * ordered by priority on sleep queues, and only they start priority
 * inheritance. A time-share thread handed a mutex is raised to
 * PRI_TS_BOOST until it releases it.
 *
 * Kernel threads default to SCHED_RR; interrupt threads are SCHED_FIFO.
 */

#ifndef _SYS_SCHEDCLASS_H_
#define _SYS_SCHEDCLASS_H_

#include <stdint.h>

#define SCHED_TS        0
#define SCHED_RR        1
#define SCHED_FIFO      2
#define SCHED_NCLASS    3

/* Time-share dispatch levels on the global scale. */
#define PRI_TS_OVER     1
#define PRI_TS_CREDIT   2
#define PRI_TS_BOOST    3       /* lowest priority-scheduled level */

/* Time-share weights are indexed 1 to TS_NWEIGHT; higher is favoured. */
#define TS_NWEIGHT      40
#define TS_DEFWEIGHT    20      /* NZERO */

/* Nice values, System V's: 0 to 2 * NZERO - 1; weight TS_NWEIGHT - nice. */
#define NZERO           20
#define TS_NICEWEIGHT(n) (TS_NWEIGHT - (n))
#define USEC_PER_TICK   10000

struct kthread;

struct sched_class {
        const char      *cl_name;
        int             cl_ps;          /* priority scheduled */
        int             cl_sliced;      /* time-sliced among equals */
        /* Enter the class; param is a priority or a weight index. */
        int             (*cl_enter)(struct kthread *, int);
        void            (*cl_leave)(struct kthread *);
        /* Clock tick while running, at splhi on its processor. */
        void            (*cl_tick)(struct kthread *);
        /* The running thread used up its slice. Its kt_lock is held. */
        void            (*cl_expire)(struct kthread *);
};

/* A time-share job: threads that share one balance. */
struct tsjob {
        struct tsjob    *j_activenext;  /* on the active list */
        int             j_active;
        int             j_nthreads;
        struct kthread  *j_threads;     /* linked through kt_jobnext */
        int             j_windex;
        int64_t         j_weight;
        volatile int64_t j_acctime;     /* microseconds owed (+) or over (-) */
};

extern const struct sched_class *const sched_classes[SCHED_NCLASS];

int kthread_setclass(struct kthread *, int, int);
int kthread_getclass(const struct kthread *);
int kthread_is_ps(const struct kthread *);

/* sched_ts.c */
int64_t ts_weight(int);
void ts_init(void);
void ts_redistribute(void);
int ts_join(struct kthread *, struct kthread *);
int ts_setweight(struct kthread *, int);

/* The effective priority from base, inheritance, and boost. */
void sched_recompute(struct kthread *);

#endif /* _SYS_SCHEDCLASS_H_ */
