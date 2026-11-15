/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Included first in the NetBSD sources built for the host
 * (tools/mkpasswd, the host's tic): what NetBSD's headers give them and
 * glibc's do not.
 */

#ifndef _HOSTCOMPAT_HOST_H_
#define _HOSTCOMPAT_HOST_H_

#include <stddef.h>
#include <stdint.h>

#define __RCSID(s)              struct __rcsid_unused
#define __COPYRIGHT(s)          struct __copyright_unused
#define _DIAGASSERT(e)          ((void)0)
#define __arraycount(a)         (sizeof(a) / sizeof((a)[0]))
#define __UNCONST(p)            ((void *)(uintptr_t)(const void *)(p))
#define __dead                  __attribute__((__noreturn__))
#define __printflike(f, a)      __attribute__((__format__(__printf__, f, a)))
#define __predict_false(e)      __builtin_expect((e) != 0, 0)
#define __predict_true(e)       __builtin_expect((e) != 0, 1)
#define __GNUC_PREREQ__(x, y)   (__GNUC__ > (x) || \
                                 (__GNUC__ == (x) && __GNUC_MINOR__ >= (y)))
#ifndef __unused
#define __unused                __attribute__((__unused__))
#endif

void *explicit_memset(void *, int, size_t);
int reallocarr(void *, size_t, size_t);
void hdestroy1(void (*)(void *), void (*)(void *));
void mi_vector_hash(const void *, size_t, uint32_t, uint32_t[3]);
intmax_t strtoi(const char *, char **, int, intmax_t, intmax_t, int *);
uintmax_t strtou(const char *, char **, int, uintmax_t, uintmax_t, int *);

extern char *program_invocation_short_name;
#define getprogname()           ((const char *)program_invocation_short_name)
#define setprogname(n)          ((void)(n))

#endif /* _HOSTCOMPAT_HOST_H_ */
