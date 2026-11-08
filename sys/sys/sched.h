/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Per-processor scheduler state, embedded in struct cpu_info.
 *
 * Each processor has one run queue: a list per priority and a bitmap of
 * the non-empty lists, so the highest-priority thread is found in
 * constant time.
 *
 * Locking: rq_lock protects the run queue. It is taken after the kt_lock
 * of a thread being queued (kthread -> run queue); code that holds a
 * run queue lock and wants a thread's lock must use a try-lock.
 */

#ifndef _SYS_SCHED_H_
#define _SYS_SCHED_H_

#include <stdint.h>

#include <sys/spinlock.h>

#define RQ_WORDS        2       /* bitmap words for NPRI priorities */

struct kthread;

struct runq {
        struct spinlock rq_lock;
        volatile unsigned int rq_count;
        uint64_t        rq_bitmap[RQ_WORDS];
        struct kthread  *rq_head[128];
        struct kthread  *rq_tail[128];
};

struct schedcpu {
        struct runq     sc_rq;
        struct kthread  *sc_idle;       /* this processor's idle thread */
        struct kthread  *sc_prev;       /* thread switched away from */
        volatile int    sc_want_resched;
        volatile int    sc_slice_expired;
        uint64_t        sc_nswitch;
};

#endif /* _SYS_SCHED_H_ */
