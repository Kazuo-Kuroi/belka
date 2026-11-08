/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Device interrupt attachment, with the NetBSD names that the planned
 * driver model will use.
 *
 * By default a handler runs in an interrupt thread, after IRIX: the
 * hardware interrupt only masks a level-triggered line, wakes the
 * source's thread, and returns. The thread, at PRI_INTR, runs every
 * handler on the line, then unmasks it. Handlers may therefore take
 * mutexes and briefly block. INTR_FAST runs the handler in the hardware
 * interrupt itself, with interrupts disabled, for the few handlers that
 * cannot wait for a thread; such a handler must not sleep.
 *
 * A handler returns nonzero if its device raised the interrupt, so that
 * lines may be shared.
 */

#ifndef _SYS_INTR_H_
#define _SYS_INTR_H_

#include <stdint.h>

#define INTR_ISA        0x0001  /* irq is an ISA IRQ, not a GSI */
#define INTR_LEVEL      0x0002  /* level-triggered (GSIs only) */
#define INTR_ACTIVE_LOW 0x0004  /* active low (GSIs only) */
#define INTR_FAST       0x0008  /* run in the hardware interrupt */

void *intr_establish(int, int, int (*)(void *), void *, const char *);
void intr_disestablish(void *);

/* Times the handler claimed an interrupt. */
uint64_t intr_handler_count(const void *);

#endif /* _SYS_INTR_H_ */
