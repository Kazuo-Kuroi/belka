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
 * mounted; that an orphan goes to init; and, on a pseudo-terminal, that
 * getty and login turn away a wrong password, refuse root on a line
 * /etc/ttys does not mark secure, and log the test user in with the
 * right password, to a shell with the user's IDs, home and name. Then
 * it halts the system with /sbin/halt, after giving init time to start
 * the console's getty.
 *
 * It prints "boottest: N failures"; the test run looks for 0.
 *
 * usage: boottest user password uid home
 */

#define _GNU_SOURCE             /* posix_openpt */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
        say(m, "exit\n");
        status = reap(pid, 10000);
        CHECK(status != -1);
        close(m);
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
        test_login(argv[1], argv[2], argv[3], argv[4]);
        printf("boottest: %d failures\n", failures);
        /* Let init start the console's getty, then stop the system. */
        sleep(3);
        execl("/sbin/halt", "halt", (char *)NULL);
        printf("boottest: cannot run /sbin/halt\n");
        return 1;
}
