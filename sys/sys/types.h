/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel types. User-visible types are added as the system call
 * interface is defined.
 */

#ifndef _SYS_TYPES_H_
#define _SYS_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#include <machine/types.h>

typedef int64_t         off_t;
typedef int64_t         ssize_t;
typedef uint64_t        ino_t;
typedef uint64_t        dev_t;
typedef uint32_t        mode_t;
typedef uint32_t        uid_t;
typedef uint32_t        gid_t;
typedef uint32_t        nlink_t;
typedef int64_t         daddr_t;        /* disk address, in 512-byte sectors */

#endif /* _SYS_TYPES_H_ */
