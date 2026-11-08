/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel threads.
 *
 * A kthread is the unit the scheduler runs, after the IRIX kthread;
 * user LWPs will be built on it. Every kthread has its own kernel stack
 * and runs until it calls kthread_exit() or returns from its function.
 *
 * Priorities run from PRI_MIN to PRI_MAX, higher being more urgent.
 * Each processor's idle thread runs below every other thread.
 *
 * Locking: kt_lock protects a thread's state, priority, run queue
 * membership, and wait channel. It is held across a context switch: the
 * thread switching away holds its own lock until the processor has left
 * its stack, so no other processor can run it early (the IRIX rule that
 * a thread's previous processor must leave its stack first).
 */

#ifndef _SYS_KTHREAD_H_
#define _SYS_KTHREAD_H_

#include <stdint.h>

#include <sys/compiler.h>
#include <sys/spinlock.h>

#include <machine/types.h>

#define PRI_MIN         0
#define PRI_MAX         127
#define NPRI            (PRI_MAX + 1)
#define PRI_KERNEL      60      /* default for kernel threads */
#define PRI_IDLE        (-1)

#define SCHED_SLICE     10      /* ticks; IRIX's default time slice */

/* kt_state */
#define KT_NEW          0       /* created, never run */
#define KT_ONPROC       1       /* running on a processor */
#define KT_RUN          2       /* runnable, on a run queue */
#define KT_SLEEP        3       /* waiting on a sync object */
#define KT_ZOMB         4       /* exited, awaiting the reaper */

/* kt_flags */
#define KTF_IDLE        0x0001  /* a processor's idle thread */
#define KTF_YIELD       0x0002  /* switching away voluntarily */
#define KTF_PREEMPTED   0x0004  /* switching away by preemption */

#define KT_NAMELEN      16
#define KT_NOCPU        (-1)

struct cpu_info;
struct mutex;
struct runq;
struct sleepq;

struct kthread {
        uint64_t        kt_rsp;         /* saved stack pointer */
        struct spinlock kt_lock;
        volatile int    kt_state;
        int             kt_flags;
        int             kt_pri;         /* current, including inherited */
        int             kt_basepri;     /* assigned */
        int             kt_slice;       /* ticks left in the time slice */
        int             kt_bindcpu;     /* KT_NOCPU, or the only cpu */

        /* Run queue or sleep queue linkage; never on both. */
        struct kthread  *kt_next;
        struct kthread  *kt_prev;
        struct runq     *kt_rq;         /* run queue holding it, or NULL */
        struct cpu_info *kt_cpu;        /* processor it last ran on */

        /* Waiting. */
        void *volatile  kt_wchan;       /* sync object, NULL once woken */
        struct sleepq   *kt_sleepq;
        struct mutex    *kt_blockedon;  /* mutex it waits for */
        struct mutex    *kt_mutexes;    /* owned mutexes with waiters */

        vaddr_t         kt_stack;       /* stack top; 0 for the boot thread */
        void            (*kt_fn)(void *);
        void            *kt_arg;

        uint64_t        kt_id;
        uint64_t        kt_nswitch;     /* times switched to */
        struct kthread  *kt_allnext;    /* all threads */
        char            kt_name[KT_NAMELEN];
};

#include <machine/cpu.h>

#define curthread       (cpu_curthread())

struct kthread *kthread_create(void (*)(void *), void *, int, const char *);
struct kthread *kthread_create_bound(void (*)(void *), void *, int,
    const char *, int);
void kthread_exit(void) __dead;
void kthread_yield(void);
void kthread_setpri(struct kthread *, int);
void kthread_bind(int);
void kthread_start(struct kthread *);
void kthread_reap(struct kthread *);
struct kthread *kthread_alloc_idle(struct cpu_info *, vaddr_t);

/* Statistics. */
unsigned int kthread_count(void);

/* kern_mutex.c: the highest priority kt inherits through its mutexes. */
int mutex_inherited_pri(const struct kthread *);

/* kern_sched.c */
void sched_init(void);
void sched_cpu_init(struct cpu_info *, struct kthread *);
void sched_idle(void) __dead;
void sched_tick(void);
void sched_preempt_check(void);
void swtch(int);
void setrun(struct kthread *);
void sched_setpri(struct kthread *, int);
int thread_lock(struct kthread *);
void thread_unlock(struct kthread *, int);
extern volatile int sched_running;

/* kern_timeout.c */
typedef int64_t toid_t;
toid_t timeout(void (*)(void *), void *, long);
int untimeout(toid_t);
void delay(long);
void callout_init(void);
void callout_tick(void);

#endif /* _SYS_KTHREAD_H_ */
