/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Multiprocessor control, the machine-independent view.
 *
 * Until the interrupt controller is driven, secondary processors wait
 * in a polling idle loop with interrupts disabled. smp_call_all() and
 * cpu_halt_others() work by posting requests that loop picks up. When
 * inter-processor interrupts exist they will be delivered as IPIs, in
 * the manner of IRIX's per-processor action queues, and the interfaces
 * below stay the same.
 */

#ifndef _SYS_SMP_H_
#define _SYS_SMP_H_

/* Number of processors running in the kernel. */
extern volatile unsigned int ncpus;

/* Start every secondary processor. Needs the page allocator. */
void cpu_boot_secondary_processors(void);

/*
 * Run fn(arg) on every running processor, this one included, and
 * return when all have finished. Only one call may be in progress.
 */
void smp_call_all(void (*)(void *), void *);

/* Stop every other processor; used on panic and shutdown. */
void cpu_halt_others(void);

#endif /* _SYS_SMP_H_ */
