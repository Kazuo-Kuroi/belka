/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * strvis(3), as far as imported programs use it: unprintable bytes as
 * C-style octal escapes, and the backslash doubled. The flags select
 * nothing else. strunvis undoes strvis, and also takes a backslash
 * with n, t, r, or s for newline, tab, return and space.
 */

#ifndef _COMPAT_VIS_H_
#define _COMPAT_VIS_H_

#include <sys/cdefs.h>
#include <stddef.h>

#define VIS_OCTAL       0x0001
#define VIS_CSTYLE      0x0002
#define VIS_SP          0x0004
#define VIS_TAB         0x0008
#define VIS_NL          0x0010
#define VIS_WHITE       (VIS_SP | VIS_TAB | VIS_NL)
#define VIS_SAFE        0x0020

__BEGIN_DECLS

int strvis(char *, const char *, int);
int strnvis(char *, size_t, const char *, int);
int strunvis(char *, const char *);

__END_DECLS

#endif /* _COMPAT_VIS_H_ */
