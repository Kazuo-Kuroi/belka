/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Multiprocessor control, the machine-independent view. Requests travel
 * between processors as inter-processor interrupts, through per-
 * processor action queues in the manner of IRIX.
 */

#ifndef _SYS_SMP_H_
#define _SYS_SMP_H_

/* Number of processors running in the kernel. */
extern volatile unsigned int ncpus;

/* Start every secondary processor. Needs the page allocator. */
void cpu_boot_secondary_processors(void);

/*
 * Run fn(arg) at splhi on every running processor, this one included,
 * and return when all have finished. Calls from several processors may
 * overlap, and a call may be made from inside fn.
 */
void smp_call_all(void (*)(void *), void *);

/* Stop every other processor, even with interrupts disabled. */
void cpu_halt_others(void);

/* Nonzero once the system has begun stopping its processors. */
int smp_halting(void);

#endif /* _SYS_SMP_H_ */
