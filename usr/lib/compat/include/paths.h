/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * mlibc's <paths.h>, with the utmpx and wtmpx records in System V's
 * places, as Belka's <utmpx.h> has them (mlibc's gives Linux's).
 */

#ifndef _COMPAT_PATHS_H_
#define _COMPAT_PATHS_H_

#include_next <paths.h>

#undef _PATH_UTMPX
#undef _PATH_WTMPX
#define _PATH_UTMPX     "/var/adm/utmpx"
#define _PATH_WTMPX     "/var/adm/wtmpx"

#endif /* _COMPAT_PATHS_H_ */
