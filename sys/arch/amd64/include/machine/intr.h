/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Interrupt priority.
 *
 * Device interrupts will run as threads, so the kernel needs only two
 * levels: interrupts enabled, and splhi, where the processor takes no
 * maskable interrupts. All device priorities collapse into splhi.
 *
 * splhi() returns a cookie recording the previous state, and splx()
 * restores it. Calls nest in the usual way.
 */

#ifndef _MACHINE_INTR_H_
#define _MACHINE_INTR_H_

#include <machine/cpufunc.h>

static inline int
splhi(void)
{
        int s = (read_rflags() & PSL_I) != 0;

        disable_intr();
        return s;
}

static inline void
splx(int s)
{
        if (s != 0)
                enable_intr();
}

/* Nonzero if maskable interrupts are blocked on this processor. */
static inline int
intr_blocked(void)
{
        return (read_rflags() & PSL_I) == 0;
}

#endif /* _MACHINE_INTR_H_ */
