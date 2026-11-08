/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Processor exception vectors. The names follow the System V i386
 * <sys/trap.h>; the numbers are the hardware vectors.
 */

#ifndef _MACHINE_TRAP_H_
#define _MACHINE_TRAP_H_

#define DIVERR          0       /* divide error */
#define SGLSTP          1       /* debug exception */
#define NMIFLT          2       /* non-maskable interrupt */
#define BPTFLT          3       /* breakpoint (int3) */
#define INTOFLT         4       /* overflow */
#define BOUNDFLT        5       /* bound range exceeded */
#define INVOPFLT        6       /* invalid opcode */
#define NOEXTFLT        7       /* FPU not available */
#define DBLFLT          8       /* double fault */
#define EXTOVRFLT       9       /* coprocessor segment overrun */
#define INVTSSFLT       10      /* invalid TSS */
#define SEGNPFLT        11      /* segment not present */
#define STKFLT          12      /* stack fault */
#define GPFLT           13      /* general protection */
#define PGFLT           14      /* page fault */
#define EXTERRFLT       16      /* x87 floating-point error */
#define ALIGNFLT        17      /* alignment check */
#define MCEFLT          18      /* machine check */
#define SIMDFLT         19      /* SIMD floating-point */
#define VIRTFLT         20      /* virtualization exception */
#define CPFLT           21      /* control protection */

#define NEXCEPTIONS     32      /* vectors reserved for exceptions */

/* Page fault error code bits. */
#define PGEX_P          0x01    /* the page was present */
#define PGEX_W          0x02    /* write access */
#define PGEX_U          0x04    /* access from user mode */
#define PGEX_RSV        0x08    /* reserved bit set in a page table */
#define PGEX_I          0x10    /* instruction fetch */

#endif /* _MACHINE_TRAP_H_ */
