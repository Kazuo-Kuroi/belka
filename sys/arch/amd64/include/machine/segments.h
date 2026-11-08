/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * amd64 segmentation: the GDT layout, selectors, and gate descriptors.
 *
 * Each processor has its own GDT, because each has its own TSS. The
 * order of the user segments is fixed by SYSRET, which loads the user
 * 32-bit code, data, and 64-bit code selectors from consecutive slots.
 * The 32-bit user code segment and the LDT slot are kept for a later
 * UnixWare i386 personality.
 *
 * Descriptors are built with shifts and masks rather than bitfields or
 * packed structures, so that their layout does not depend on the
 * compiler.
 */

#ifndef _MACHINE_SEGMENTS_H_
#define _MACHINE_SEGMENTS_H_

#include <stdint.h>

#define GDT_NULL        0
#define GDT_KCODE       1       /* kernel 64-bit code */
#define GDT_KDATA       2       /* kernel data */
#define GDT_UCODE32     3       /* user 32-bit code */
#define GDT_UDATA       4       /* user data */
#define GDT_UCODE       5       /* user 64-bit code */
#define GDT_TSS         6       /* TSS; a system descriptor takes 2 slots */
#define GDT_LDT         8       /* LDT, reserved; 2 slots */
#define NGDT            10

#define SEL_KPL         0
#define SEL_UPL         3
#define SEL_RPL_MASK    3

#define GSEL(slot, rpl) ((uint16_t)(((slot) << 3) | (rpl)))

#define KCSEL           GSEL(GDT_KCODE, SEL_KPL)
#define KDSEL           GSEL(GDT_KDATA, SEL_KPL)
#define UCSEL32         GSEL(GDT_UCODE32, SEL_UPL)
#define UDSEL           GSEL(GDT_UDATA, SEL_UPL)
#define UCSEL           GSEL(GDT_UCODE, SEL_UPL)
#define TSSSEL          GSEL(GDT_TSS, SEL_KPL)

/*
 * Code and data descriptors. In 64-bit mode base and limit are ignored
 * for code, data, and stack segments, but the 32-bit user segments need
 * a flat 4 GB limit.
 */
#define SD_KCODE64      UINT64_C(0x00af9a000000ffff)
#define SD_KDATA        UINT64_C(0x00cf92000000ffff)
#define SD_UCODE32      UINT64_C(0x00cffa000000ffff)
#define SD_UDATA        UINT64_C(0x00cff2000000ffff)
#define SD_UCODE64      UINT64_C(0x00affa000000ffff)

#define SDT_SYS386TSS   0x9     /* available 64-bit TSS */
#define SDT_SYS386IGT   0xe     /* 64-bit interrupt gate */

/* Number of IDT entries. */
#define NIDT            256

/* Interrupt stack table slots used by the kernel. */
#define IST_DBLFLT      1
#define IST_NMI         2
#define IST_MCE         3
#define NIST            3

/* Fill in a 16-byte system segment descriptor (TSS or LDT). */
static inline void
set_sys_desc(uint64_t *d, uint64_t base, uint32_t limit, int type)
{
        d[0] = (uint64_t)(limit & 0xffff) |
            ((base & 0xffffff) << 16) |
            ((uint64_t)(type & 0xf) << 40) |
            (UINT64_C(1) << 47) |               /* present */
            ((uint64_t)((limit >> 16) & 0xf) << 48) |
            (((base >> 24) & 0xff) << 56);
        d[1] = base >> 32;
}

/* Fill in a 16-byte interrupt gate descriptor. */
static inline void
set_gate_desc(uint64_t *d, uint64_t offset, uint16_t sel, int ist,
    int type, int dpl)
{
        d[0] = (offset & 0xffff) |
            ((uint64_t)sel << 16) |
            ((uint64_t)(ist & 0x7) << 32) |
            ((uint64_t)(type & 0xf) << 40) |
            ((uint64_t)(dpl & 0x3) << 45) |
            (UINT64_C(1) << 47) |               /* present */
            (((offset >> 16) & 0xffff) << 48);
        d[1] = offset >> 32;
}

#endif /* _MACHINE_SEGMENTS_H_ */
