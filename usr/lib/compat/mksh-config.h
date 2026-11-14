/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * mksh's configuration for Belka over mlibc, written by hand: the
 * HAVE_* answers its Build.sh would find by trying the compiler, and
 * the MKSH_* choices. Included first in each of mksh's sources.
 */

#ifndef _COMPAT_MKSH_CONFIG_H_
#define _COMPAT_MKSH_CONFIG_H_

/* The compiler. */
#define HAVE_ATTRIBUTE_BOUNDED  0
#define HAVE_ATTRIBUTE_FORMAT   1
#define HAVE_ATTRIBUTE_NORETURN 1
#define HAVE_ATTRIBUTE_PURE     1
#define HAVE_ATTRIBUTE_UNUSED   1
#define HAVE_ATTRIBUTE_USED     1
#define HAVE_STRING_POOLING     1
#define HAVE_CAN_INTTYPES       1
#define HAVE_CAN_UCBINTS        1
#define HAVE_CAN_INT8TYPE       1
#define HAVE_CAN_UCBINT8        1

/* Headers. */
#define HAVE_SYS_BSDTYPES_H     0
#define HAVE_SYS_FILE_H         1
#define HAVE_SYS_MKDEV_H        0
#define HAVE_SYS_MMAN_H         1
#define HAVE_SYS_PARAM_H        1
#define HAVE_SYS_RESOURCE_H     1
#define HAVE_SYS_SELECT_H       1
#define HAVE_SYS_SYSMACROS_H    0
#define HAVE_BSTRING_H          0
#define HAVE_GRP_H              1
#define HAVE_IO_H               0
#define HAVE_LIBGEN_H           1
#define HAVE_LIBUTIL_H          0
#define HAVE_PATHS_H            1
#define HAVE_STDINT_H           1
#define HAVE_STRINGS_H          1
#define HAVE_TERMIOS_H          1
#define HAVE_ULIMIT_H           0
#define HAVE_VALUES_H           0
#define HAVE_SYS_TIME_H         1
#define HAVE_TIME_H             1
#define HAVE_BOTH_TIME_H        1

/* Types and functions. */
#define HAVE_SELECT             1
#define HAVE_RLIM_T             1
#define HAVE_SIG_T              0
#define HAVE_SYS_ERRLIST        0
#define HAVE_SYS_ERRLIST_DECL   0
#define HAVE_SYS_SIGNAME        0
#define HAVE_SYS_SIGLIST        0
#define HAVE_SYS_SIGLIST_DECL   0
#define HAVE_FLOCK              1
#define HAVE_FLOCK_DECL         1
#define HAVE_LOCK_FCNTL         1
#define HAVE_GETRUSAGE          1
#define HAVE_GETSID             1
#define HAVE_GETTIMEOFDAY       1
#define HAVE_KILLPG             1
#define HAVE_MEMMOVE            1
#define HAVE_MKNOD              0       /* needs BSD setmode() */
#define HAVE_NICE               0       /* no nice call yet */
#define HAVE_REVOKE             0
#define HAVE_REVOKE_DECL        1       /* the compat library's stand-in */
#define HAVE_RLIMIT             1
#define HAVE_SETGROUPS          1
#define HAVE_SETRESUGID         0
#define HAVE_SETLOCALE_CTYPE    0
#define HAVE_LANGINFO_CODESET   0
#define HAVE_STRERROR           1
#define HAVE_STRSIGNAL          1
#define HAVE_STRLCPY            1
#define HAVE_ST_MTIM            1
#define HAVE_ST_MTIMENSEC       0
#define HAVE_PERSISTENT_HISTORY 0

/* Choices. */
#define MKSH_BUILDSH            1       /* as if built by Build.sh */
#define MKSH_BUILD_R            593     /* R59c */
#define MKSH_DONT_EMIT_IDSTRING 1

#endif /* _COMPAT_MKSH_CONFIG_H_ */
