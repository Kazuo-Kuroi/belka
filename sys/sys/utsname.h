/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/* uname(), with the System V structure. */

#ifndef _SYS_UTSNAME_H_
#define _SYS_UTSNAME_H_

#define SYS_NMLN        257

struct utsname {
        char    sysname[SYS_NMLN];
        char    nodename[SYS_NMLN];
        char    release[SYS_NMLN];
        char    version[SYS_NMLN];
        char    machine[SYS_NMLN];
};

#endif /* _SYS_UTSNAME_H_ */
