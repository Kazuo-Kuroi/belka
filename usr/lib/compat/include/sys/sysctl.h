/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * BSD's sysctl(3), as far as imported programs use it: hw.machine,
 * from uname(2). Any other name fails with ENOENT.
 */

#ifndef _COMPAT_SYS_SYSCTL_H_
#define _COMPAT_SYS_SYSCTL_H_

#include <sys/cdefs.h>
#include <stddef.h>

#define CTL_HW          6
#define HW_MACHINE      1

#ifndef _SYS_NMLN
#define _SYS_NMLN       256
#endif

__BEGIN_DECLS

int sysctl(const int *, unsigned int, void *, size_t *, const void *,
    size_t);

__END_DECLS

#endif /* _COMPAT_SYS_SYSCTL_H_ */
