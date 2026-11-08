/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The fixed-priority classes, SCHED_RR and SCHED_FIFO, and the class
 * switch. A fixed-priority thread's base priority is the one it was
 * given; RR threads of equal priority share by time slice, FIFO threads
 * keep the processor until they block, yield, or are preempted by a
 * higher priority.
 */

#include <stddef.h>

#include <sys/kthread.h>
#include <sys/schedclass.h>
#include <sys/systm.h>

static int fixed_enter(struct kthread *, int);

static const struct sched_class rr_class = {
        "RR", 1, 1, fixed_enter, NULL, NULL, NULL
};

static const struct sched_class fifo_class = {
        "FIFO", 1, 0, fixed_enter, NULL, NULL, NULL
};

extern const struct sched_class ts_class;

const struct sched_class *const sched_classes[SCHED_NCLASS] = {
        &ts_class, &rr_class, &fifo_class
};

/* Priority-scheduled levels lie above the time-share levels. */
static int
fixed_enter(struct kthread *kt, int pri)
{
        int s;

        if (pri < PRI_TS_BOOST || pri > PRI_MAX)
                return -1;
        s = thread_lock(kt);
        kt->kt_basepri = pri;
        sched_recompute(kt);
        thread_unlock(kt, s);
        return 0;
}

/*
 * Move kt to class cls with parameter param: a priority for RR and
 * FIFO, a weight index for TS. Returns 0, or -1 if the parameter is out
 * of range, leaving the thread unchanged. Thread context only.
 */
int
kthread_setclass(struct kthread *kt, int cls, int param)
{
        const struct sched_class *old = kt->kt_class;
        const struct sched_class *new;
        int s;

        if (cls < 0 || cls >= SCHED_NCLASS || (kt->kt_flags & KTF_IDLE) != 0)
                return -1;
        new = sched_classes[cls];
        if (cls == SCHED_TS ? (param < 1 || param > TS_NWEIGHT) :
            (param < PRI_TS_BOOST || param > PRI_MAX))
                return -1;

        if (old != NULL && old->cl_leave != NULL)
                old->cl_leave(kt);
        s = thread_lock(kt);
        kt->kt_class = new;
        thread_unlock(kt, s);
        if (new->cl_enter(kt, param) != 0)
                panic("kthread_setclass: %s refused %d", new->cl_name, param);
        return 0;
}

int
kthread_getclass(const struct kthread *kt)
{
        for (int i = 0; i < SCHED_NCLASS; i++) {
                if (kt->kt_class == sched_classes[i])
                        return i;
        }
        return -1;
}

int
kthread_is_ps(const struct kthread *kt)
{
        return kt->kt_class == NULL || kt->kt_class->cl_ps;
}
