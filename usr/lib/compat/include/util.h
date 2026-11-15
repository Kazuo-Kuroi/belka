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

#include <utmpx.h>

__BEGIN_DECLS

int logout(const char *);
void logwtmp(const char *, const char *, const char *);

/* The utmpx and wtmpx records (contrib/netbsd/lib/libutil). */
void loginx(const struct utmpx *);
int logoutx(const char *, int, int);
void logwtmpx(const char *, const char *, const char *, int, int);

/* efun(3), from NetBSD's libutil (contrib/netbsd/lib/libutil/efun.c). */
void (*esetfunc(void (*)(int, const char *, ...)))(int, const char *, ...);
size_t estrlcpy(char *, const char *, size_t);
size_t estrlcat(char *, const char *, size_t);
char *estrdup(const char *);
char *estrndup(const char *, size_t);
void *emalloc(size_t);
void *ecalloc(size_t, size_t);
void *erealloc(void *, size_t);
void ereallocarr(void *, size_t, size_t);
int easprintf(char **, const char *, ...);

__END_DECLS

#endif /* _COMPAT_UTIL_H_ */
