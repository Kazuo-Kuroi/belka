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
 *
 * gethrtime() is the high-resolution clock: nanoseconds since the clock
 * started, from the machine's nanosecond clock where it has one (the
 * invariant TSC on amd64), else from lbolt. Processors' clocks can
 * differ a little, so the latest value read on any processor is kept,
 * and no reader gets an earlier one.
 */

#include <stdint.h>

#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <machine/atomic.h>
#include <machine/md_var.h>

volatile uint64_t lbolt;
int64_t boottime_sec;
static volatile uint64_t hrtime_last;

uint64_t
gethrtime(void)
{
        uint64_t t, last, prev;

        t = md_hrtime_ok ? md_hrtime() : lbolt * (NANOSEC / HZ);
        last = hrtime_last;
        while (t > last) {
                if ((prev = atomic_cas_64(&hrtime_last, last, t)) == last)
                        return t;
                last = prev;
        }
        return last;
}

void
gethrestime(struct timespec *ts)
{
        uint64_t t = gethrtime();

        ts->tv_sec = boottime_sec + (int64_t)(t / NANOSEC);
        ts->tv_nsec = (long)(t % NANOSEC);
}

void
clock(void)
{
        lbolt++;
        callout_tick();
}
