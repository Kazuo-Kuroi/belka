/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#include "../../../../contrib/netbsd/external/bsd/less/dist/defines.h"

/* Use POSIX signals and terminfo's termcap emulation on Belka. */
#undef HAVE_SIGSETMASK
#define HAVE_SIGSETMASK 0
#undef HAVE_SYS_ERRLIST
#define HAVE_SYS_ERRLIST 0
#undef HAVE_OSPEED
#define HAVE_OSPEED 0

#ifndef __P
#define __P(args) args
#endif
