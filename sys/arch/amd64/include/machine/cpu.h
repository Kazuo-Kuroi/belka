/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Per-processor data.
 *
 * Each processor owns a struct cpu_info, found through the GS segment
 * base while in the kernel: %gs:0 holds the structure's own address, so
 * curcpu() is one load. This plays the part of the IRIX PDA, without
 * the fixed virtual address that the MIPS design relied on.
 *
 * Processor 0 is the bootstrap processor. The others are numbered in
 * the order the boot loader lists them.
 */

#ifndef _MACHINE_CPU_H_
#define _MACHINE_CPU_H_

#include <stdint.h>

#include <sys/compiler.h>
#include <sys/sched.h>
#include <sys/spinlock.h>

#include <machine/param.h>
#include <machine/segments.h>
#include <machine/tss.h>

#define KSTACK_ORDER    2               /* kernel stacks: 16 KB */
#define IST_STACK_SIZE  16384
#define CPU_ACTIONS     64              /* action queue entries */

/* A request queued for a processor by cpu_action(). */
struct cpu_action {
        void            (*ca_fn)(void *);
        void            *ca_arg;
};

struct hat;
struct kthread;

/*
 * ci_self, ci_curthread, ci_kstack, and ci_uscratch are reached with
 * single %gs-relative accesses, the last two from the system call entry
 * before any register is free, so their offsets are fixed; cpu_tables.c
 * checks them.
 */
#define CI_SELF_OFFSET          0
#define CI_CURTHREAD_OFFSET     8
#define CI_KSTACK_OFFSET        16
#define CI_USCRATCH_OFFSET      24

struct cpu_info {
        struct cpu_info *ci_self;       /* must be first: read via %gs:0 */
        struct kthread  *ci_curthread;  /* read via %gs:8 */
        uint64_t        ci_kstack;      /* curthread's stack top, %gs:16 */
        uint64_t        ci_uscratch;    /* user %rsp at syscall, %gs:24 */
        struct hat      *ci_hat;        /* user address space loaded */
        uint32_t        ci_cpuid;       /* kernel processor number */
        uint32_t        ci_lapic_id;
        volatile uint32_t ci_flags;
        uint32_t        ci_pad;
        uint64_t        ci_stack_top;   /* idle/boot kernel stack */
        uint64_t        ci_hatch_top;   /* start-up stack, until running */
        uint64_t        ci_ist_top[NIST];
        uint64_t        ci_gdt[NGDT];
        uint32_t        ci_tss[TSS_WORDS];

        /* Scheduler state; see <sys/sched.h>. */
        struct schedcpu ci_sched;

        /* Spin locks held, and interrupt nesting, for assertions. */
        volatile int    ci_spinlocks;
        volatile int    ci_intr_depth;

        /* Clock and interrupt counts. */
        volatile uint64_t ci_ticks;
        volatile uint64_t ci_nintr;
        volatile uint64_t ci_nipi;

        /*
         * Action queue: functions other processors ask this one to run,
         * in order, at splhi. Protected by ci_act_lock.
         */
        struct spinlock ci_act_lock;
        unsigned int    ci_act_head;    /* next entry to run */
        unsigned int    ci_act_count;
        struct cpu_action ci_act[CPU_ACTIONS];

        /* Expected-trap state for self-tests; see trap_expect(). */
        volatile int    ci_texp_vec;    /* vector + 1, or 0 for none */
        volatile int    ci_texp_skip;
        volatile int    ci_texp_hit;
        volatile uint64_t ci_texp_err;
};

/* ci_flags */
#define CPUF_PRESENT    0x0001  /* listed by the boot loader */
#define CPUF_BSP        0x0002  /* the bootstrap processor */
#define CPUF_RUNNING    0x0004  /* started and in the kernel */

extern struct cpu_info cpu_info_store[MAXCPUS];
#define cpu_info_primary (cpu_info_store[0])

#ifdef HAVE_INLINE_ASM
static inline struct cpu_info *
curcpu(void)
{
        struct cpu_info *ci;

        __asm__ __volatile__("movq %%gs:0, %0" : "=r"(ci));
        return ci;
}
#else
struct cpu_info *curcpu(void);
#endif

/*
 * The current thread, read in one instruction so that preemption cannot
 * split the read of the processor from the read of its thread.
 */
#ifdef HAVE_INLINE_ASM
static inline struct kthread *
cpu_curthread(void)
{
        struct kthread *kt;

        __asm__ __volatile__("movq %%gs:8, %0" : "=r"(kt));
        return kt;
}
#else
struct kthread *cpu_curthread(void);
#endif

/*
 * The processor number is stable only while the caller cannot migrate:
 * at splhi, or in a thread bound to one processor.
 */
#define cpu_number()    (curcpu()->ci_cpuid)

/* Look up a processor by number; NULL if it is not running. */
struct cpu_info *cpu_lookup(unsigned int);

/* Ask processor ci to reschedule, by IPI if it is another processor. */
void cpu_need_resched(struct cpu_info *);

/* Idle: halt until an interrupt, serving halt requests and actions. */
void cpu_idle_wait(void);

/* cpu_tables.c */
void idt_init(void);
void cpu_tables_init(struct cpu_info *);
void cpu_set_ist(struct cpu_info *, int, uint64_t);

/* intr.c */
struct trapframe;
void intr_init(void);
void intr_cpu_init(struct cpu_info *);
void intr_dispatch(struct trapframe *);
void cpu_action(struct cpu_info *, void (*)(void *), void *);
void cpu_action_drain(void);
void cpu_halt_self(void) __dead;
int cpu_halt_requested(void);

/* locore.S: run fn(arg) on the stack whose top is given; never returns. */
void stack_switch_call(uint64_t, void (*)(void *), void *) __dead;

/* locore.S: save callee-saved state to *old, resume the thread at new. */
void cpu_switch(uint64_t *, uint64_t);

/* Prepare a new thread's stack so that its first switch enters it. */
void cpu_thread_setup(struct kthread *);

#endif /* _MACHINE_CPU_H_ */
