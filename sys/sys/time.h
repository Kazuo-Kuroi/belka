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
 * The current time. There is no time-of-day clock yet, so this counts
 * from boot; file times are therefore boot-relative for now.
 */
void gethrestime(struct timespec *);

#endif /* _SYS_TIME_H_ */
