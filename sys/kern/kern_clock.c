/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * The clock tick.
 *
 * clock() runs HZ times a second on the bootstrap processor, from the
 * timer interrupt. lbolt counts ticks since the clock started, as in
 * System V. Callouts, time of day, and scheduler accounting are added
 * here as they arrive; per-processor work happens in the machine-
 * dependent tick handler on every processor.
 */

#include <stdint.h>

#include <sys/kthread.h>
#include <sys/systm.h>

volatile uint64_t lbolt;

void
clock(void)
{
        lbolt++;
        callout_tick();
}
