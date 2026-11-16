/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-15
 * SPDX-License-Identifier: MIT
 */

/*
 * nvi's configuration: NetBSD's (its usr.bin/nvi/config.h, which nvi's
 * sources include as "config.h" and find here first), but for what
 * Belka lacks.
 */

#ifndef _COMPAT_NVI_CONFIG_H_
#define _COMPAT_NVI_CONFIG_H_

#include "../../../../contrib/netbsd/external/bsd/nvi/usr.bin/nvi/config.h"

/* mlibc has no iconv(3): files are read and written in the locale's
 * own encoding, with no conversion. */
#undef USE_ICONV

/* Linux's open flag, which mlibc's <fcntl.h> has and NetBSD does not: nvi
 * names an option O_PATH (options_def.h). */
#undef O_PATH

#endif /* _COMPAT_NVI_CONFIG_H_ */
