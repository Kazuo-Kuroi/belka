/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-16
 * SPDX-License-Identifier: MIT
 */

/*
 * sysinfo(command, buf, count), System V's (call 139): the names uname
 * reports, one at a time, and setting the host name. It copies the
 * string, cut short and ended with a NUL if count is too small, and
 * returns the size it needs, with its NUL. Setting is the superuser's;
 * the name is then what uname's nodename is.
 */

#ifndef _SYS_SYSTEMINFO_H_
#define _SYS_SYSTEMINFO_H_

#define SI_SYSNAME              1
#define SI_HOSTNAME             2
#define SI_RELEASE              3
#define SI_VERSION              4
#define SI_MACHINE              5
#define SI_ARCHITECTURE         6
#define SI_SET_HOSTNAME         258

#endif /* _SYS_SYSTEMINFO_H_ */
