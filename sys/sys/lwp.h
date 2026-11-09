/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * LWPs from user mode: the _lwp_create flags, LWP semaphores, and
 * _lwp_info's result. The calls are UnixWare's (173 to 178, 193, 203,
 * 204, 207 to 209); the flag values and structure layouts are the
 * native ones, since UnixWare's header is not among our references.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_LWP_H_
#define _SYS_LWP_H_

#include <stdint.h>

typedef uint32_t lwpid_t;

/* _lwp_create flags. */
#define LWP_DETACHED    0x00000040      /* not to be waited for */
#define LWP_SUSPENDED   0x00000080      /* created suspended */

/*
 * An LWP semaphore: a count in user memory that the kernel decrements
 * for _lwp_sema_wait (blocking at zero) and increments for
 * _lwp_sema_post. Only those calls may change it.
 */
typedef struct {
        volatile int32_t sema_count;
        int32_t         sema_pad[3];
} _lwp_sema_t;

struct lwpinfo {
        int64_t         lwp_utime_sec;
        int64_t         lwp_utime_nsec;
        int64_t         lwp_stime_sec;
        int64_t         lwp_stime_nsec;
};

#endif /* _SYS_LWP_H_ */
