/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * The BSD <sys/cdefs.h> macros that imported programs (contrib/netbsd,
 * contrib/mksh) use, over mlibc, which has no such header. Only the
 * compat library and those programs see this directory.
 */

#ifndef _COMPAT_SYS_CDEFS_H_
#define _COMPAT_SYS_CDEFS_H_

#ifdef __cplusplus
#define __BEGIN_DECLS   extern "C" {
#define __END_DECLS     }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

/* Identification strings: dropped. */
#define __RCSID(s)              struct __rcsid_unused
#define __COPYRIGHT(s)          struct __copyright_unused
#define __KERNEL_RCSID(n, s)    struct __rcsid_unused

#define __dead                  __attribute__((__noreturn__))
#ifndef __unused
#define __unused                __attribute__((__unused__))
#endif
#define __used                  __attribute__((__used__))
#define __packed                __attribute__((__packed__))
#define __aligned(x)            __attribute__((__aligned__(x)))
#define __printflike(f, a)      __attribute__((__format__(__printf__, f, a)))
#define __scanflike(f, a)       __attribute__((__format__(__scanf__, f, a)))
#define __sysloglike(f, a)      __attribute__((__format__(__printf__, f, a)))
#define __predict_true(e)       __builtin_expect((e) != 0, 1)
#define __predict_false(e)      __builtin_expect((e) != 0, 0)
#define __GNUC_PREREQ__(x, y)   (__GNUC__ > (x) || \
                                 (__GNUC__ == (x) && __GNUC_MINOR__ >= (y)))
#define __arraycount(a)         (sizeof(a) / sizeof((a)[0]))

/* Drop const from a pointer without a warning. */
#define __UNCONST(p)            ((void *)(unsigned long)(const void *)(p))
#define __UNVOLATILE(p)         ((void *)(unsigned long)(volatile void *)(p))

#define __aconst                /* nothing: NetBSD's "const in C++" */
#define __CONCAT(x, y)          x ## y
#define __STRING(x)             #x

/* NetBSD's versioned symbol names: the plain ones here. */
#define __RENAME(x)

/* Link-time warnings about a symbol: none. */
#define __warn_references(sym, msg)
#define __nothing               ((void)0)
#define __USE(a)                ((void)(a))
#define __strong_alias(alias, sym) \
        extern __typeof(sym) alias __attribute__((__alias__(#sym)));

/* NetBSD's name for the compiler's va_list. */
#define __va_list               __builtin_va_list

#endif /* _COMPAT_SYS_CDEFS_H_ */
