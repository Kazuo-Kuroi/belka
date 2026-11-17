/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * BSD functions that mlibc lacks, for imported programs; see compat.h.
 * Some are stand-ins until Belka has what they need:
 *
 *   setlogin   accepted and forgotten: the kernel keeps no login name
 *   revoke     does nothing: there is no call to revoke a terminal from
 *              the processes that have it open (a new session's
 *              TIOCSCTTY still takes it over)
 *   logout, logwtmp
 *              do nothing: there is no utmp or wtmp
 *   arc4random a small generator seeded from the clock and process id,
 *              NOT for secrets: there is no kernel source of randomness
 */

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#include "compat.h"
#include "util.h"

extern char *program_invocation_short_name;

static const char *progname;

const char *
getprogname(void)
{
        return progname != NULL ? progname : program_invocation_short_name;
}

void
setprogname(const char *name)
{
        const char *slash = strrchr(name, '/');

        progname = slash != NULL ? slash + 1 : name;
}

int
setlogin(const char *name)
{
        if (strlen(name) >= 32) {
                errno = EINVAL;
                return -1;
        }
        if (geteuid() != 0) {
                errno = EPERM;
                return -1;
        }
        return 0;
}

int
revoke(const char *path)
{
        return access(path, F_OK);
}

int
logout(const char *line)
{
        (void)line;
        return 0;
}

void
logwtmp(const char *line, const char *name, const char *host)
{
        (void)line;
        (void)name;
        (void)host;
}

static uint64_t rng_state;

uint32_t
arc4random(void)
{
        uint64_t x;

        if (rng_state == 0) {
                struct timespec ts;

                clock_gettime(CLOCK_MONOTONIC, &ts);
                rng_state = ((uint64_t)ts.tv_sec << 32 ^ (uint64_t)ts.tv_nsec ^
                    (uint64_t)getpid() << 16) | 1;
        }
        /* xorshift64* */
        x = rng_state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        rng_state = x;
        return (uint32_t)((x * 0x2545f4914f6cdd1dULL) >> 32);
}

uint32_t
arc4random_uniform(uint32_t bound)
{
        uint32_t r, min;

        if (bound < 2)
                return 0;
        min = -bound % bound;   /* 2**32 mod bound: reject below it */
        do
                r = arc4random();
        while (r < min);
        return r % bound;
}

long long
strtonum(const char *s, long long lo, long long hi, const char **errstrp)
{
        long long v = 0;
        char *end;
        const char *err = NULL;

        if (lo > hi) {
                err = "invalid";
                errno = EINVAL;
        } else {
                errno = 0;
                v = strtoll(s, &end, 10);
                if (end == s || *end != '\0') {
                        err = "invalid";
                        errno = EINVAL;
                } else if ((v == LLONG_MIN && errno == ERANGE) || v < lo) {
                        err = "too small";
                        errno = ERANGE;
                } else if ((v == LLONG_MAX && errno == ERANGE) || v > hi) {
                        err = "too large";
                        errno = ERANGE;
                }
        }
        if (errstrp != NULL)
                *errstrp = err;
        return err != NULL ? 0 : v;
}

/* memset that the compiler may not drop as a dead store. */
void *
explicit_memset(void *p, int c, size_t n)
{
        void *(*volatile fn)(void *, int, size_t) = memset;

        return fn(p, c, n);
}

/* fgetln over getline: the line stays valid until the next call. */
char *
compat_fgetln(FILE *fp, size_t *lenp)
{
        static char *buf;
        static size_t size;
        ssize_t n = getline(&buf, &size, fp);

        if (n < 0)
                return NULL;
        *lenp = (size_t)n;
        return buf;
}

int
sysctl(const int *name, unsigned int namelen, void *old, size_t *oldlenp,
    const void *new, size_t newlen)
{
        struct utsname u;
        size_t len;

        (void)newlen;
        if (namelen != 2 || name[0] != CTL_HW || name[1] != HW_MACHINE ||
            new != NULL) {
                errno = ENOENT;
                return -1;
        }
        if (uname(&u) != 0)
                return -1;
        len = strlen(u.machine) + 1;
        if (old != NULL) {
                if (*oldlenp < len) {
                        errno = ENOMEM;
                        return -1;
                }
                memcpy(old, u.machine, len);
        }
        *oldlenp = len;
        return 0;
}

/* Whether the program was started set-user-ID or set-group-ID. */
int
issetugid(void)
{
        return getauxval(AT_SECURE) != 0;
}

/* No process titles: ps shows the command line. */
void
setproctitle(const char *fmt, ...)
{
        (void)fmt;
}

/* No /etc/ttyaction. */
int
ttyaction(const char *tty, const char *act, const char *user)
{
        (void)tty;
        (void)act;
        (void)user;
        return 0;
}

/* The name of user uid, or (unless nouser) its number. */
const char *
user_from_uid(uid_t uid, int nouser)
{
        static char num[24];
        struct passwd *pw = getpwuid(uid);

        if (pw != NULL)
                return pw->pw_name;
        if (nouser)
                return NULL;
        snprintf(num, sizeof(num), "%u", (unsigned int)uid);
        return num;
}

/* No networking yet: an address prints as "?". */
int
sockaddr_snprintf(char *buf, size_t len, const char *fmt,
    const struct sockaddr *sa)
{
        (void)fmt;
        (void)sa;
        return snprintf(buf, len, "?");
}

/* fpurge: discard what is buffered, as mlibc's __fpurge does. */
int
fpurge(FILE *fp)
{
        __fpurge(fp);
        return 0;
}

/*
 * pw's password, if it is "x", from /etc/shadow (getspnam): login and
 * init check it (built with BELKA_SHADOW). pw is left alone if there is
 * no shadow entry, which only root can read.
 *
 * pw is mlibc's getpwnam entry, whose fields that library allocates and
 * frees at its next call; the hash takes the place of its "x" the same
 * way, so that the library frees it in turn.
 */
void
compat_shadow(struct passwd *pw)
{
        struct spwd *sp;
        char *hash;

        if (strcmp(pw->pw_passwd, "x") != 0 ||
            (sp = getspnam(pw->pw_name)) == NULL ||
            (hash = strdup(sp->sp_pwdp)) == NULL)
                return;
        free(pw->pw_passwd);
        pw->pw_passwd = hash;
}

const char *
group_from_gid(gid_t gid, int nogroup)
{
        static char num[24];
        struct group *gr = getgrgid(gid);

        if (gr != NULL)
                return gr->gr_name;
        if (nogroup)
                return NULL;
        snprintf(num, sizeof(num), "%u", (unsigned int)gid);
        return num;
}

/* Belka has no file flags (see compat.h). */
int
chflags(const char *path, unsigned long flags)
{
        (void)path;
        (void)flags;
        errno = EOPNOTSUPP;
        return -1;
}

/* No flag names are known: any string is refused (find -flags none works). */
int
string_to_flags(char **sp, unsigned long *setp, unsigned long *clrp)
{
        (void)sp;
        *setp = 0;
        if (clrp != NULL)
                *clrp = 0;
        return 1;
}

/*
 * strmode: the ten characters ls -l shows for a mode, a space, and a
 * NUL, in buf (12 bytes): the type, then three each for the owner, the
 * group and others, with the set-ID and sticky bits in the x places.
 */
void
strmode(mode_t mode, char *buf)
{
        static const char rwx[] = "rwxrwxrwx";
        char *p = buf;

        switch (mode & S_IFMT) {
        case S_IFDIR:   *p++ = 'd'; break;
        case S_IFCHR:   *p++ = 'c'; break;
        case S_IFBLK:   *p++ = 'b'; break;
        case S_IFLNK:   *p++ = 'l'; break;
        case S_IFIFO:   *p++ = 'p'; break;
        case S_IFSOCK:  *p++ = 's'; break;
        case S_IFREG:   *p++ = '-'; break;
        default:        *p++ = '?'; break;
        }
        for (int i = 0; i < 9; i++)
                *p++ = (mode & (0400 >> i)) ? rwx[i] : '-';
        if (mode & S_ISUID)
                buf[3] = buf[3] == 'x' ? 's' : 'S';
        if (mode & S_ISGID)
                buf[6] = buf[6] == 'x' ? 's' : 'S';
        if (mode & S_ISVTX)
                buf[9] = buf[9] == 'x' ? 't' : 'T';
        *p++ = ' ';
        *p = '\0';
}

/*
 * consttime_memequal: whether the n bytes at a and b are the same (1)
 * or not (0), taking as long whatever they are, for comparing secrets.
 */
int
consttime_memequal(const void *a, const void *b, size_t n)
{
        const volatile unsigned char *p = a, *q = b;
        unsigned char diff = 0;

        for (size_t i = 0; i < n; i++)
                diff |= p[i] ^ q[i];
        return diff == 0;
}
