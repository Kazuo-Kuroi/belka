/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Included first in every imported program (-include compat.h): the
 * BSD functions that mlibc lacks and the compat library provides, in
 * the headers BSD declares them in.
 */

#ifndef _COMPAT_COMPAT_H_
#define _COMPAT_COMPAT_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <termios.h>        /* struct winsize, which mlibc keeps here */
#include <sys/ioctl.h>
#include <sys/endian.h>
#include <stdarg.h>

/*
 * regex(3) is NetBSD's (contrib/netbsd/lib/libc/regex, with its
 * <regex.h>), under names of its own, so that it never stands in for
 * mlibc's, whose regex_t is not the same.
 */
#define regcomp         compat_regcomp
#define regexec         compat_regexec
#define regerror        compat_regerror
#define regfree         compat_regfree
#define regnsub         compat_regnsub
#define regasub         compat_regasub

/* NetBSD's internal assertions: off. */
#define _DIAGASSERT(e)  ((void)0)

__BEGIN_DECLS

/* <stdlib.h> */
const char *getprogname(void);
void setprogname(const char *);
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t);
long long strtonum(const char *, long long, long long, const char **);
int reallocarr(void *, size_t, size_t);
void mi_vector_hash(const void *, size_t, uint32_t, uint32_t[3]);
intmax_t strtoi(const char *, char **, int, intmax_t, intmax_t, int *);
uintmax_t strtou(const char *, char **, int, uintmax_t, uintmax_t, int *);

/* <search.h> */
void hdestroy1(void (*)(void *), void (*)(void *));

/* <fcntl.h>: BSD's locking opens; Belka has none. */
#ifndef O_EXLOCK
#define O_EXLOCK        0
#define O_SHLOCK        0
#endif

/* <errno.h>: NetBSD's "inappropriate file type or format" (db). */
#ifndef EFTYPE
#define EFTYPE          EINVAL
#endif

/* <signal.h> */
typedef void (*sig_t)(int);

/* <sys/param.h> and <pwd.h> */
#ifndef MAXLOGNAME
#define MAXLOGNAME      16
#endif
#define _PASSWORD_WARNDAYS      14
#define _PASSWORD_CHGNOW        -1
#define _PASSWORD_LEN           128

/* <sys/stat.h> */
#ifndef DEFFILEMODE
#define DEFFILEMODE     0666
#endif
#ifndef ALLPERMS
#define ALLPERMS        07777
#endif
#define st_atimespec    st_atim         /* BSD's names for them */
#define st_mtimespec    st_mtim
#define st_ctimespec    st_ctim

/* <utmpx.h>: the names of the records init writes (NetBSD's). */
#define BOOT_MSG        "system boot"
#define DOWN_MSG        "system down"
#define RUNLVL_MSG      "run-level %c"
#ifndef DOWN_TIME
#define DOWN_TIME       11              /* NetBSD's: the system went down */
#endif

/* <paths.h> */
#define _PATH_CONSTTY   "/dev/constty"
#include <paths.h>               /* the compat one: utmpx paths */

/*
 * <sys/stat.h>: BSD's file flags. Belka has none: st_flags reads one
 * of struct stat's spare fields, which the kernel always zeroes, and
 * chflags fails (misc.c).
 */
#define st_flags        __st_spare[0]
#ifndef UF_IMMUTABLE
#define UF_IMMUTABLE    0x00000002
#define UF_APPEND       0x00000004
#define SF_IMMUTABLE    0x00020000
#define SF_APPEND       0x00040000
#endif
int chflags(const char *, unsigned long);
int string_to_flags(char **, unsigned long *, unsigned long *);
void strmode(mode_t, char *);

/*
 * Device numbers, Belka's way (the major in the upper 32 bits); not
 * mlibc's <sys/sysmacros.h>, which has Linux's.
 */
#undef major
#undef minor
#undef makedev
#define major(d)        ((unsigned int)((uint64_t)(d) >> 32))
#define minor(d)        ((unsigned int)((uint64_t)(d) & 0xffffffffu))
#define makedev(ma, mi) ((dev_t)(((uint64_t)(ma) << 32) | (uint32_t)(mi)))

/* <limits.h>: exec's limit on arguments and environment (the kernel's). */
#ifndef ARG_MAX
#define ARG_MAX         65536
#endif

/* <unistd.h>: a pathconf name mlibc lacks; pathconf refuses it. */
#ifndef _PC_SYNC_IO
#define _PC_SYNC_IO     (-1)
#endif

/* <sys/stat.h>: BSD's old name for the sticky bit; setmode(3), getmode. */
#ifndef S_ISTXT
#define S_ISTXT         S_ISVTX
#endif
void *setmode(const char *);
mode_t getmode(const void *, mode_t);

/* <sys/time.h> */
#ifndef timespeccmp
#define timespeccmp(a, b, op) \
        ((a)->tv_sec == (b)->tv_sec ? (a)->tv_nsec op (b)->tv_nsec : \
        (a)->tv_sec op (b)->tv_sec)
#endif

/*
 * <sys/statvfs.h>: NetBSD's names for System V's fields and flags.
 * Belka's file systems are all local; MNT_LOCAL is a bit no f_flag has,
 * which find (BELKA_ALL_LOCAL) sets itself.
 */
#include <sys/statvfs.h>
#define f_fstypename    f_basetype
#define MNT_RDONLY      ST_RDONLY
#define MNT_LOCAL       0x40000000UL

/* <signal.h>: BSD's ^T status signal; Belka has none, and sigaction
 * refuses 0, so a handler for it is never installed. */
#ifndef SIGINFO
#define SIGINFO         0
#endif

/* <util.h>: a written date (parsedate.c, a small subset of NetBSD's). */
time_t parsedate(const char *, const time_t *, const int *);

/* <signal.h>: signal names without "SIG" (signame.c). */
extern const char *const sys_signame[];

/* <stdio.h> */
int fpurge(FILE *);
#define snprintf_ss     snprintf        /* NetBSD's signal-safe one */
#define FPARSELN_UNESCESC       0x01
#define FPARSELN_UNESCCONT      0x02
#define FPARSELN_UNESCCOMM      0x04
#define FPARSELN_UNESCREST      0x08
#define FPARSELN_UNESCALL       0x0f
char *fparseln(FILE *, size_t *, size_t *, const char[3], int);
char *compat_fgetln(FILE *, size_t *);
#define fgetln          compat_fgetln   /* mlibc's is not implemented */

/* <unistd.h> */
int setlogin(const char *);
int revoke(const char *);
int issetugid(void);
void setproctitle(const char *, ...);

/* <utmpx.h>: each user's last login, as NetBSD keeps it (lastlogx.c). */
#define _PATH_LASTLOGX  "/var/adm/lastlogx"
struct lastlogx {
        struct timeval          ll_tv;
        char                    ll_line[32];
        char                    ll_host[256];
        struct sockaddr_storage ll_ss;
};
struct lastlogx *getlastlogx(const char *, uid_t, struct lastlogx *);
int updlastlogx(const char *, uid_t, struct lastlogx *);

/* <util.h> */
int ttyaction(const char *, const char *, const char *);
int sockaddr_snprintf(char *, size_t, const char *, const struct sockaddr *);

/* <pwd.h> */
const char *user_from_uid(uid_t, int);
const char *group_from_gid(gid_t, int);
struct passwd;
void compat_shadow(struct passwd *);
/* /etc/shadow's ageing fields, in days since 1970; -1: none (misc.c). */
struct compat_age {
        long    lastchg, min, max, warn, inact, expire;
};
int compat_shadow_age(const char *, struct compat_age *);
char *crypt(const char *, const char *);

/* <string.h> */
void *explicit_memset(void *, int, size_t);
int consttime_memequal(const void *, const void *, size_t);

/* <err.h> */
void errc(int, int, const char *, ...) __attribute__((__noreturn__));
void verrc(int, int, const char *, va_list) __attribute__((__noreturn__));
void warnc(int, const char *, ...);
void vwarnc(int, const char *, va_list);

__END_DECLS

#endif /* _COMPAT_COMPAT_H_ */
