/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Commands and functions for uadmin(2) and the kernel's own shutdown
 * path. The values are those of System V Release 4.
 */

#ifndef _SYS_UADMIN_H_
#define _SYS_UADMIN_H_

/* Commands. */
#define A_REBOOT        1
#define A_SHUTDOWN      2
#define A_REMOUNT       4

/* Functions for A_REBOOT and A_SHUTDOWN. */
#define AD_HALT         0       /* halt the processors */
#define AD_BOOT         1       /* reboot */
#define AD_IBOOT        2       /* reboot, asking for a boot program */

#endif /* _SYS_UADMIN_H_ */
