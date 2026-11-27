/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Interrupt dispatch, the clock tick, and cross-processor actions.
 *
 * Interrupts arrive through interrupt gates, so handlers run with
 * interrupts disabled and never nest. On the way out of an interrupt
 * that arrived at spl0, trap() lets the scheduler preempt the
 * interrupted thread. Device interrupts normally wake interrupt threads,
 * as in IRIX, so that device drivers can block. A message-signalled
 * source and its thread run on the same processor.
 *
 * Cross-processor actions follow the IRIX design: each processor has a
 * FIFO of function calls that others post with cpu_action(), followed
 * by an IPI. The target runs them in order at splhi. smp_call_all() is
 * built on them. Any processor that waits for another to act keeps
 * draining its own queue while it waits, so two processors that wait
 * on each other, even with interrupts disabled, both make progress.
 *
 * Halting the system uses NMIs, which reach processors even when they
 * have interrupts disabled.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/intr.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/random.h>
#include <sys/schedclass.h>
#include <sys/smp.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/acpi_machdep.h>
#include <machine/bootinfo.h>
#include <machine/frame.h>
#include <machine/intr.h>
#include <machine/intrdefs.h>
#include <machine/ioapic.h>
#include <machine/lapic.h>
#include <machine/segments.h>
#include <machine/trap.h>

#include "dev/acpi/acpivar.h"

/* An smp_call_all() in progress. It lives on the caller's stack. */
struct smp_call {
        void            (*sc_fn)(void *);
        void            *sc_arg;
        volatile uint32_t sc_pending;   /* processors yet to finish */
};

/*
 * A device interrupt source: one GSI on one vector, with its handlers
 * and, unless it is fast, its interrupt thread.
 */
struct intrhand {
        struct intrhand *ih_next;
        int             (*ih_fn)(void *);
        void            *ih_arg;
        struct intrsource *ih_src;
        volatile uint64_t ih_count;
        const char      *ih_name;
};

struct intrsource {
        uint32_t        is_gsi;
        unsigned int    is_cpu;        /* destination for MSI sources */
        int             is_vector;
        int             is_level;
        int             is_active_low;
        int             is_fast;
        struct intrhand *volatile is_handlers;
        struct sema     is_sema;        /* wakes the thread */
        struct kthread  *is_thread;
        volatile uint32_t is_busy;      /* thread running handlers */
        volatile uint64_t is_count;     /* hardware interrupts */
        volatile uint32_t is_badcpu;    /* MSI affinity violations */
};

#define INTR_NOGSI      UINT32_MAX      /* message-signalled source */

/* MSI message address: the local APIC window, physical destination. */
#define MSI_ADDR_BASE   UINT64_C(0xfee00000)
#define MSI_ADDR_DEST_SHIFT 12
#define MSI_ADDR_BROADCAST 0xffU

static struct intrsource *volatile intr_sources[NIDT];
static int intr_next_vector = DEVICE_VECTOR_FIRST;
static unsigned int intr_msi_next_cpu = 1; /* start off the BSP on SMP */
static struct mutex intr_mutex;
static volatile uint32_t cpus_halting;
static volatile uint32_t stray_reported;

static void smp_call_run(void *);
static void intr_device(struct intrsource *, struct cpu_info *);
static void intr_thread(void *);
static void intr_nop(void *);

/*
 * Set up interrupts on the bootstrap processor: the ACPI tables that
 * describe the interrupt controllers, the local APIC, the I/O APICs,
 * clock calibration, and the periodic tick. Interrupts stay disabled
 * until the caller enables them.
 */
void
intr_init(void)
{
        if (intr_stubs_end - intr_stubs !=
            (NIDT - NEXCEPTIONS) * INTR_STUB_SIZE)
                panic("intr_init: interrupt stubs are %td bytes, expected %d",
                    intr_stubs_end - intr_stubs,
                    (NIDT - NEXCEPTIONS) * INTR_STUB_SIZE);

        spinlock_init(&cpu_info_primary.ci_act_lock, "cpu_act");
        mutex_init(&intr_mutex, "intr");
        acpi_init(bootinfo.bi_rsdp);
        acpi_md_init();
        lapic_init();
        ioapic_init();
        clock_calibrate();
        lapic_timer_start();
}

/*
 * The hardware half of a device interrupt. A fast source runs its
 * handlers here. Otherwise a level-triggered line is masked, so that it
 * does not fire again before the device is serviced, and the source's
 * thread is woken; it unmasks the line when the handlers are done.
 */
static void
intr_device(struct intrsource *src, struct cpu_info *ci)
{
        if (src->is_gsi == INTR_NOGSI) {
                ci->ci_nmsi++;
                if (ci->ci_cpuid != src->is_cpu)
                        atomic_inc_32_nv(&src->is_badcpu);
        }
        src->is_count++;
        rnd_intr(src->is_vector);
        if (src->is_fast) {
                for (struct intrhand *ih = src->is_handlers; ih != NULL;
                    ih = ih->ih_next) {
                        if (ih->ih_fn(ih->ih_arg))
                                ih->ih_count++;
                }
        } else {
                if (src->is_level && src->is_gsi != INTR_NOGSI)
                        ioapic_mask(src->is_gsi);
                vsema(&src->is_sema);
        }
        lapic_eoi();
}

/* The interrupt thread of one source. */
static void
intr_thread(void *arg)
{
        struct intrsource *src = arg;
        struct kthread *kt = curthread;
        int s = thread_lock(kt);

        kt->kt_flags |= KTF_ITHREAD;
        thread_unlock(kt, s);

        for (;;) {
                psema(&src->is_sema);
                if (src->is_gsi == INTR_NOGSI && cpu_number() != src->is_cpu)
                        atomic_inc_32_nv(&src->is_badcpu);
                atomic_store_32(&src->is_busy, 1);
                /* Order the store before the list read; see disestablish. */
                membar_sync();
                for (struct intrhand *ih = src->is_handlers; ih != NULL;
                    ih = ih->ih_next) {
                        if (ih->ih_fn(ih->ih_arg))
                                ih->ih_count++;
                }
                atomic_store_32(&src->is_busy, 0);
                if (src->is_level && src->is_gsi != INTR_NOGSI)
                        ioapic_unmask(src->is_gsi);
        }
}

static void
intr_nop(void *arg __unused)
{
}

/*
 * Attach handler fn(arg) to interrupt irq: an ISA IRQ with INTR_ISA,
 * otherwise a GSI whose trigger and polarity come from flags. Returns a
 * cookie for intr_disestablish(), or NULL if the interrupt cannot be
 * routed or is already attached in the other mode (fast or threaded).
 * Must be called from thread context.
 */
void *
intr_establish(int irq, int flags, int (*fn)(void *), void *arg,
    const char *name)
{
        struct intrsource *src = NULL;
        struct intrhand *ih;
        int level, active_low, fast = (flags & INTR_FAST) != 0;
        uint32_t gsi;

        if ((flags & INTR_ISA) != 0) {
                if (irq < 0 || irq >= ISA_NIRQ)
                        return NULL;
                gsi = isa_irq_to_gsi(irq, &level, &active_low);
        } else {
                gsi = (uint32_t)irq;
                level = (flags & INTR_LEVEL) != 0;
                active_low = (flags & INTR_ACTIVE_LOW) != 0;
        }

        mutex_lock(&intr_mutex);
        for (int v = DEVICE_VECTOR_FIRST; v < intr_next_vector; v++) {
                if (intr_sources[v] != NULL && intr_sources[v]->is_gsi == gsi &&
                    gsi != INTR_NOGSI) {
                        src = intr_sources[v];
                        break;
                }
        }
        if (src != NULL && src->is_fast != fast) {
                /* An idle source may change mode. */
                if (src->is_handlers != NULL) {
                        mutex_unlock(&intr_mutex);
                        return NULL;
                }
                src->is_fast = fast;
        }
        if (src == NULL) {
                if (intr_next_vector > DEVICE_VECTOR_LAST) {
                        mutex_unlock(&intr_mutex);
                        return NULL;
                }
                src = kmem_zalloc(sizeof(*src), KM_SLEEP);
                src->is_gsi = gsi;
                src->is_vector = intr_next_vector;
                src->is_level = level;
                src->is_active_low = active_low;
                src->is_fast = fast;
                sema_init(&src->is_sema, 0, "intr");
                if (ioapic_route(gsi, src->is_vector, level, active_low,
                    cpu_info_primary.ci_lapic_id) != 0) {
                        kmem_free(src, sizeof(*src));
                        mutex_unlock(&intr_mutex);
                        return NULL;
                }
                intr_next_vector++;
                membar_producer();
                intr_sources[src->is_vector] = src;
        }
        if (!src->is_fast && src->is_thread == NULL) {
                char tname[KT_NAMELEN];

                snprintf(tname, sizeof(tname), "intr%u", gsi);
                src->is_thread = kthread_create(intr_thread, src, PRI_INTR,
                    tname);
                (void)kthread_setclass(src->is_thread, SCHED_FIFO, PRI_INTR);
        }

        ih = kmem_zalloc(sizeof(*ih), KM_SLEEP);
        ih->ih_fn = fn;
        ih->ih_arg = arg;
        ih->ih_src = src;
        ih->ih_name = name;
        ih->ih_next = src->is_handlers;
        membar_producer();
        src->is_handlers = ih;
        ioapic_unmask(gsi);
        mutex_unlock(&intr_mutex);
        return ih;
}

/*
 * Detach a handler. On return it is not running and will not run again.
 * The source keeps its vector and thread for later use.
 */
void
intr_disestablish(void *cookie)
{
        struct intrhand *ih = cookie;
        struct intrsource *src = ih->ih_src;
        struct intrhand *volatile *link;

        mutex_lock(&intr_mutex);
        for (link = &src->is_handlers; *link != NULL;
            link = &(*link)->ih_next) {
                if (*link == ih) {
                        *link = ih->ih_next;
                        break;
                }
        }
        if (src->is_handlers == NULL && src->is_gsi != INTR_NOGSI)
                ioapic_mask(src->is_gsi);

        /*
         * Hardware halves run with interrupts disabled, so once every
         * processor has run a call, none is still inside one. Then wait
         * for the thread to finish any pass that may have seen ih.
         */
        smp_call_all(intr_nop, NULL);
        while (atomic_load_32(&src->is_busy) != 0)
                delay(1);
        mutex_unlock(&intr_mutex);
        kmem_free(ih, sizeof(*ih));
}

/* An online CPU whose physical APIC ID fits a unicast MSI address. */
static struct cpu_info *
intr_msi_cpu(unsigned int id)
{
        struct cpu_info *ci = cpu_lookup(id);

        if (ci == NULL || ci->ci_lapic_id >= MSI_ADDR_BROADCAST)
                return NULL;
        return ci;
}

/*
 * Each message has its own vector with fixed, edge-triggered delivery.
 * Choose a destination in round-robin order under intr_mutex.
 */
void *
intr_establish_msi(int flags, int (*fn)(void *), void *arg,
    const char *name, uint64_t *addrp, uint32_t *datap)
{
        struct intrsource *src;
        struct intrhand *ih;
        struct cpu_info *target = NULL;
        unsigned int n = ncpus;

        mutex_lock(&intr_mutex);
        if (intr_next_vector > DEVICE_VECTOR_LAST) {
                mutex_unlock(&intr_mutex);
                return NULL;
        }
        for (unsigned int i = 0; i < n; i++) {
                target = intr_msi_cpu((intr_msi_next_cpu + i) % n);
                if (target != NULL)
                        break;
        }
        if (target == NULL) {
                mutex_unlock(&intr_mutex);
                return NULL;
        }
        intr_msi_next_cpu = (target->ci_cpuid + 1) % n;
        src = kmem_zalloc(sizeof(*src), KM_SLEEP);
        src->is_gsi = INTR_NOGSI;
        src->is_cpu = target->ci_cpuid;
        src->is_vector = intr_next_vector++;
        src->is_fast = (flags & INTR_FAST) != 0;
        sema_init(&src->is_sema, 0, "intr");
        if (!src->is_fast) {
                char tname[KT_NAMELEN];

                snprintf(tname, sizeof(tname), "msi%d", src->is_vector);
                src->is_thread = kthread_create_bound(intr_thread, src,
                    PRI_INTR, tname, (int)src->is_cpu);
                (void)kthread_setclass(src->is_thread, SCHED_FIFO, PRI_INTR);
        }
        ih = kmem_zalloc(sizeof(*ih), KM_SLEEP);
        ih->ih_fn = fn;
        ih->ih_arg = arg;
        ih->ih_src = src;
        ih->ih_name = name;
        src->is_handlers = ih;
        membar_producer();
        intr_sources[src->is_vector] = src;
        mutex_unlock(&intr_mutex);

        *addrp = MSI_ADDR_BASE |
            ((uint64_t)target->ci_lapic_id << MSI_ADDR_DEST_SHIFT);
        *datap = (uint32_t)src->is_vector;
        return ih;
}

uint64_t
intr_handler_count(const void *cookie)
{
        const struct intrhand *ih = cookie;

        return ih->ih_count;
}

/* Inspect real device deliveries after the disk and network self-tests. */
int
intr_selftest_msi(void)
{
        unsigned int assigned[MAXCPUS] = { 0 };
        unsigned int online = 0, receiving = 0, active = 0;
        unsigned int most = 0, least = NIDT;
        int failures = 0;

        mutex_lock(&intr_mutex);
        for (int v = DEVICE_VECTOR_FIRST; v < intr_next_vector; v++) {
                const struct intrsource *src = intr_sources[v];

                if (src == NULL || src->is_gsi != INTR_NOGSI)
                        continue;
                assigned[src->is_cpu]++;
                if (atomic_load_64(&src->is_count) != 0)
                        active++;
                if (atomic_load_32(&src->is_badcpu) != 0) {
                        printf("selftest: %s:%d: MSI vector %d ran outside "
                            "cpu%u\n", __FILE__, __LINE__, v, src->is_cpu);
                        failures++;
                }
        }
        mutex_unlock(&intr_mutex);

        for (unsigned int i = 0; i < ncpus; i++) {
                const struct cpu_info *ci = intr_msi_cpu(i);
                uint64_t delivered;

                if (ci == NULL)
                        continue;
                online++;
                if (assigned[i] < least)
                        least = assigned[i];
                if (assigned[i] > most)
                        most = assigned[i];
                delivered = atomic_load_64(&ci->ci_nmsi);
                if (delivered != 0)
                        receiving++;
                printf("selftest: MSI cpu%u: %u vectors, %ju deliveries\n",
                    i, assigned[i], (uintmax_t)delivered);
        }
        if (online != 0 && most - least > 1) {
                printf("selftest: %s:%d: MSI vector assignments are uneven\n",
                    __FILE__, __LINE__);
                failures++;
        }
        if (online > 1 && active > 1 && receiving < 2) {
                printf("selftest: %s:%d: MSI traffic did not reach multiple "
                    "CPUs\n", __FILE__, __LINE__);
                failures++;
        }
        return failures;
}

/*
 * Set up interrupts on a secondary processor, before it is marked
 * running. Interrupts stay disabled.
 */
void
intr_cpu_init(struct cpu_info *ci)
{
        spinlock_init(&ci->ci_act_lock, "cpu_act");
        lapic_cpu_init();
        lapic_timer_start();
}

/* Called from trap() for vectors 32 to 255, with interrupts disabled. */
void
intr_dispatch(struct trapframe *tf)
{
        struct cpu_info *ci = curcpu();
        int vector = (int)tf->tf_trapno;

        ci->ci_nintr++;
        ci->ci_intr_depth++;
        switch (vector) {
        case LAPIC_TIMER_VECTOR:
                ci->ci_ticks++;
                lapic_eoi();
                if (ci == &cpu_info_primary)
                        clock();
                sched_tick((tf->tf_cs & SEL_RPL_MASK) == SEL_UPL);
                break;
        case IPI_RESCHED_VECTOR:
                ci->ci_sched.sc_want_resched = 1;
                lapic_eoi();
                break;
        case IPI_ACTION_VECTOR:
                ci->ci_nipi++;
                /* EOI first, so a request posted while draining raises a
                 * new IPI rather than being missed. */
                lapic_eoi();
                cpu_action_drain();
                break;
        case LAPIC_ERROR_VECTOR:
                printf("cpu%u: local APIC error\n", ci->ci_cpuid);
                lapic_eoi();
                break;
        case LAPIC_SPURIOUS_VECTOR:
                /* Spurious interrupts take no EOI. */
                break;
        default:
                if (intr_sources[vector] != NULL) {
                        intr_device(intr_sources[vector], ci);
                        break;
                }
                if (vector >= ICU_OFFSET && vector < ICU_OFFSET + 16)
                        break;  /* spurious 8259 interrupt; all are masked */
                if (atomic_swap_32(&stray_reported, 1) == 0)
                        printf("cpu%u: stray interrupt, vector %d\n",
                            ci->ci_cpuid, vector);
                lapic_eoi();
                break;
        }
        ci->ci_intr_depth--;
}

/*
 * Ask processor ci to run fn(arg) at splhi, after any requests already
 * queued for it. Returns once the request is queued, not when it has
 * run. A request for the calling processor runs at once.
 */
void
cpu_action(struct cpu_info *ci, void (*fn)(void *), void *arg)
{
        int s;

        if (ci == curcpu()) {
                s = splhi();
                fn(arg);
                splx(s);
                return;
        }

        for (;;) {
                s = spin_lock(&ci->ci_act_lock);
                if (ci->ci_act_count < CPU_ACTIONS) {
                        struct cpu_action *ca = &ci->ci_act[
                            (ci->ci_act_head + ci->ci_act_count) %
                            CPU_ACTIONS];

                        ca->ca_fn = fn;
                        ca->ca_arg = arg;
                        ci->ci_act_count++;
                        spin_unlock(&ci->ci_act_lock, s);
                        break;
                }
                spin_unlock(&ci->ci_act_lock, s);
                /* The target may be waiting on us; serve our own queue. */
                cpu_action_drain();
                cpu_spinwait();
        }
        lapic_send_ipi(ci->ci_lapic_id, IPI_ACTION_VECTOR);
}

/* Run every request queued for the calling processor, in order. */
void
cpu_action_drain(void)
{
        int s = splhi();
        struct cpu_info *ci = curcpu();

        for (;;) {
                struct cpu_action ca;
                int s2;

                s2 = spin_lock(&ci->ci_act_lock);
                if (ci->ci_act_count == 0) {
                        spin_unlock(&ci->ci_act_lock, s2);
                        break;
                }
                ca = ci->ci_act[ci->ci_act_head];
                ci->ci_act_head = (ci->ci_act_head + 1) % CPU_ACTIONS;
                ci->ci_act_count--;
                spin_unlock(&ci->ci_act_lock, s2);
                ca.ca_fn(ca.ca_arg);
        }
        splx(s);
}

static void
smp_call_run(void *arg)
{
        struct smp_call *sc = arg;

        sc->sc_fn(sc->sc_arg);
        membar_exit();
        atomic_dec_32_nv(&sc->sc_pending);
}

/*
 * Run fn(arg) on every running processor and return when all have
 * finished. May be called from any processor, with interrupts enabled
 * or disabled, and from inside an action, but not while holding a spin
 * lock that fn or another processor's pending action might take.
 */
void
smp_call_all(void (*fn)(void *), void *arg)
{
        int spl = splhi();      /* stay on this processor throughout */
        struct cpu_info *self = curcpu();
        struct smp_call sc;
        unsigned int n = ncpus;

        sc.sc_fn = fn;
        sc.sc_arg = arg;
        sc.sc_pending = 0;
        for (unsigned int i = 0; i < n; i++) {
                struct cpu_info *ci = &cpu_info_store[i];

                if (ci != self && (ci->ci_flags & CPUF_RUNNING) != 0)
                        sc.sc_pending++;
        }
        membar_producer();
        for (unsigned int i = 0; i < n; i++) {
                struct cpu_info *ci = &cpu_info_store[i];

                if (ci != self && (ci->ci_flags & CPUF_RUNNING) != 0)
                        cpu_action(ci, smp_call_run, &sc);
        }

        fn(arg);

        while (atomic_load_32(&sc.sc_pending) != 0) {
                if (cpu_halt_requested())
                        cpu_halt_self();
                cpu_action_drain();
                cpu_spinwait();
        }
        membar_enter();
        splx(spl);
}

/* Look up a running processor by number. */
struct cpu_info *
cpu_lookup(unsigned int id)
{
        struct cpu_info *ci;

        if (id >= ncpus)
                return NULL;
        ci = &cpu_info_store[id];
        return (ci->ci_flags & CPUF_RUNNING) != 0 ? ci : NULL;
}

/*
 * Make processor ci reschedule soon. For the calling processor a self
 * IPI is used, so that the switch happens as soon as interrupts are
 * enabled again rather than at the next tick.
 */
void
cpu_need_resched(struct cpu_info *ci)
{
        if (!lapic_ready())
                return;
        if (ci == curcpu())
                lapic_send_self_ipi(IPI_RESCHED_VECTOR);
        else
                lapic_send_ipi(ci->ci_lapic_id, IPI_RESCHED_VECTOR);
}

/*
 * Wait for an interrupt from the idle thread, which calls this at
 * splhi. Returns at splhi after one interrupt has been taken, or at once
 * if a reschedule is already pending.
 */
void
cpu_idle_wait(void)
{
        if (cpu_halt_requested())
                cpu_halt_self();
        cpu_action_drain();
        if (curcpu()->ci_sched.sc_want_resched)
                return;
        cpu_wait_for_interrupt();
        disable_intr();
}

/* Stop every other processor, whatever its interrupt state. */
void
cpu_halt_others(void)
{
        atomic_store_32(&cpus_halting, 1);
        membar_sync();
        lapic_send_nmi_others();
}

int
cpu_halt_requested(void)
{
        return atomic_load_32(&cpus_halting) != 0;
}

int
smp_halting(void)
{
        return cpu_halt_requested();
}

void
cpu_halt_self(void)
{
        for (;;) {
                disable_intr();
                cpu_halt();
        }
}
