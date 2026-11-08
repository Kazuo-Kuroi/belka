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

#include <machine/param.h>
#include <machine/segments.h>
#include <machine/tss.h>

#define KSTACK_ORDER    2               /* kernel stacks: 16 KB */
#define IST_STACK_SIZE  16384

struct cpu_info {
        struct cpu_info *ci_self;       /* must be first: read via %gs:0 */
        uint32_t        ci_cpuid;       /* kernel processor number */
        uint32_t        ci_lapic_id;
        volatile uint32_t ci_flags;
        uint32_t        ci_pad;
        uint64_t        ci_stack_top;   /* idle/boot kernel stack */
        uint64_t        ci_ist_top[NIST];
        uint64_t        ci_gdt[NGDT];
        uint32_t        ci_tss[TSS_WORDS];
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

#define cpu_number()    (curcpu()->ci_cpuid)

/* cpu_tables.c */
void idt_init(void);
void cpu_tables_init(struct cpu_info *);

/* locore.S: run fn(arg) on the stack whose top is given; never returns. */
void stack_switch_call(uint64_t, void (*)(void *), void *) __dead;

#endif /* _MACHINE_CPU_H_ */
