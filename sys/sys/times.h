/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-10
 * SPDX-License-Identifier: MIT
 */

/*
 * times() (43): a process's processor time and its reaped children's,
 * in clock ticks (HZ a second); the call returns the ticks since boot.
 * All time is counted as user time for now.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_TIMES_H_
#define _SYS_TIMES_H_

#include <stdint.h>

struct tms {
        int64_t         tms_utime;      /* the process's user time */
        int64_t         tms_stime;      /* its system time */
        int64_t         tms_cutime;     /* its reaped children's user time */
        int64_t         tms_cstime;     /* their system time */
};

#endif /* _SYS_TIMES_H_ */
