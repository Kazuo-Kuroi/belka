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
 */

#ifndef _MACHINE_CPU_H_
#define _MACHINE_CPU_H_

#include <stdint.h>

#include <sys/compiler.h>

#include <machine/segments.h>
#include <machine/tss.h>

#define IST_STACK_SIZE  16384

struct cpu_info {
        struct cpu_info *ci_self;       /* must be first: read via %gs:0 */
        uint32_t        ci_cpuid;       /* kernel processor number */
        uint32_t        ci_lapic_id;
        uint64_t        ci_gdt[NGDT];
        uint32_t        ci_tss[TSS_WORDS];
};

extern struct cpu_info cpu_info_primary;

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

/* cpu_tables.c */
void idt_init(void);
void cpu_tables_init(struct cpu_info *, uint64_t, const uint64_t *);

#endif /* _MACHINE_CPU_H_ */
