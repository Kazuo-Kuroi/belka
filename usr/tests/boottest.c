/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * The boot test, run by /etc/rc.local on the test root image while init
 * brings the system up (tools/mkroot.sh -t): the system as booted, with
 * NetBSD's init, Belka's getty, NetBSD's login and mksh.
 *
 * It checks that it runs as root under init (process 1) with /dev
 * mounted; that an orphan goes to init; that /proc reports processes;
 * that the utilities work (usr/tests/utiltest.sh, and their own suite);
 * on a pseudo-terminal, that getty and login turn away a wrong password,
 * refuse root on a line /etc/ttys does not mark secure, and log the test
 * user in with the right password, to a shell with the user's IDs, home
 * and name, which who lists, and where passwd changes the password, so
 * that the old one fails and the new one works; and that utmpx and
 * wtmpx have the records.
 * Then it halts the system with /sbin/halt, after giving init time to
 * start the console's getty.
 *
 * It prints "boottest: N failures"; the test run looks for 0.
 *
 * usage: boottest user password uid home
 */

#define _GNU_SOURCE             /* posix_openpt */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#include "../../sys/sys/procfs.h"

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("boottest: %d: %s\n", __LINE__, #expr);  \
                        failures++;                                     \
                }                                                       \
        } while (0)

static long
now_ms(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* What the master has read so far, for the expectations. */
static char seen[8192];
static size_t nseen;

/*
 * Read from the master until want appears in what was read since the
 * last match (then drop up to it), for at most ms. 1 if it came.
 */
static int
expect(int m, const char *want, long ms)
{
        long end = now_ms() + ms;

        for (;;) {
                struct pollfd pfd = { m, POLLIN, 0 };
                char *hit;
                long left;
                ssize_t n;

                seen[nseen] = '\0';
                if ((hit = strstr(seen, want)) != NULL) {
                        size_t used = (size_t)(hit - seen) + strlen(want);

                        memmove(seen, seen + used, nseen - used);
                        nseen -= used;
                        return 1;
                }
                if ((left = end - now_ms()) <= 0)
                        break;
                if (poll(&pfd, 1, (int)left) <= 0)
                        continue;
                if (nseen + 1 >= sizeof(seen)) {
                        /* Keep the tail: a match may straddle it. */
                        memmove(seen, seen + sizeof(seen) / 2,
                            nseen - sizeof(seen) / 2);
                        nseen -= sizeof(seen) / 2;
                }
                n = read(m, seen + nseen, sizeof(seen) - 1 - nseen);
                if (n <= 0)
                        break;
                nseen += (size_t)n;
        }
        seen[nseen] = '\0';
        printf("boottest: wanted \"%s\"; the line said \"%s\"\n", want, seen);
        return 0;
}

static void
say(int m, const char *s)
{
        (void)write(m, s, strlen(s));
}

/* A pseudo-terminal with getty on its slave; the master, or -1. */
static int
start_getty(pid_t *pidp)
{
        int m = posix_openpt(O_RDWR | O_NOCTTY);
        const char *slave;
        pid_t pid;

        if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0 ||
            (slave = ptsname(m)) == NULL)
                return -1;
        if ((pid = fork()) == 0) {
                close(m);
                execl("/usr/libexec/getty", "getty", "std", slave,
                    (char *)NULL);
                _exit(127);
        }
        *pidp = pid;
        nseen = 0;
        return m;
}

/* Wait up to ms for pid; its status, or -1 (then it is killed). */
static int
reap(pid_t pid, long ms)
{
        long end = now_ms() + ms;
        int status;

        while (now_ms() < end) {
                pid_t r = waitpid(pid, &status, WNOHANG);

                if (r == pid)
                        return status;
                if (r < 0)
                        return -1;
                usleep(20000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
}

/* Root, under init, with devfs on /dev. */
static void
test_setting(void)
{
        struct stat st;

        CHECK(getuid() == 0 && geteuid() == 0);
        CHECK(kill(1, 0) == 0);
        CHECK(stat("/dev/console", &st) == 0 && S_ISCHR(st.st_mode));
        CHECK(stat("/dev/ptmx", &st) == 0 && S_ISCHR(st.st_mode));
}

/* A process whose parent exits goes to init. */
static void
test_orphan(void)
{
        int p[2];
        pid_t pid, ppid = 0;
        int status;

        CHECK(pipe(p) == 0);
        if ((pid = fork()) == 0) {
                if (fork() == 0) {
                        pid_t was = getppid(), now;

                        /* Wait for the middle process to be gone. */
                        for (int i = 0; i < 200 && (now = getppid()) == was;
                            i++)
                                usleep(10000);
                        now = getppid();
                        (void)write(p[1], &now, sizeof(now));
                        _exit(0);
                }
                _exit(0);
        }
        close(p[1]);
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(read(p[0], &ppid, sizeof(ppid)) == (ssize_t)sizeof(ppid));
        CHECK(ppid == 1);
        close(p[0]);
}

/* PIOCPSINFO on /proc/pinfo/NNNNN; 0, or the error. */
static int
psinfo(pid_t pid, struct prpsinfo *ps)
{
        char path[64];
        int fd, r;

        snprintf(path, sizeof(path), "/proc/pinfo/%05d", (int)pid);
        if ((fd = open(path, O_RDONLY)) < 0)
                return errno;
        r = ioctl(fd, PIOCPSINFO, ps) == 0 ? 0 : errno;
        close(fd);
        return r;
}

/* /proc, IRIX's: a file per process, and PIOCPSINFO. */
static void
test_proc(void)
{
        struct prpsinfo ps;
        struct stat st;
        struct dirent *de;
        DIR *dir;
        pid_t pid;
        int status = -1, seen1 = 0, seenpinfo = 0, p[2];
        char c;

        /* The files, their owners and modes. */
        CHECK(stat("/proc/00001", &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & 07777) == 0600 && st.st_uid == 0);
        CHECK(stat("/proc/pinfo/00001", &st) == 0 &&
            (st.st_mode & 07777) == 0444);
        CHECK(stat("/proc/29999", &st) == -1 && errno == ENOENT);
        CHECK((dir = opendir("/proc")) != NULL);
        while (dir != NULL && (de = readdir(dir)) != NULL) {
                seen1 |= strcmp(de->d_name, "00001") == 0;
                seenpinfo |= strcmp(de->d_name, "pinfo") == 0;
        }
        if (dir != NULL)
                closedir(dir);
        CHECK(seen1 && seenpinfo);

        /* init, and this process. */
        CHECK(psinfo(1, &ps) == 0 && ps.pr_pid == 1 && ps.pr_ppid == 0 &&
            strcmp(ps.pr_fname, "init") == 0 && ps.pr_uid == 0 &&
            ps.pr_ttydev == PRNODEV);
        CHECK(psinfo(getpid(), &ps) == 0 && ps.pr_pid == getpid() &&
            ps.pr_ppid == getppid() && ps.pr_sname == 'O' &&
            strncmp(ps.pr_psargs, "/usr/tests/boottest test", 24) == 0 &&
            strcmp(ps.pr_fname, "boottest") == 0 && ps.pr_size > 0 &&
            strcmp(ps.pr_clname, "TS") == 0 && ps.pr_nice == 20 &&
            ps.pr_nlwp == 1 && ps.pr_start[0] > 0);

        /* A stopped child, then a zombie. */
        if ((pid = fork()) == 0) {
                pause();
                _exit(0);
        }
        CHECK(pid > 0 && kill(pid, SIGSTOP) == 0);
        CHECK(waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status));
        CHECK(psinfo(pid, &ps) == 0 && ps.pr_sname == 'T' &&
            ps.pr_ppid == getpid());
        CHECK(kill(pid, SIGKILL) == 0);
        /* Killed but not yet reaped: a zombie. */
        for (int i = 0; i < 100 && psinfo(pid, &ps) == 0 && !ps.pr_zomb; i++)
                usleep(10000);
        CHECK(psinfo(pid, &ps) == 0 && ps.pr_zomb && ps.pr_sname == 'Z');
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(psinfo(pid, &ps) == ENOENT);

        /* Not root: init's own file is closed to it, pinfo is not. */
        CHECK(pipe(p) == 0);
        if ((pid = fork()) == 0) {
                int fd;

                c = 'x';
                if (setuid(100) == 0 &&
                    open("/proc/00001", O_RDONLY) == -1 && errno == EACCES &&
                    (fd = open("/proc/pinfo/00001", O_RDONLY)) >= 0 &&
                    ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_pid == 1)
                        c = 'y';
                (void)write(p[1], &c, 1);
                _exit(0);
        }
        close(p[1]);
        c = 0;
        CHECK(read(p[0], &c, 1) == 1 && c == 'y');
        close(p[0]);
        CHECK(waitpid(pid, &status, 0) == pid);
}

/*
 * The login records, after test_login: init's boot record and the
 * console's getty in utmpx, and the test user's login, which login wrote
 * to both utmpx and wtmpx.
 */
static void
test_utmpx(const char *user)
{
        struct utmpx *ut;
        int boot = 0, console = 0, mine = 0, inwtmp = 0;
        FILE *fp;

        setutxent();
        while ((ut = getutxent()) != NULL) {
                if (ut->ut_type == BOOT_TIME)
                        boot = 1;
                if (ut->ut_type == LOGIN_PROCESS &&
                    strcmp(ut->ut_line, "console") == 0)
                        console = 1;
                if (ut->ut_type == USER_PROCESS &&
                    strcmp(ut->ut_user, user) == 0 &&
                    strncmp(ut->ut_line, "pts/", 4) == 0)
                        mine = 1;
        }
        endutxent();
        CHECK(boot);
        CHECK(console);
        CHECK(mine);

        CHECK((fp = fopen("/var/adm/wtmpx", "r")) != NULL);
        if (fp != NULL) {
                struct utmpx rec;

                while (fread(&rec, sizeof(rec), 1, fp) == 1)
                        if (rec.ut_type == USER_PROCESS &&
                            strcmp(rec.ut_user, user) == 0)
                                inwtmp = 1;
                fclose(fp);
        }
        CHECK(inwtmp);
}

/* The utilities, by usr/tests/utiltest.sh: it must find no failures. */
static void
test_utilities(void)
{
        pid_t pid;
        int status = -1;

        if ((pid = fork()) == 0) {
                execl("/bin/sh", "sh", "/usr/tests/utiltest.sh", (char *)NULL);
                _exit(127);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/*
 * ksenon-utils' own suite (/usr/tests/xn/tests/run.sh, over the 45
 * utilities in /usr/tests/xn/cmd), as on GNU/Linux: it prints each
 * failed check and "N passed, M failed", and must find no failures.
 */
static void
test_xn_suite(void)
{
        pid_t pid;
        int status = -1;

        if ((pid = fork()) == 0) {
                execl("/bin/sh", "sh", "/usr/tests/xn/tests/run.sh",
                    (char *)NULL);
                _exit(127);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/*
 * After passwd changed user's password: /etc/passwd still says "x" for
 * it; /etc/shadow is still root's, mode 0400, with a new hash and today
 * as the day of the change; passwd's copy is gone.
 */
static void
test_shadow(const char *user)
{
        char line[512], want[64];
        struct stat st;
        size_t ulen = strlen(user);
        int seen = 0;
        FILE *f;

        CHECK(stat("/etc/shadow", &st) == 0 && st.st_uid == 0 &&
            (st.st_mode & 07777) == 0400);
        CHECK(stat("/etc/stmp", &st) == -1 && errno == ENOENT);
        CHECK(stat("/etc/.pwd.lock", &st) == 0);
        if ((f = fopen("/etc/passwd", "r")) != NULL) {
                snprintf(want, sizeof(want), "%s:x:", user);
                while (fgets(line, sizeof(line), f) != NULL)
                        seen |= strncmp(line, want, strlen(want)) == 0;
                fclose(f);
        }
        CHECK(seen);
        seen = 0;
        snprintf(want, sizeof(want), ":%ld:", (long)(time(NULL) / 86400));
        if ((f = fopen("/etc/shadow", "r")) != NULL) {
                while (fgets(line, sizeof(line), f) != NULL)
                        if (strncmp(line, user, ulen) == 0 &&
                            line[ulen] == ':' &&
                            strncmp(line + ulen + 1, "$sha1$", 6) == 0 &&
                            strstr(line, "belkatst") == NULL &&
                            strstr(line, want) != NULL)
                                seen = 1;
                fclose(f);
        }
        CHECK(seen);
}

/* Logins through getty and login on a pseudo-terminal. */
static void
test_login(const char *user, const char *password, const char *uid,
    const char *home)
{
        char line[256], want[256];
        pid_t pid;
        int m, status;

        /* A wrong password, then root on a line not marked secure. */
        CHECK((m = start_getty(&pid)) >= 0);
        if (m < 0)
                return;
        CHECK(expect(m, "login: ", 5000));
        snprintf(line, sizeof(line), "%s\n", user);
        say(m, line);
        CHECK(expect(m, "Password:", 5000));
        say(m, "not-the-password\n");
        CHECK(expect(m, "Login incorrect", 10000));
        CHECK(expect(m, "login: ", 5000));
        say(m, "root\n");
        CHECK(expect(m, "Login incorrect", 10000));

        /* The user, rightly. */
        CHECK(expect(m, "login: ", 5000));
        say(m, line);
        CHECK(expect(m, "Password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        CHECK(expect(m, "$ ", 10000));
        say(m, "echo \"<$USER_ID:$(cd && pwd):$USER:$LOGNAME>\"\n");
        snprintf(want, sizeof(want), "<%s:%s:%s:%s>", uid, home, user, user);
        CHECK(expect(m, want, 5000));
        /* who lists the login (utmpx, from login). */
        say(m, "who\n");
        snprintf(want, sizeof(want), "%s ", user);
        CHECK(expect(m, want, 5000));
        /* /etc/shadow is closed to the user. */
        say(m, "cat /etc/shadow\n");
        CHECK(expect(m, "/etc/shadow: Access denied", 5000));
        /* passwd changes the password, giving the old one first. */
        say(m, "passwd\n");
        CHECK(expect(m, "Old password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        CHECK(expect(m, "New password:", 10000));
        say(m, "new-Pass-2026\n");
        CHECK(expect(m, "Retype new password:", 5000));
        say(m, "new-Pass-2026\n");
        CHECK(expect(m, "The password is changed.", 15000));
        say(m, "exit\n");
        status = reap(pid, 10000);
        CHECK(status != -1);
        close(m);

        /* Now the old password fails, and the new one works. */
        CHECK((m = start_getty(&pid)) >= 0);
        if (m < 0)
                return;
        CHECK(expect(m, "login: ", 5000));
        snprintf(line, sizeof(line), "%s\n", user);
        say(m, line);
        CHECK(expect(m, "Password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        CHECK(expect(m, "Login incorrect", 10000));
        CHECK(expect(m, "login: ", 5000));
        snprintf(line, sizeof(line), "%s\n", user);
        say(m, line);
        CHECK(expect(m, "Password:", 5000));
        say(m, "new-Pass-2026\n");
        CHECK(expect(m, "$ ", 10000));
        say(m, "echo \"<$((40 + 2))>\"\n");
        CHECK(expect(m, "<42>", 5000));
        say(m, "exit\n");
        status = reap(pid, 10000);
        CHECK(status != -1);
        close(m);
        test_shadow(user);
}

int
main(int argc, char **argv)
{
        if (argc != 5) {
                fprintf(stderr, "usage: boottest user password uid home\n");
                return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        test_setting();
        test_orphan();
        test_proc();
        test_utilities();
        test_xn_suite();
        test_login(argv[1], argv[2], argv[3], argv[4]);
        test_utmpx(argv[1]);
        printf("boottest: %d failures\n", failures);
        /* Let init start the console's getty, then stop the system. */
        sleep(3);
        execl("/sbin/halt", "halt", (char *)NULL);
        printf("boottest: cannot run /sbin/halt\n");
        return 1;
}
