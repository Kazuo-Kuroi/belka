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
 * wtmpx have the records; and that /etc/rc.d/network has plumbed the
 * network: vionet0 configured as /etc/rc.conf says, the default route,
 * TCP and UDP sockets over the loopback, the gateway answering a ping,
 * and netcfg show; and that /etc/rc.d/dnscache has started dnscache,
 * which resolves names, from a fake root server of the test's, for the
 * C library and djbdns' tools; and that chroot(2) confines a process.
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
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stropts.h>
#include <sys/ioctl.h>
#include <sys/ipconf.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
                /* Moves across file systems: /var/tmp (the root) from /tmp. */
                setenv("XDEVDIR", "/var/tmp", 1);
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
        /*
         * su: the user is not in group wheel, so may not become root; may
         * become test2 with its password, not with a wrong one.
         */
        say(m, "su root -c true\n");
        CHECK(expect(m, "correct secondary group (wheel)", 5000));
        say(m, "su test2 -c \"echo '<'\\$(id -un)'>'\"\n");
        CHECK(expect(m, "Password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        CHECK(expect(m, "<test2>", 10000));
        say(m, "su test2 -c true\n");
        CHECK(expect(m, "Password:", 5000));
        say(m, "not-the-password\n");
        CHECK(expect(m, "Sorry", 10000));
        /* And setting the host name. */
        say(m, "hostname other\n");
        CHECK(expect(m, "Operation not permitted", 5000));
        /* So is mount. */
        say(m, "/sbin/mount /dev/vd1 /mnt\n");
        CHECK(expect(m, "only the superuser may mount", 5000));
        say(m, "/usr/sbin/useradd unauthorized-account\n");
        CHECK(expect(m, "only the superuser", 5000));
        say(m, "/usr/sbin/groupadd unauthorized-group\n");
        CHECK(expect(m, "only the superuser", 5000));
        /* chsh enforces ownership, listed shells, and authentication. */
        struct stat passwd_before;

        CHECK(stat("/etc/passwd", &passwd_before) == 0);
        say(m, "chsh -s /bin/tcsh test2\n");
        CHECK(expect(m, "only the superuser", 5000));
        say(m, "chsh -s /bin/cat\n");
        CHECK(expect(m, "not listed in /etc/shells", 5000));
        say(m, "chsh -s /bin/tcsh\n");
        CHECK(expect(m, "Password:", 5000));
        say(m, "not-the-password\n");
        CHECK(expect(m, "authentication failed", 10000));
        say(m, "chsh -s /bin/tcsh\n");
        CHECK(expect(m, "Password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        CHECK(expect(m, "The shell is changed.", 15000));
        {
                struct passwd *pw = getpwnam(user);

                CHECK(pw != NULL && strcmp(pw->pw_shell, "/bin/tcsh") == 0);
        }
        say(m, "chsh -s /bin/sh\n");
        CHECK(expect(m, "Password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        CHECK(expect(m, "The shell is changed.", 15000));
        {
                struct stat st;
                struct passwd *pw = getpwnam(user);

                CHECK(pw != NULL && strcmp(pw->pw_shell, "/bin/sh") == 0);
                CHECK(stat("/etc/passwd", &st) == 0 &&
                    st.st_uid == 0 && (st.st_mode & 07777) == (passwd_before.st_mode & 07777));
        }
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
        /* login says when the user last logged in, and on which line. */
        CHECK(expect(m, "Last login: ", 10000));
        CHECK(expect(m, " on pts/", 5000));
        CHECK(expect(m, "$ ", 10000));
        say(m, "echo \"<$((40 + 2))>\"\n");
        CHECK(expect(m, "<42>", 5000));
        say(m, "exit\n");
        status = reap(pid, 10000);
        CHECK(status != -1);
        close(m);
        test_shadow(user);
}

/* Log user in with password on a new getty; the master, or -1. */
static int
login_as(const char *user, const char *password, pid_t *pidp)
{
        char line[256];
        int m;

        if ((m = start_getty(pidp)) < 0)
                return -1;
        CHECK(expect(m, "login: ", 5000));
        snprintf(line, sizeof(line), "%s\n", user);
        say(m, line);
        CHECK(expect(m, "Password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        return m;
}

static void
login_done(int m, pid_t pid)
{
        say(m, "exit\n");
        CHECK(reap(pid, 10000) != -1);
        close(m);
}

/*
 * Password ageing (/etc/shadow, login and passwd), on test3, which has
 * the test user's password: a change made due with passwd -f is asked
 * for at login, and the login goes on once it is made; a password near
 * its end is warned of, and may not be changed again within min days; an
 * expired account and a locked one are turned away.
 */
static void
test_ageing(const char *password)
{
        static const char newpw[] = "Aged-Pass-2026";
        char line[64];
        pid_t pid;
        int m;

        CHECK(system("passwd -f test3") == 0);
        if ((m = login_as("test3", password, &pid)) < 0)
                return;
        CHECK(expect(m, "Your password has expired", 10000));
        CHECK(expect(m, "Old password:", 5000));
        snprintf(line, sizeof(line), "%s\n", password);
        say(m, line);
        CHECK(expect(m, "New password:", 10000));
        snprintf(line, sizeof(line), "%s\n", newpw);
        say(m, line);
        CHECK(expect(m, "Retype new password:", 5000));
        say(m, line);
        CHECK(expect(m, "The password is changed.", 15000));
        CHECK(expect(m, "$ ", 10000));
        login_done(m, pid);

        /* Due in 10 days, warned of 20 before; no change within 5. */
        CHECK(system("passwd -n 5 -x 10 -w 20 test3") == 0);
        if ((m = login_as("test3", newpw, &pid)) < 0)
                return;
        CHECK(expect(m, "Warning: your password expires on", 10000));
        CHECK(expect(m, "$ ", 10000));
        say(m, "passwd\n");
        CHECK(expect(m, "changed too recently", 5000));
        login_done(m, pid);

        /* An account that expired in 1970 (the eighth field, day 1). */
        CHECK(system("awk -F: -v OFS=: '$1 == \"test3\" { $8 = 1 } "
            "{ print }' /etc/shadow > /etc/shadow.new && "
            "mv /etc/shadow.new /etc/shadow && chmod 400 /etc/shadow") == 0);
        if ((m = login_as("test3", newpw, &pid)) < 0)
                return;
        CHECK(expect(m, "your account has expired", 10000));
        CHECK(reap(pid, 15000) != -1);
        close(m);

        /* Locked: the right password no longer works. */
        CHECK(system("passwd -l test3") == 0);
        if ((m = login_as("test3", newpw, &pid)) < 0)
                return;
        CHECK(expect(m, "Login incorrect", 10000));
        kill(pid, SIGKILL);
        (void)reap(pid, 5000);
        close(m);
}

static uint32_t
inet4(int a, int b, int c, int d)
{
        return htonl((uint32_t)a << 24 | (uint32_t)b << 16 |
            (uint32_t)c << 8 | (uint32_t)d);
}

static int
ipstr(int fd, int cmd, void *data, int len)
{
        struct strioctl sio = { cmd, 0, len, data };

        return ioctl(fd, I_STR, &sio);
}

static uint16_t
cksum16(const unsigned char *p, size_t len)
{
        uint32_t sum = 0;

        for (; len > 1; p += 2, len -= 2)
                sum += (uint32_t)p[0] << 8 | p[1];
        if (len > 0)
                sum += (uint32_t)p[0] << 8;
        while ((sum >> 16) != 0)
                sum = (sum & 0xffff) + (sum >> 16);
        return (uint16_t)~sum;
}

/* An ICMP echo to dst on a raw stream, and its reply within ms. */
static int
ping(uint32_t dst, int ms)
{
        unsigned char p[64], r[256];
        int fd = open("/dev/ip", O_RDWR), proto = 1, ok = 0;
        long end = now_ms() + ms;
        uint16_t sum;

        if (fd < 0)
                return 0;
        if (ipstr(fd, IP_BINDPROTO, &proto, sizeof(proto)) != 0) {
                close(fd);
                return 0;
        }
        memset(p, 0, sizeof(p));
        p[0] = 0x45;
        p[9] = 1;
        memcpy(p + 16, &dst, 4);
        p[20] = 8;                      /* echo request */
        p[24] = 0x0b;                   /* id */
        p[27] = 1;                      /* sequence */
        sum = cksum16(p + 20, 44);
        p[22] = (unsigned char)(sum >> 8);
        p[23] = (unsigned char)sum;
        if (write(fd, p, sizeof(p)) == (ssize_t)sizeof(p)) {
                while (!ok && now_ms() < end) {
                        struct pollfd pf = { fd, POLLIN, 0 };
                        ssize_t n;

                        if (poll(&pf, 1, (int)(end - now_ms())) != 1)
                                break;
                        n = read(fd, r, sizeof(r));
                        ok = n >= 28 && memcmp(r + 12, &dst, 4) == 0 &&
                            r[(r[0] & 0x0f) * 4] == 0 &&
                            r[(r[0] & 0x0f) * 4 + 4] == 0x0b;
                }
        }
        close(fd);
        return ok;
}

static void
test_network(void)
{
        struct sockaddr_in sin;
        struct ip_ifinfo ifs[8];
        struct ip_route rts[16];
        socklen_t len = sizeof(sin);
        int ipfd, n, found = 0, l, c, a, u, status = -1;
        char buf[8];
        pid_t pid;

        ipfd = open("/dev/ip", O_RDWR);
        CHECK(ipfd >= 0);
        n = ipstr(ipfd, IP_IFLIST, ifs, sizeof(ifs));
        for (int i = 0; i < n && i < 8; i++)
                if (strcmp(ifs[i].ii_name, "vionet0") == 0)
                        found = ifs[i].ii_addr == inet4(10, 0, 2, 15) &&
                            ifs[i].ii_mask == inet4(255, 255, 255, 0) &&
                            (ifs[i].ii_flags & (IPIF_UP | IPIF_RUNNING)) ==
                            (IPIF_UP | IPIF_RUNNING);
        CHECK(found);
        n = ipstr(ipfd, IP_ROUTELIST, rts, sizeof(rts));
        found = 0;
        for (int i = 0; i < n && i < 16; i++)
                if (rts[i].rt_mask == 0 &&
                    rts[i].rt_gateway == inet4(10, 0, 2, 2))
                        found = 1;
        CHECK(found);
        /* The static route of /etc/routed/static_route.conf; no forwarding. */
        found = 0;
        for (int i = 0; i < n && i < 16; i++)
                if (rts[i].rt_dst == inet4(10, 77, 0, 0) &&
                    rts[i].rt_mask == inet4(255, 255, 0, 0) &&
                    rts[i].rt_gateway == inet4(10, 0, 2, 2))
                        found = 1;
        CHECK(found);
        CHECK(ipstr(ipfd, IP_GETFORWARD, NULL, 0) == 0);
        close(ipfd);
        CHECK(ping(inet4(10, 0, 2, 2), 3000));

        /* TCP and UDP sockets, which need the plumbing. */
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        l = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(l >= 0 && bind(l, (struct sockaddr *)&sin, sizeof(sin)) == 0 &&
            listen(l, 1) == 0 &&
            getsockname(l, (struct sockaddr *)&sin, &len) == 0);
        c = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(c >= 0 && connect(c, (struct sockaddr *)&sin, sizeof(sin)) == 0);
        a = accept(l, NULL, NULL);
        CHECK(a >= 0 && write(c, "boot", 4) == 4 &&
            read(a, buf, sizeof(buf)) == 4 && memcmp(buf, "boot", 4) == 0);
        close(a);
        close(c);
        close(l);
        u = socket(AF_INET, SOCK_DGRAM, 0);
        sin.sin_port = 0;
        CHECK(u >= 0 && bind(u, (struct sockaddr *)&sin, sizeof(sin)) == 0 &&
            getsockname(u, (struct sockaddr *)&sin, &len) == 0 &&
            sendto(u, "dgram", 5, 0, (struct sockaddr *)&sin,
            sizeof(sin)) == 5 && recv(u, buf, sizeof(buf), 0) == 5);
        close(u);

        if ((pid = fork()) == 0) {
                int null = open("/dev/null", O_WRONLY);

                if (null >= 0)
                        dup2(null, 1);
                execl("/sbin/netcfg", "netcfg", "show", (char *)NULL);
                _exit(127);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/*
 * The name server dnscache asks, as the test image's servers/@ says: on
 * 127.0.0.53, for ms, it answers for test.belka (10.20.30.40) and its
 * address's name, and that nothing else is there, authoritatively.
 */
static void
fake_root(int fd, int ms)
{
        static const unsigned char name[] = "\4test\5belka";
        static const unsigned char rev[] = "\00240\00230\00220\00210"
            "\7in-addr\4arpa";
        long end = now_ms() + ms;
        unsigned char q[512], r[512];

        while (now_ms() < end) {
                struct pollfd pf = { fd, POLLIN, 0 };
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                ssize_t n;
                size_t qend, rl;
                int type, found = 0;

                if (poll(&pf, 1, (int)(end - now_ms())) != 1)
                        continue;
                n = recvfrom(fd, q, sizeof(q), 0, (struct sockaddr *)&from,
                    &fl);
                if (n < 12 + 5)
                        continue;
                for (qend = 12; qend < (size_t)n && q[qend] != 0;
                    qend += q[qend] + 1)
                        continue;
                if (qend + 5 > (size_t)n)
                        continue;
                type = q[qend + 1] << 8 | q[qend + 2];
                qend += 5;
                memcpy(r, q, qend);
                r[2] = 0x84;            /* a reply, authoritative */
                r[3] = 0;
                r[6] = r[7] = r[8] = r[9] = r[10] = r[11] = 0;
                rl = qend;
                if (type == 1 && qend - 12 - 4 == sizeof(name) &&
                    strncasecmp((char *)q + 12, (char *)name,
                    sizeof(name)) == 0) {
                        static const unsigned char a[] = { 0xc0, 12, 0, 1,
                            0, 1, 0, 0, 0, 60, 0, 4, 10, 20, 30, 40 };

                        memcpy(r + rl, a, sizeof(a));
                        rl += sizeof(a);
                        found = 1;
                } else if (type == 12 && qend - 12 - 4 == sizeof(rev) &&
                    strncasecmp((char *)q + 12, (char *)rev,
                    sizeof(rev)) == 0) {
                        static const unsigned char p[] = { 0xc0, 12, 0, 12,
                            0, 1, 0, 0, 0, 60, 0, sizeof(name) };

                        memcpy(r + rl, p, sizeof(p));
                        memcpy(r + rl + sizeof(p), name, sizeof(name));
                        rl += sizeof(p) + sizeof(name);
                        found = 1;
                }
                if (found)
                        r[7] = 1;
                else
                        r[3] = 3;       /* NXDOMAIN */
                (void)sendto(fd, r, rl, 0, (struct sockaddr *)&from, fl);
        }
}

/*
 * The output of argv[0] with argv, in envp (NULL: this environment),
 * into out (size bytes, terminated), and whether it exited 0.
 */
static int
run_outv(char *const argv[], char *const envp[], char *out, size_t size)
{
        int p[2], status = -1;
        ssize_t n, got = 0;
        pid_t pid;

        if (pipe(p) != 0)
                return 0;
        if ((pid = fork()) == 0) {
                dup2(p[1], 1);
                close(p[0]);
                close(p[1]);
                if (envp != NULL)
                        execve(argv[0], argv, envp);
                else
                        execv(argv[0], argv);
                _exit(127);
        }
        close(p[1]);
        while (got < (ssize_t)size - 1 &&
            (n = read(p[0], out + got, size - 1 - (size_t)got)) > 0)
                got += n;
        out[got] = '\0';
        close(p[0]);
        return pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* prog with arg and arg2 (NULL: none). */
static int
run_out2(const char *prog, const char *arg, const char *arg2, char *out,
    size_t size)
{
        char *argv[] = { (char *)prog, (char *)arg, (char *)arg2, NULL };

        return run_outv(argv, NULL, out, size);
}

static int
run_out(const char *prog, const char *arg, char *out, size_t size)
{
        return run_out2(prog, arg, NULL, out, size);
}

/*
 * Names, through dnscache (/etc/rc.d/dnscache) on 127.0.0.1: the C
 * library's getaddrinfo and getnameinfo, and djbdns' dnsip and dnsname,
 * for a name and an address the fake root server knows, a name it does
 * not, and localhost, which dnscache knows itself; and /dev/urandom,
 * which seeds it.
 */
static void
test_dns(void)
{
        struct sockaddr_in sin;
        struct addrinfo hints, *ai = NULL;
        unsigned char a[32], b[32];
        char host[64], out[128];
        int fd, rfd, ok = 0;
        pid_t pid;

        rfd = open("/dev/urandom", O_RDONLY);
        CHECK(rfd >= 0 && read(rfd, a, sizeof(a)) == sizeof(a) &&
            read(rfd, b, sizeof(b)) == sizeof(b) &&
            memcmp(a, b, sizeof(a)) != 0);
        if (rfd >= 0)
                close(rfd);

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(53);
        sin.sin_addr.s_addr = inet4(127, 0, 0, 53);
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&sin,
            sizeof(sin)) == 0);
        if ((pid = fork()) == 0) {
                fake_root(fd, 20000);
                _exit(0);
        }
        close(fd);

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        /* dnscache may still be starting: a few tries. */
        for (int i = 0; i < 10 && !ok; i++) {
                int e = getaddrinfo("test.belka", NULL, &hints, &ai);

                ok = e == 0 && ai != NULL && ai->ai_family == AF_INET &&
                    ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr ==
                    inet4(10, 20, 30, 40);
                if (e == 0)
                        freeaddrinfo(ai);
                if (!ok)
                        sleep(1);
        }
        CHECK(ok);
        CHECK(getaddrinfo("nothere.belka", NULL, &hints, &ai) == EAI_NONAME);
        CHECK(getaddrinfo("localhost.", NULL, &hints, &ai) == 0 &&
            ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr ==
            inet4(127, 0, 0, 1));
        sin.sin_port = 0;
        sin.sin_addr.s_addr = inet4(10, 20, 30, 40);
        CHECK(getnameinfo((struct sockaddr *)&sin, sizeof(sin), host,
            sizeof(host), NULL, 0, NI_NAMEREQD) == 0 &&
            strcmp(host, "test.belka") == 0);
        CHECK(run_out("/usr/bin/dnsip", "test.belka", out, sizeof(out)) &&
            strcmp(out, "10.20.30.40 \n") == 0);
        CHECK(run_out("/usr/bin/dnsname", "10.20.30.40", out,
            sizeof(out)) && strcmp(out, "test.belka\n") == 0);
        if (pid > 0) {
                kill(pid, SIGKILL);
                (void)reap(pid, 5000);
        }
}

/* Whether the file holds exactly what (at most 255 bytes). */
static int
file_is(const char *path, const char *what)
{
        char buf[256];
        ssize_t n = -1;
        int fd;

        if ((fd = open(path, O_RDONLY)) >= 0) {
                n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
        }
        if (n < 0)
                return 0;
        buf[n] = '\0';
        return strcmp(buf, what) == 0;
}

/* vionet0's address (0: none), and whether a default route is by gw. */
static in_addr_t
vionet0_addr(int ipfd, in_addr_t gw, int *routed)
{
        struct ip_ifinfo ifs[8];
        struct ip_route rts[16];
        in_addr_t addr = 0;
        int n;

        n = ipstr(ipfd, IP_IFLIST, ifs, sizeof(ifs));
        for (int i = 0; i < n && i < 8; i++)
                if (strcmp(ifs[i].ii_name, "vionet0") == 0)
                        addr = ifs[i].ii_addr;
        *routed = 0;
        n = ipstr(ipfd, IP_ROUTELIST, rts, sizeof(rts));
        for (int i = 0; i < n && i < 16; i++)
                if (rts[i].rt_dst == 0 && rts[i].rt_mask == 0 &&
                    rts[i].rt_gateway == gw)
                        *routed = 1;
        return addr;
}

/*
 * The lease (/etc/rc.d/dhcpcd, from QEMU's DHCP server; test_network
 * found its address and route): dhcpcd is running and has kept it; its
 * control socket dumps it (-U); a release (-k) takes the address and
 * every route by vionet0 away (IP_IFADDR with address 0), and a rebind
 * (-n) brings them back.
 */
static void
test_dhcp(void)
{
        char out[2048], buf[16];
        struct stat st;
        int ipfd, fd, routed, ok = 0;
        ssize_t n;
        pid_t pid = 0;

        CHECK((fd = open("/var/run/dhcpcd/pid", O_RDONLY)) >= 0);
        if (fd >= 0) {
                if ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
                        buf[n] = '\0';
                        pid = (pid_t)strtol(buf, NULL, 10);
                }
                close(fd);
        }
        CHECK(pid > 1 && kill(pid, 0) == 0);
        CHECK(stat("/var/db/dhcpcd/vionet0.lease", &st) == 0 &&
            st.st_size > 0);
        CHECK(stat("/var/db/dhcpcd/duid", &st) == 0);
        CHECK(run_out2("/sbin/dhcpcd", "-U", "vionet0", out, sizeof(out)) &&
            strstr(out, "ip_address=10.0.2.15") != NULL &&
            strstr(out, "routers=10.0.2.2") != NULL);

        ipfd = open("/dev/ip", O_RDWR);
        CHECK(ipfd >= 0);
        CHECK(file_is("/var/db/dhcpcd/vionet0.dns", "10.0.2.3\n"));

        /* netcfg takes the address away, and puts it back. */
        {
                char *none[] = { "/sbin/netcfg", "addr", "vionet0", "none",
                    NULL };
                char *addr[] = { "/sbin/netcfg", "addr", "vionet0",
                    "10.0.2.15/24", NULL };
                char *route[] = { "/sbin/netcfg", "route", "add", "default",
                    "10.0.2.2", NULL };

                CHECK(run_outv(none, NULL, out, sizeof(out)) &&
                    vionet0_addr(ipfd, inet4(10, 0, 2, 2), &routed) == 0 &&
                    !routed);
                CHECK(run_outv(addr, NULL, out, sizeof(out)) &&
                    run_outv(route, NULL, out, sizeof(out)) &&
                    vionet0_addr(ipfd, inet4(10, 0, 2, 2), &routed) ==
                    inet4(10, 0, 2, 15) && routed);
        }
        CHECK(run_out2("/sbin/dhcpcd", "-k", "vionet0", out, sizeof(out)));
        for (int i = 0; i < 50 && !ok; i++) {
                ok = vionet0_addr(ipfd, inet4(10, 0, 2, 2), &routed) == 0 &&
                    !routed;
                if (!ok)
                        usleep(100000);
        }
        CHECK(ok);
        CHECK(stat("/var/db/dhcpcd/vionet0.dns", &st) == -1 &&
            errno == ENOENT);
        CHECK(run_out2("/sbin/dhcpcd", "-n", "vionet0", out, sizeof(out)));
        ok = 0;
        for (int i = 0; i < 150 && !ok; i++) {
                ok = vionet0_addr(ipfd, inet4(10, 0, 2, 2), &routed) ==
                    inet4(10, 0, 2, 15) && routed;
                if (!ok)
                        usleep(100000);
        }
        CHECK(ok);
        CHECK(ok && ping(inet4(10, 0, 2, 2), 3000));
        CHECK(file_is("/var/db/dhcpcd/vionet0.dns", "10.0.2.3\n"));
        if (ipfd >= 0)
                close(ipfd);
}

/*
 * SO_PEERCRED on local stream sockets: a pair gives its maker; across
 * a connection, each end gives the other's process and user, the
 * client the listener's from listen() and the server the client's
 * from connect(); a socket not connected has none.
 */
static void
test_peercred(void)
{
        struct sockaddr_un sun = { .sun_family = AF_UNIX };
        struct ucred uc;
        socklen_t len = sizeof(uc);
        int sv[2], l, a, status = -1;
        pid_t pid;

        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &uc, &len) == 0 &&
            len == sizeof(uc) && uc.pid == getpid() && uc.uid == geteuid() &&
            uc.gid == getegid());
        close(sv[0]);
        close(sv[1]);

        snprintf(sun.sun_path, sizeof(sun.sun_path), "/tmp/peercred-%d",
            (int)getpid());
        unlink(sun.sun_path);
        l = socket(AF_UNIX, SOCK_STREAM, 0);
        len = sizeof(uc);
        CHECK(l >= 0 && getsockopt(l, SOL_SOCKET, SO_PEERCRED, &uc,
            &len) == -1 && errno == ENOTCONN);
        CHECK(bind(l, (struct sockaddr *)&sun, sizeof(sun)) == 0 &&
            chmod(sun.sun_path, 0666) == 0 && listen(l, 1) == 0);
        if ((pid = fork()) == 0) {
                int c = -1, bad;

                if (setgid(100) != 0 || setuid(100) != 0 ||
                    (c = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
                    connect(c, (struct sockaddr *)&sun, sizeof(sun)) != 0)
                        _exit(1);
                len = sizeof(uc);
                bad = getsockopt(c, SOL_SOCKET, SO_PEERCRED, &uc, &len) != 0 ||
                    uc.pid != getppid() || uc.uid != 0 || uc.gid != 0;
                (void)write(c, "x", 1);
                _exit(bad ? 2 : 0);
        }
        a = accept(l, NULL, NULL);
        len = sizeof(uc);
        CHECK(a >= 0 && getsockopt(a, SOL_SOCKET, SO_PEERCRED, &uc,
            &len) == 0 && uc.pid == pid && uc.uid == 100 && uc.gid == 100);
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (a >= 0)
                close(a);
        if (l >= 0)
                close(l);
        unlink(sun.sun_path);
}

/*
 * dnscache forwarding to the name servers a lease offers: with
 * dnscache_forwarders="DHCP", dhcpcd's hook, told of a lease that names
 * 127.0.0.54, starts dnscache again forwarding there (servers/@), and
 * names resolve through a fake server on 127.0.0.54 alone (the root,
 * 127.0.0.53, is not there now). Then everything is put back: the
 * setting taken out, the lease's own server (10.0.2.3) told to the
 * hook again, and dnscache resolving from the roots.
 */
static void
test_dns_forward(void)
{
        static const char conf[] = "\ndnscache_forwarders=\"DHCP\"\n";
        char *bound[] = { "reason=BOUND", "interface=vionet0",
            "new_domain_name_servers=127.0.0.54", NULL };
        char *again[] = { "reason=RENEW", "interface=vionet0",
            "new_domain_name_servers=10.0.2.3", NULL };
        char *hook[] = { "/bin/sh", "/libexec/dhcpcd-run-hooks", NULL };
        char *restart[] = { "/bin/sh", "/etc/rc.d/dnscache", "restart",
            NULL };
        struct addrinfo hints, *ai = NULL;
        struct sockaddr_in sin;
        char out[512], saved[4096];
        ssize_t len = -1;
        int fd, ok = 0;
        pid_t pid;

        if ((fd = open("/etc/rc.conf", O_RDWR | O_APPEND)) >= 0) {
                len = pread(fd, saved, sizeof(saved), 0);
                CHECK(len > 0 && len < (ssize_t)sizeof(saved) &&
                    write(fd, conf, sizeof(conf) - 1) ==
                    (ssize_t)sizeof(conf) - 1);
                close(fd);
        }
        CHECK(fd >= 0);

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(53);
        sin.sin_addr.s_addr = inet4(127, 0, 0, 54);
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&sin,
            sizeof(sin)) == 0);
        if ((pid = fork()) == 0) {
                fake_root(fd, 20000);
                _exit(0);
        }
        close(fd);

        CHECK(run_outv(hook, bound, out, sizeof(out)));
        CHECK(file_is("/var/db/dhcpcd/vionet0.dns", "127.0.0.54\n"));
        CHECK(file_is("/etc/dnscache/root/servers/@", "127.0.0.54\n"));
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        for (int i = 0; i < 10 && !ok; i++) {
                int e = getaddrinfo("test.belka", NULL, &hints, &ai);

                ok = e == 0 && ai != NULL &&
                    ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr ==
                    inet4(10, 20, 30, 40);
                if (e == 0)
                        freeaddrinfo(ai);
                if (!ok)
                        sleep(1);
        }
        CHECK(ok);
        if (pid > 0) {
                kill(pid, SIGKILL);
                (void)reap(pid, 5000);
        }

        if (len > 0 && (fd = open("/etc/rc.conf", O_WRONLY | O_TRUNC)) >= 0) {
                CHECK(write(fd, saved, (size_t)len) == len);
                close(fd);
        }
        CHECK(run_outv(hook, again, out, sizeof(out)));
        CHECK(file_is("/var/db/dhcpcd/vionet0.dns", "10.0.2.3\n"));
        CHECK(run_outv(restart, NULL, out, sizeof(out)));
        CHECK(file_is("/etc/dnscache/root/servers/@", "127.0.0.53\n"));
}

/*
 * chroot(2), in a child: refused to an ordinary user; for root, "/" is
 * then the new root, ".." stops there, and what is outside is gone.
 * getentropy(), which needs no /dev, still works there: 256 bytes and
 * no more, different each time.
 */
static void
test_chroot(void)
{
        int status = -1;
        pid_t pid;

        if ((pid = fork()) == 0) {
                static unsigned char a[GETENTROPY_MAX], b[GETENTROPY_MAX];
                struct stat st;
                int bad = 0;

                if (chdir("/etc/dnscache/root") != 0 || chroot(".") != 0)
                        _exit(1);
                bad |= stat("/servers/@", &st) != 0;
                bad |= stat("/../../servers/@", &st) != 0;
                bad |= stat("/etc/passwd", &st) == 0 || errno != ENOENT;
                bad |= chdir("..") != 0 || stat("ip/127", &st) != 0;
                bad |= getentropy(a, sizeof(a)) != 0 ||
                    getentropy(b, sizeof(b)) != 0 ||
                    memcmp(a, b, sizeof(a)) == 0;
                bad |= getentropy(a, GETENTROPY_MAX + 1) != -1 ||
                    errno != EINVAL;
                bad |= getentropy((void *)8, 16) != -1 || errno != EFAULT;
                bad |= getentropy(a, 0) != 0;
                if (setuid(100) != 0 || chroot("/ip") == 0 || errno != EPERM)
                        bad = 1;
                _exit(bad ? 2 : 0);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
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
        test_network();
        test_dhcp();
        test_peercred();
        test_dns();
        test_dns_forward();
        test_chroot();
        test_login(argv[1], argv[2], argv[3], argv[4]);
        test_utmpx(argv[1]);
        test_ageing(argv[2]);
        printf("boottest: %d failures\n", failures);
        /* Let init start the console's getty, then stop the system. */
        sleep(3);
        execl("/sbin/halt", "halt", (char *)NULL);
        printf("boottest: cannot run /sbin/halt\n");
        return 1;
}
