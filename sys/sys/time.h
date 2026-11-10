/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Time: the timespec, the clocks clock_gettime() reads (with the POSIX
 * values the C library uses), and, in the kernel, the high-resolution
 * clock and the time of day.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

#include <stdint.h>

struct timespec {
        int64_t         tv_sec;
        long            tv_nsec;
};

#define NANOSEC         ((uint64_t)1000000000)

/* Clocks for clock_gettime() and clock_getres(). */
#define CLOCK_REALTIME          0       /* the time of day */
#define CLOCK_MONOTONIC         1       /* time since boot */
#define CLOCK_PROCESS_CPUTIME_ID 2      /* the process's processor time */
#define CLOCK_THREAD_CPUTIME_ID 3       /* the calling LWP's */
#define CLOCK_MONOTONIC_RAW     4       /* as CLOCK_MONOTONIC */
#define CLOCK_REALTIME_COARSE   5       /* as CLOCK_REALTIME */
#define CLOCK_MONOTONIC_COARSE  6       /* as CLOCK_MONOTONIC */
#define CLOCK_BOOTTIME          7       /* as CLOCK_MONOTONIC */

#ifdef _KERNEL
/*
 * Nanoseconds since the clock started, never decreasing, on any
 * processor: from the invariant TSC where there is one, else from the
 * clock tick.
 */
uint64_t gethrtime(void);

/*
 * The current time: the time of day read from the hardware clock at
 * boot, advanced by gethrtime() since. It is not yet adjusted, so it
 * drifts with the clock it is advanced by.
 */
void gethrestime(struct timespec *);

/* Seconds since 1970 at boot; set by machine-dependent start-up. */
extern int64_t boottime_sec;
#endif /* _KERNEL */

#endif /* _SYS_TIME_H_ */
