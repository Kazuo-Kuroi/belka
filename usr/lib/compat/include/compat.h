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

/* <stdio.h> */
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

/* <util.h> */
int ttyaction(const char *, const char *, const char *);
int sockaddr_snprintf(char *, size_t, const char *, const struct sockaddr *);

/* <pwd.h> */
const char *user_from_uid(uid_t, int);
char *crypt(const char *, const char *);

/* <string.h> */
void *explicit_memset(void *, int, size_t);

/* <err.h> */
void errc(int, int, const char *, ...) __attribute__((__noreturn__));
void verrc(int, int, const char *, va_list) __attribute__((__noreturn__));
void warnc(int, const char *, ...);
void vwarnc(int, const char *, va_list);

__END_DECLS

#endif /* _COMPAT_COMPAT_H_ */
