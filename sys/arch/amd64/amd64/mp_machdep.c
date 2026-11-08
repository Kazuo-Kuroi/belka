/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Secondary processor start-up and the idle loop.
 *
 * Limine has already brought every processor into 64-bit mode and
 * parked it. Starting one gives it its own kernel stack, interrupt
 * stacks, GDT, and TSS, switches it to the kernel's page tables, starts
 * its local APIC and tick, and sends it to cpu_idle_loop().
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

#include <vm/hat.h>
#include <vm/page.h>

/* How long to wait for a processor to report in. */
#define CPU_START_USECS 1000000
#define CPU_START_POLL  10

struct cpu_info cpu_info_store[MAXCPUS];
volatile unsigned int ncpus = 1;

static uint64_t cpu_alloc_stack(void);
static void cpu_hatch_entry(void *);
static void cpu_hatch(void *);
static void cpu_idle_loop(void) __dead;

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

        /*
         * The new stack is in the direct map, which the boot loader's
         * tables and ours both contain, so switching tables here is safe.
         */
        hat_cpu_init();
        cpu_tables_init(ci);
        intr_cpu_init(ci);
        atomic_or_32(&ci->ci_flags, CPUF_RUNNING);
        cpu_idle_loop();
}

/*
 * Sleep until an interrupt arrives, then serve it. This stands in for
 * the scheduler's idle thread. Requests from other processors arrive as
 * action IPIs and are run by the interrupt handler.
 */
static void
cpu_idle_loop(void)
{
        for (;;) {
                disable_intr();
                if (cpu_halt_requested())
                        cpu_halt_self();
                cpu_action_drain();
                cpu_wait_for_interrupt();
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
                unsigned int waited;

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
                for (waited = 0; waited < CPU_START_USECS;
                    waited += CPU_START_POLL) {
                        if ((atomic_load_32(&ci->ci_flags) &
                            CPUF_RUNNING) != 0)
                                break;
                        drv_usecwait(CPU_START_POLL);
                }
                if (waited >= CPU_START_USECS) {
                        printf("cpu%u: LAPIC id %u did not start\n",
                            next, bi->bi_cpu_lapic[i]);
                        continue;
                }
                if (ci->ci_lapic_id != bi->bi_cpu_lapic[i])
                        printf("cpu%u: LAPIC id %u, boot loader said %u\n",
                            next, ci->ci_lapic_id, bi->bi_cpu_lapic[i]);
                next++;
                atomic_inc_32_nv((volatile uint32_t *)&ncpus);
        }

        printf("cpu: %u of %ju processors running\n", ncpus,
            (uintmax_t)bi->bi_ncpu);
}
