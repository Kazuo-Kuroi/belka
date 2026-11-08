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

#include <sys/kmem.h>
#include <sys/kthread.h>

#include <vm/hat.h>
#include <vm/kva.h>
#include <vm/page.h>

/* How long to wait for a processor to report in. */
#define CPU_START_USECS 1000000
#define CPU_START_POLL  10

struct cpu_info cpu_info_store[MAXCPUS];
volatile unsigned int ncpus = 1;

static void cpu_hatch_entry(void *);
static void cpu_hatch_tables(void *);
static void cpu_hatch(void *);

/* thread_start in locore.S. */
extern char thread_start[];

/*
 * A processor starts on the boot loader's stack, under the boot loader's
 * page tables, which do not map KVA. It moves first to a one-page hatch
 * stack in the direct map, which both sets of tables map, switches to
 * the kernel's tables there, and only then moves to its guarded KVA
 * stack.
 */
static void
cpu_hatch_entry(void *arg)
{
        struct cpu_info *ci = arg;

        stack_switch_call(ci->ci_hatch_top, cpu_hatch_tables, ci);
}

static void
cpu_hatch_tables(void *arg)
{
        struct cpu_info *ci = arg;

        hat_cpu_init();
        stack_switch_call(ci->ci_stack_top, cpu_hatch, ci);
}

static void
cpu_hatch(void *arg)
{
        struct cpu_info *ci = arg;

        cpu_tables_init(ci);
        intr_cpu_init(ci);
        /* This context, on its guarded stack, becomes the idle thread. */
        ci->ci_curthread = kthread_alloc_idle(ci, ci->ci_stack_top);
        atomic_or_32(&ci->ci_flags, CPUF_RUNNING);
        sched_idle();
}

/*
 * Lay out a new thread's stack as cpu_switch() expects: six zeroed
 * callee-saved registers, with the kthread in %r12, then thread_start as
 * the return address. The frame starts 8 bytes off a 16-byte boundary
 * so that thread_start runs with an aligned stack.
 */
void
cpu_thread_setup(struct kthread *kt)
{
        uint64_t *sp = (uint64_t *)(uintptr_t)(kt->kt_stack - 72);

        for (int i = 0; i < 9; i++)
                sp[i] = 0;
        sp[3] = (uint64_t)(uintptr_t)kt;                /* %r12 */
        sp[6] = (uint64_t)(uintptr_t)thread_start;      /* return */
        kt->kt_rsp = (uint64_t)(uintptr_t)sp;
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
                struct page *hatch;
                unsigned int waited;

                if (bi->bi_cpu_lapic[i] == bi->bi_bsp_lapic_id)
                        continue;

                ci = &cpu_info_store[next];
                ci->ci_self = ci;
                ci->ci_cpuid = next;
                ci->ci_lapic_id = bi->bi_cpu_lapic[i];
                ci->ci_flags = CPUF_PRESENT;
                if ((hatch = page_alloc(0, 0)) == NULL)
                        panic("cpu_boot: out of memory");
                ci->ci_hatch_top = (uint64_t)(uintptr_t)page_to_kva(hatch) +
                    PAGESIZE;
                ci->ci_stack_top = kstack_alloc(KM_SLEEP);
                for (int j = 0; j < NIST; j++)
                        ci->ci_ist_top[j] = kstack_alloc(KM_SLEEP);

                limine_cpu_start(i, cpu_hatch_entry, ci);
                for (waited = 0; waited < CPU_START_USECS;
                    waited += CPU_START_POLL) {
                        if ((atomic_load_32(&ci->ci_flags) &
                            CPUF_RUNNING) != 0)
                                break;
                        drv_usecwait(CPU_START_POLL);
                }
                if (waited >= CPU_START_USECS) {
                        /* It may yet start, so its memory is kept. */
                        printf("cpu%u: LAPIC id %u did not start\n",
                            next, bi->bi_cpu_lapic[i]);
                        continue;
                }
                page_free(hatch, 0);
                ci->ci_hatch_top = 0;
                if (ci->ci_lapic_id != bi->bi_cpu_lapic[i])
                        printf("cpu%u: LAPIC id %u, boot loader said %u\n",
                            next, ci->ci_lapic_id, bi->bi_cpu_lapic[i]);
                next++;
                atomic_inc_32_nv((volatile uint32_t *)&ncpus);
        }

        printf("cpu: %u of %ju processors running\n", ncpus,
            (uintmax_t)bi->bi_ncpu);
}
