/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * The part of BSD's <util.h> (libutil) that imported programs use:
 * the terminal functions are mlibc's; the login records do nothing,
 * since Belka keeps no utmp or wtmp yet.
 */

#ifndef _COMPAT_UTIL_H_
#define _COMPAT_UTIL_H_

#include <sys/cdefs.h>
#include <pty.h>
#include <utmp.h>

__BEGIN_DECLS

int logout(const char *);
void logwtmp(const char *, const char *, const char *);

__END_DECLS

#endif /* _COMPAT_UTIL_H_ */
