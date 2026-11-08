/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel time.
 */

#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

#include <stdint.h>

struct timespec {
        int64_t         tv_sec;
        long            tv_nsec;
};

/*
 * The current time: the time of day read from the hardware clock at
 * boot, advanced by the clock tick since. It is not yet adjusted, so it
 * drifts with the tick.
 */
void gethrestime(struct timespec *);

/* Seconds since 1970 at boot; set by machine-dependent start-up. */
extern int64_t boottime_sec;

#endif /* _SYS_TIME_H_ */
