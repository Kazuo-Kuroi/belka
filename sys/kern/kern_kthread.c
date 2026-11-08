/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel thread creation, exit, and reaping.
 *
 * A thread cannot free the stack it is running on, so an exiting thread
 * becomes a zombie: once the processor has switched to the next thread,
 * sched_finish() hands it to the reaper thread, which frees its stack
 * and its structure. Freeing a stack unmaps it, which needs a TLB
 * shootdown, which a thread can do and a context switch cannot.
 *
 * The bootstrap processor's start-up context becomes thread0, which
 * keeps the boot stack. Each processor has an idle thread; on secondary
 * processors the start-up context becomes the idle thread.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/sched.h>
#include <sys/schedclass.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/intr.h>

#include <vm/kva.h>

static struct kmem_zone *kthread_zone;
static struct spinlock allthreads_lock = SPINLOCK_INITIALIZER("allthreads");
static struct kthread *allthreads;
static volatile uint32_t nthreads;
static volatile uint64_t next_tid;

static struct spinlock zombie_lock = SPINLOCK_INITIALIZER("zombies");
static struct kthread *zombies;
static struct sema reaper_sema;

static struct kthread thread0;

static void kthread_init_fields(struct kthread *, const char *, int);
static struct kthread *kthread_alloc(const char *, int);
static void kthread_free(struct kthread *);
static void reaper(void *);
static void idle_entry(void *);

static void
kthread_init_fields(struct kthread *kt, const char *name, int pri)
{
        size_t i;

        memset(kt, 0, sizeof(*kt));
        spinlock_init(&kt->kt_lock, "kthread");
        kt->kt_state = KT_NEW;
        kt->kt_pri = pri;
        kt->kt_basepri = pri;
        kt->kt_slice = SCHED_SLICE;
        kt->kt_bindcpu = KT_NOCPU;
        kt->kt_class = sched_classes[SCHED_RR];
        kt->kt_id = atomic_inc_64_nv(&next_tid);
        for (i = 0; i + 1 < KT_NAMELEN && name[i] != '\0'; i++)
                kt->kt_name[i] = name[i];
        kt->kt_name[i] = '\0';
}

static struct kthread *
kthread_alloc(const char *name, int pri)
{
        struct kthread *kt = kmem_zone_alloc(kthread_zone, KM_SLEEP);
        int s;

        kthread_init_fields(kt, name, pri);
        s = spin_lock(&allthreads_lock);
        kt->kt_allnext = allthreads;
        allthreads = kt;
        spin_unlock(&allthreads_lock, s);
        atomic_inc_32_nv(&nthreads);
        return kt;
}

static void
kthread_free(struct kthread *kt)
{
        struct kthread **link;
        int s;

        s = spin_lock(&allthreads_lock);
        for (link = &allthreads; *link != NULL; link = &(*link)->kt_allnext) {
                if (*link == kt) {
                        *link = kt->kt_allnext;
                        break;
                }
        }
        spin_unlock(&allthreads_lock, s);
        kstack_free(kt->kt_stack);
        kmem_zone_free(kthread_zone, kt);
        /* Last, so a count back at its old value means all is freed. */
        atomic_dec_32_nv(&nthreads);
}

/*
 * Create a thread that runs fn(arg) at priority pri, bound to processor
 * cpu unless cpu is KT_NOCPU, and make it runnable.
 */
struct kthread *
kthread_create_bound(void (*fn)(void *), void *arg, int pri,
    const char *name, int cpu)
{
        struct kthread *kt;
        int s;

        if (pri < PRI_TS_BOOST || pri > PRI_MAX)
                panic("kthread_create: %s: bad priority %d", name, pri);
        kt = kthread_alloc(name, pri);
        kt->kt_bindcpu = cpu;
        kt->kt_fn = fn;
        kt->kt_arg = arg;
        kt->kt_stack = kstack_alloc(KM_SLEEP);
        cpu_thread_setup(kt);

        s = thread_lock(kt);
        setrun(kt);
        thread_unlock(kt, s);
        return kt;
}

struct kthread *
kthread_create(void (*fn)(void *), void *arg, int pri, const char *name)
{
        return kthread_create_bound(fn, arg, pri, name, KT_NOCPU);
}

void
kthread_exit(void)
{
        struct kthread *kt = curthread;
        int s;

        if (kt == &thread0 || (kt->kt_flags & KTF_IDLE) != 0)
                panic("kthread_exit: %s may not exit", kt->kt_name);
        if (kt->kt_mutexes != NULL || kt->kt_tsboost != 0)
                panic("kthread_exit: %s exits holding contended mutexes",
                    kt->kt_name);
        if (kt->kt_class != NULL && kt->kt_class->cl_leave != NULL)
                kt->kt_class->cl_leave(kt);
        s = thread_lock(kt);
        kt->kt_state = KT_ZOMB;
        swtch(s);
        panic("kthread_exit: zombie %s ran again", kt->kt_name);
}

/* Called from sched_finish(), at splhi, with no locks held. */
void
kthread_reap(struct kthread *kt)
{
        int s = spin_lock(&zombie_lock);

        kt->kt_next = zombies;
        zombies = kt;
        spin_unlock(&zombie_lock, s);
        vsema(&reaper_sema);
}

static void
reaper(void *arg __unused)
{
        for (;;) {
                struct kthread *kt;
                int s;

                psema(&reaper_sema);
                s = spin_lock(&zombie_lock);
                kt = zombies;
                if (kt != NULL)
                        zombies = kt->kt_next;
                spin_unlock(&zombie_lock, s);
                if (kt != NULL)
                        kthread_free(kt);
        }
}

/*
 * Change a fixed-priority thread's assigned priority; inheritance may
 * keep it higher. Time-share threads are placed by their class and
 * ignore this.
 */
void
kthread_setpri(struct kthread *kt, int pri)
{
        int s;

        if (!kthread_is_ps(kt) || pri < PRI_TS_BOOST || pri > PRI_MAX)
                return;
        s = thread_lock(kt);
        kt->kt_basepri = pri;
        sched_recompute(kt);
        thread_unlock(kt, s);
}

/*
 * Bind the calling thread to processor cpu, or unbind it with KT_NOCPU,
 * and move it there.
 */
void
kthread_bind(int cpu)
{
        struct kthread *kt = curthread;
        int s = thread_lock(kt);

        kt->kt_bindcpu = cpu;
        thread_unlock(kt, s);
        if (cpu != KT_NOCPU) {
                s = splhi();
                if (cpu_number() != (unsigned int)cpu) {
                        splx(s);
                        kthread_yield();
                } else {
                        splx(s);
                }
        }
}

unsigned int
kthread_count(void)
{
        return atomic_load_32(&nthreads);
}

static void
idle_entry(void *arg __unused)
{
        sched_idle();
}

/*
 * Make an idle thread for processor ci. With stack_top 0 it gets a new
 * stack and starts in sched_idle() at its first switch; otherwise the
 * caller's current context, on that stack, becomes the idle thread.
 */
struct kthread *
kthread_alloc_idle(struct cpu_info *ci, vaddr_t stack_top)
{
        char name[KT_NAMELEN];
        struct kthread *kt;

        snprintf(name, sizeof(name), "idle%u", ci->ci_cpuid);
        kt = kthread_alloc(name, PRI_MIN);
        kt->kt_pri = PRI_IDLE;
        kt->kt_basepri = PRI_IDLE;
        kt->kt_flags = KTF_IDLE;
        kt->kt_class = NULL;
        kt->kt_bindcpu = (int)ci->ci_cpuid;
        if (stack_top == 0) {
                kt->kt_fn = idle_entry;
                kt->kt_stack = kstack_alloc(KM_SLEEP);
                cpu_thread_setup(kt);
                kt->kt_state = KT_RUN;
        } else {
                kt->kt_stack = stack_top;
                kt->kt_state = KT_ONPROC;
        }
        sched_cpu_init(ci, kt);
        return kt;
}

/*
 * Start the scheduler on the bootstrap processor. The calling context
 * becomes thread0, bound to processor 0.
 */
void
sched_init(void)
{
        struct cpu_info *ci = curcpu();
        int s;

        kthread_zone = kmem_zone_init(sizeof(struct kthread), "kthread");
        sema_init(&reaper_sema, 0, "reaper");
        callout_init();
        ts_init();

        kthread_init_fields(&thread0, "thread0", PRI_KERNEL);
        thread0.kt_state = KT_ONPROC;
        thread0.kt_bindcpu = 0;
        thread0.kt_cpu = ci;
        s = spin_lock(&allthreads_lock);
        thread0.kt_allnext = allthreads;
        allthreads = &thread0;
        spin_unlock(&allthreads_lock, s);
        atomic_inc_32_nv(&nthreads);

        (void)kthread_alloc_idle(ci, 0);
        ci->ci_curthread = &thread0;
        sched_running = 1;

        (void)kthread_create(reaper, NULL, PRI_KERNEL, "reaper");
}
