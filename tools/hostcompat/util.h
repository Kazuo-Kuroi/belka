/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/* <util.h> for host builds: libutil's efun(3), in contrib/netbsd. */

#ifndef _HOSTCOMPAT_UTIL_H_
#define _HOSTCOMPAT_UTIL_H_

#include <stdarg.h>
#include <stddef.h>

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
int evasprintf(char **, const char *, va_list);

#endif /* _HOSTCOMPAT_UTIL_H_ */
