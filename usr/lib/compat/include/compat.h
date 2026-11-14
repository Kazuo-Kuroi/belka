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

/* NetBSD's internal assertions: off. */
#define _DIAGASSERT(e)  ((void)0)

__BEGIN_DECLS

/* <stdlib.h> */
const char *getprogname(void);
void setprogname(const char *);
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t);
long long strtonum(const char *, long long, long long, const char **);

/* <signal.h> */
typedef void (*sig_t)(int);

/* <sys/param.h> and <pwd.h> */
#ifndef MAXLOGNAME
#define MAXLOGNAME      16
#endif
#define _PASSWORD_WARNDAYS      14
#define _PASSWORD_CHGNOW        -1
#define _PASSWORD_LEN           128

/* <paths.h> */
#define _PATH_CONSTTY   "/dev/constty"

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

__END_DECLS

#endif /* _COMPAT_COMPAT_H_ */
