/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Secondary processor start-up and the polling idle loop.
 *
 * Limine has already brought every processor into 64-bit mode and
 * parked it. Starting one gives it its own kernel stack, interrupt
 * stacks, GDT, and TSS, then sends it to cpu_idle_loop().
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/smp.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/bootinfo.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/param.h>

#include <vm/page.h>

/* Polls to wait for a processor to report in; KVM needs far fewer. */
#define CPU_START_SPINS UINT64_C(2000000000)

struct cpu_info cpu_info_store[MAXCPUS];
volatile unsigned int ncpus = 1;

/* The request posted by smp_call_all(). */
static struct {
        void            (*sc_fn)(void *);
        void            *sc_arg;
        volatile uint32_t sc_gen;       /* bumped to post a request */
        volatile uint32_t sc_done;      /* processors that finished */
} smp_call;

static volatile uint32_t cpus_halting;

static uint64_t cpu_alloc_stack(void);
static void cpu_hatch_entry(void *);
static void cpu_hatch(void *);
static void cpu_idle_loop(struct cpu_info *) __dead;
static void cpu_halt_self(void) __dead;

/* Allocate a kernel stack and return its top. */
static uint64_t
cpu_alloc_stack(void)
{
        struct page *pp;

        if ((pp = page_alloc(KSTACK_ORDER, 0)) == NULL)
                panic("cpu_alloc_stack: out of memory");
        return (uint64_t)(uintptr_t)page_to_kva(pp) +
            ((uint64_t)PAGESIZE << KSTACK_ORDER);
}

/* Entered on the boot loader's stack; move to our own. */
static void
cpu_hatch_entry(void *arg)
{
        struct cpu_info *ci = arg;

        stack_switch_call(ci->ci_stack_top, cpu_hatch, ci);
}

static void
cpu_hatch(void *arg)
{
        struct cpu_info *ci = arg;

        cpu_tables_init(ci);
        atomic_or_32(&ci->ci_flags, CPUF_RUNNING);
        cpu_idle_loop(ci);
}

/*
 * Wait for work with interrupts disabled. This is a stand-in for the
 * scheduler's idle thread, which arrives with interrupt handling.
 */
static void
cpu_idle_loop(struct cpu_info *ci __unused)
{
        uint32_t seen = atomic_load_32(&smp_call.sc_gen);

        for (;;) {
                uint32_t gen;

                if (atomic_load_32(&cpus_halting) != 0)
                        cpu_halt_self();
                gen = atomic_load_32(&smp_call.sc_gen);
                if (gen != seen) {
                        seen = gen;
                        membar_consumer();
                        smp_call.sc_fn(smp_call.sc_arg);
                        atomic_inc_32_nv(&smp_call.sc_done);
                }
                cpu_spinwait();
        }
}

static void
cpu_halt_self(void)
{
        for (;;) {
                disable_intr();
                cpu_halt();
        }
}

void
cpu_boot_secondary_processors(void)
{
        const struct bootinfo *bi = &bootinfo;
        unsigned int next = 1;

        if (bi->bi_ncpu_dropped != 0)
                printf("cpu: %ju processors beyond MAXCPUS (%d) ignored\n",
                    (uintmax_t)bi->bi_ncpu_dropped, MAXCPUS);

        for (unsigned int i = 0; i < bi->bi_ncpu; i++) {
                struct cpu_info *ci;
                uint64_t spins;

                if (bi->bi_cpu_lapic[i] == bi->bi_bsp_lapic_id)
                        continue;

                ci = &cpu_info_store[next];
                ci->ci_self = ci;
                ci->ci_cpuid = next;
                ci->ci_lapic_id = bi->bi_cpu_lapic[i];
                ci->ci_flags = CPUF_PRESENT;
                ci->ci_stack_top = cpu_alloc_stack();
                for (int j = 0; j < NIST; j++)
                        ci->ci_ist_top[j] = cpu_alloc_stack();

                limine_cpu_start(i, cpu_hatch_entry, ci);
                for (spins = 0; spins < CPU_START_SPINS; spins++) {
                        if ((atomic_load_32(&ci->ci_flags) &
                            CPUF_RUNNING) != 0)
                                break;
                        cpu_spinwait();
                }
                if (spins == CPU_START_SPINS) {
                        printf("cpu%u: LAPIC id %u did not start\n",
                            next, ci->ci_lapic_id);
                        continue;
                }
                next++;
                atomic_inc_32_nv((volatile uint32_t *)&ncpus);
        }

        printf("cpu: %u of %ju processors running\n", ncpus,
            (uintmax_t)bi->bi_ncpu);
}

void
smp_call_all(void (*fn)(void *), void *arg)
{
        unsigned int others = ncpus - 1;

        smp_call.sc_fn = fn;
        smp_call.sc_arg = arg;
        atomic_store_32(&smp_call.sc_done, 0);
        membar_producer();
        atomic_inc_32_nv(&smp_call.sc_gen);

        fn(arg);

        while (atomic_load_32(&smp_call.sc_done) != others) {
                if (panicstr != NULL)
                        cpu_halt_self();
                cpu_spinwait();
        }
}

void
cpu_halt_others(void)
{
        atomic_store_32(&cpus_halting, 1);
        membar_sync();
}
