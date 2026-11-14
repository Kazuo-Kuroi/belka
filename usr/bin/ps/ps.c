/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * ps [-aAdefl] [-p pidlist] [-t termlist] [-u userlist]: report on
 * processes, System V's way, from /proc/pinfo (see <sys/procfs.h>),
 * which anyone may read, so ps needs no privilege.
 *
 * Without options it shows the processes with the caller's effective
 * user and controlling terminal. -e and -A show every process; -d every
 * one but session leaders; -a every one but session leaders and those
 * with no terminal. -p, -t and -u select by process id, terminal
 * (console, pts/N) and user (name or number); lists are separated by
 * commas or blanks, and selections add up. -f gives the full listing
 * (user name, parent, start time, arguments), -l the long one (flags,
 * state, priority, nice value, size).
 */

#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../../sys/sys/procfs.h"

#define PINFO           "/proc/pinfo"
#define MAXSEL          64

/* Device numbers: the major in the upper 32 bits (<sys/mkdev.h>). */
#define DEV_CONS_MAJOR  1
#define DEV_PTS_MAJOR   5

static int fflag, lflag, eflag, dflag, aflag;
static long pids[MAXSEL], uids[MAXSEL];
static uint64_t ttys[MAXSEL];
static int npids, nuids, nttys;
static uid_t me;
static uint64_t mytty = PRNODEV;

static void
usage(void)
{
        fprintf(stderr, "usage: ps [-aAdefl] [-p pidlist] [-t termlist] "
            "[-u userlist]\n");
        exit(2);
}

/* A terminal's name as ps shows it: console, pts/N, or "?". */
static void
ttyname_of(uint64_t dev, char *buf, size_t len)
{
        unsigned long major = (unsigned long)(dev >> 32);
        unsigned long minor = (unsigned long)(dev & 0xffffffffu);

        if (dev == PRNODEV)
                snprintf(buf, len, "?");
        else if (major == DEV_CONS_MAJOR)
                snprintf(buf, len, "console");
        else if (major == DEV_PTS_MAJOR)
                snprintf(buf, len, "pts/%lu", minor);
        else
                snprintf(buf, len, "%lu,%lu", major, minor);
}

/* A terminal named on the command line: console, pts/N, /dev/... */
static int
tty_of(const char *name, uint64_t *dev)
{
        char path[64];
        struct stat st;

        if (strncmp(name, "/dev/", 5) == 0)
                name += 5;
        snprintf(path, sizeof(path), "/dev/%s", name);
        if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
                return -1;
        *dev = (uint64_t)st.st_rdev;
        return 0;
}

/* Split a list on commas and blanks, calling fn on each item. */
static void
each(char *list, void (*fn)(const char *))
{
        for (char *s = strtok(list, ", \t"); s != NULL;
            s = strtok(NULL, ", \t"))
                fn(s);
}

static void
add_pid(const char *s)
{
        char *end;
        long v = strtol(s, &end, 10);

        if (*end != '\0' || v < 0) {
                fprintf(stderr, "ps: bad process id: %s\n", s);
                exit(2);
        }
        if (npids < MAXSEL)
                pids[npids++] = v;
}

static void
add_user(const char *s)
{
        struct passwd *pw = getpwnam(s);
        char *end;
        long v;

        if (pw != NULL) {
                v = (long)pw->pw_uid;
        } else {
                v = strtol(s, &end, 10);
                if (*end != '\0' || v < 0) {
                        fprintf(stderr, "ps: unknown user: %s\n", s);
                        exit(2);
                }
        }
        if (nuids < MAXSEL)
                uids[nuids++] = v;
}

static void
add_tty(const char *s)
{
        uint64_t dev;

        if (tty_of(s, &dev) != 0) {
                fprintf(stderr, "ps: unknown terminal: %s\n", s);
                exit(2);
        }
        if (nttys < MAXSEL)
                ttys[nttys++] = dev;
}

/* Whether ps should show the process. */
static int
selected(const struct prpsinfo *ps)
{
        int any = npids != 0 || nuids != 0 || nttys != 0;
        int leader = ps->pr_pid == ps->pr_sid;

        if (eflag)
                return 1;
        if (dflag && !leader)
                return 1;
        if (aflag && !leader && ps->pr_ttydev != PRNODEV)
                return 1;
        for (int i = 0; i < npids; i++)
                if (pids[i] == ps->pr_pid)
                        return 1;
        for (int i = 0; i < nuids; i++)
                if ((uid_t)uids[i] == ps->pr_euid)
                        return 1;
        for (int i = 0; i < nttys; i++)
                if (ttys[i] == ps->pr_ttydev)
                        return 1;
        if (any || dflag || aflag)
                return 0;
        /* The default: our own processes on our terminal. */
        return ps->pr_euid == me && ps->pr_ttydev == mytty &&
            mytty != PRNODEV;
}

static void
fmt_time(const int64_t t[2], char *buf, size_t len)
{
        long secs = (long)t[0];

        snprintf(buf, len, "%ld:%02ld", secs / 60, secs % 60);
}

/* Start time: the clock time today, else the month and day. */
static void
fmt_stime(const int64_t t[2], char *buf, size_t len)
{
        time_t now = time(NULL), start = (time_t)t[0];
        struct tm tn, ts;

        gmtime_r(&now, &tn);
        gmtime_r(&start, &ts);
        if (tn.tm_year == ts.tm_year && tn.tm_yday == ts.tm_yday)
                strftime(buf, len, "%H:%M:%S", &ts);
        else
                strftime(buf, len, "%b %d", &ts);
}

static void
header(void)
{
        if (lflag)
                printf(" F S");
        if (fflag)
                printf("      UID");
        else if (lflag)
                printf("   UID");
        printf("   PID");
        if (fflag || lflag)
                printf("  PPID  C");
        if (lflag)
                printf(" PRI NI     SZ");
        if (fflag)
                printf("    STIME");
        printf(" TTY        TIME CMD\n");
}

static void
show(const struct prpsinfo *ps)
{
        char tty[32], tbuf[32], sbuf[32];

        ttyname_of(ps->pr_ttydev, tty, sizeof(tty));
        fmt_time(ps->pr_time, tbuf, sizeof(tbuf));
        if (lflag)
                printf("%2x %c", (unsigned int)(ps->pr_flag >> 8),
                    ps->pr_sname);
        if (fflag) {
                struct passwd *pw = getpwuid(ps->pr_euid);

                if (pw != NULL)
                        printf(" %8.8s", pw->pw_name);
                else
                        printf(" %8u", (unsigned int)ps->pr_euid);
        } else if (lflag) {
                printf(" %5u", (unsigned int)ps->pr_euid);
        }
        printf(" %5d", (int)ps->pr_pid);
        if (fflag || lflag)
                printf(" %5d %2d", (int)ps->pr_ppid, (int)ps->pr_cpu);
        if (lflag)
                printf(" %3d %2d %6lu", (int)ps->pr_pri, (int)ps->pr_nice,
                    (unsigned long)ps->pr_size);
        if (fflag) {
                fmt_stime(ps->pr_start, sbuf, sizeof(sbuf));
                printf(" %8s", sbuf);
        }
        printf(" %-8s %6s ", tty, tbuf);
        if (ps->pr_zomb)
                printf("<defunct>\n");
        else
                printf("%s\n", fflag ? ps->pr_psargs : ps->pr_fname);
}

int
main(int argc, char **argv)
{
        struct dirent *de;
        DIR *dir;
        int c, status = 0;

        while ((c = getopt(argc, argv, "aAdeflp:t:u:")) != -1) {
                switch (c) {
                case 'a':
                        aflag = 1;
                        break;
                case 'A':
                case 'e':
                        eflag = 1;
                        break;
                case 'd':
                        dflag = 1;
                        break;
                case 'f':
                        fflag = 1;
                        break;
                case 'l':
                        lflag = 1;
                        break;
                case 'p':
                        each(optarg, add_pid);
                        break;
                case 't':
                        each(optarg, add_tty);
                        break;
                case 'u':
                        each(optarg, add_user);
                        break;
                default:
                        usage();
                }
        }
        if (optind != argc)
                usage();
        me = geteuid();
        {
                /* Our own terminal, from our own entry. */
                char path[64];
                struct prpsinfo self;
                int fd;

                snprintf(path, sizeof(path), PINFO "/%05d", (int)getpid());
                if ((fd = open(path, O_RDONLY)) >= 0) {
                        if (ioctl(fd, PIOCPSINFO, &self) == 0)
                                mytty = self.pr_ttydev;
                        close(fd);
                }
        }
        if ((dir = opendir(PINFO)) == NULL) {
                fprintf(stderr, "ps: %s: %s\n", PINFO, strerror(errno));
                return 1;
        }
        header();
        while ((de = readdir(dir)) != NULL) {
                char path[sizeof(PINFO) + 1 + sizeof(de->d_name)];
                struct prpsinfo ps;
                int fd;

                if (de->d_name[0] < '0' || de->d_name[0] > '9')
                        continue;
                snprintf(path, sizeof(path), PINFO "/%s", de->d_name);
                if ((fd = open(path, O_RDONLY)) < 0)
                        continue;       /* gone since the directory read */
                if (ioctl(fd, PIOCPSINFO, &ps) != 0) {
                        close(fd);
                        continue;
                }
                close(fd);
                if (selected(&ps))
                        show(&ps);
        }
        closedir(dir);
        return status;
}
