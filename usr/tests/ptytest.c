/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * The pseudo-terminal and job control test, built against mlibc: devfs
 * (/dev/null, /dev/zero, /dev/pts), a pseudo-terminal pair both ways, a
 * controlling terminal taken by a session leader's open, /dev/tty,
 * SIGTTIN and SIGTTOU for background groups, the signal characters
 * through the master, SIGHUP when the master closes or the session
 * leader exits, and SIGHUP and SIGCONT for an orphaned group with a
 * stopped member.
 *
 * Exits with the number of failed checks.
 */

#define _GNU_SOURCE             /* posix_openpt, cfmakeraw */

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
#include <termios.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("ptytest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

static int
dir_has(const char *dir, const char *name)
{
        DIR *d = opendir(dir);
        struct dirent *de;
        int found = 0;

        if (d == NULL)
                return 0;
        while ((de = readdir(d)) != NULL)
                if (strcmp(de->d_name, name) == 0)
                        found = 1;
        closedir(d);
        return found;
}

/* Read what there is from fd, waiting up to a second for the first. */
static ssize_t
drain(int fd, char *buf, size_t size)
{
        struct pollfd p = { fd, POLLIN, 0 };
        ssize_t n;

        if (poll(&p, 1, 1000) != 1)
                return -1;
        n = read(fd, buf, size - 1);
        buf[n > 0 ? n : 0] = '\0';
        return n;
}

/* Run fn in a child, which exits with its failures; 0 if all went well. */
static int
in_child(void (*fn)(void *), void *arg)
{
        int status;
        pid_t pid;

        fflush(stdout);
        if ((pid = fork()) == 0) {
                failures = 0;
                alarm(20);
                fn(arg);
                fflush(stdout);
                _exit(failures);
        }
        if (pid < 0 || waitpid(pid, &status, 0) != pid)
                return -1;
        if (!WIFEXITED(status)) {
                printf("ptytest: child status %#x\n", status);
                return -1;
        }
        return WEXITSTATUS(status);
}

static void
test_devfs(void)
{
        char buf[16];
        int fd;

        CHECK(dir_has("/dev", "console") && dir_has("/dev", "null") &&
            dir_has("/dev", "ptmx") && dir_has("/dev", "pts") &&
            dir_has("/dev", "tty"));
        CHECK((fd = open("/dev/null", O_RDWR)) >= 0);
        CHECK(write(fd, "gone", 4) == 4 && read(fd, buf, sizeof(buf)) == 0);
        close(fd);
        CHECK((fd = open("/dev/zero", O_RDONLY)) >= 0);
        memset(buf, 1, sizeof(buf));
        CHECK(read(fd, buf, sizeof(buf)) == sizeof(buf) && buf[0] == 0 &&
            buf[15] == 0);
        close(fd);
        CHECK(open("/dev/nosuch", O_RDONLY) == -1 && errno == ENOENT);
        /* The kernel's processes have no controlling terminal. */
        CHECK(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO);
}

/* A pair: both ways, echo, raw mode, the window size. */
static void
test_pair(void)
{
        char buf[64], *name;
        struct termios t;
        struct winsize ws = { 50, 132, 0, 0 }, got;
        struct stat st;
        int m, s, n;

        CHECK((m = posix_openpt(O_RDWR | O_NOCTTY)) >= 0);
        CHECK(grantpt(m) == 0);
        CHECK((name = ptsname(m)) != NULL &&
            strncmp(name, "/dev/pts/", 9) == 0);
        if (name == NULL)
                return;
        name = strdup(name);
        CHECK(dir_has("/dev/pts", name + 9));
        CHECK(stat(name, &st) == 0 && S_ISCHR(st.st_mode) &&
            st.st_uid == getuid() && (st.st_mode & 0777) == 0620);
        /* Locked until unlockpt. */
        CHECK(open(name, O_RDWR | O_NOCTTY) == -1 && errno == EIO);
        CHECK(unlockpt(m) == 0);
        CHECK((s = open(name, O_RDWR | O_NOCTTY)) >= 0);
        CHECK(isatty(s) && ptsname(s) == NULL);
        CHECK(tcgetattr(s, &t) == 0 && (t.c_lflag & ICANON) != 0);

        /* Typed on the master: a line for the slave, echoed back. */
        CHECK(write(m, "hi\n", 3) == 3);
        CHECK(read(s, buf, sizeof(buf)) == 3 && memcmp(buf, "hi\n", 3) == 0);
        CHECK(drain(m, buf, sizeof(buf)) == 4 && strcmp(buf, "hi\r\n") == 0);
        CHECK(ioctl(m, FIONREAD, &n) == 0 && n == 0);

        /* Written by the slave: for the master, with ONLCR. */
        t.c_lflag &= ~(tcflag_t)ECHO;
        CHECK(tcsetattr(s, TCSANOW, &t) == 0);
        CHECK(write(s, "out\n", 4) == 4);
        CHECK(ioctl(m, FIONREAD, &n) == 0 && n == 5);
        CHECK(drain(m, buf, sizeof(buf)) == 5 && strcmp(buf, "out\r\n") == 0);

        /* Raw: every character as it is. */
        cfmakeraw(&t);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        CHECK(tcsetattr(s, TCSANOW, &t) == 0);
        CHECK(write(m, "\003\r", 2) == 2);
        CHECK(read(s, buf, 2) == 2 && buf[0] == 3 && buf[1] == '\r');
        CHECK(write(s, "\n", 1) == 1);
        CHECK(drain(m, buf, sizeof(buf)) == 1 && buf[0] == '\n');

        /* The window size, set through the master. */
        CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0);
        CHECK(ioctl(s, TIOCGWINSZ, &got) == 0 && got.ws_row == 50 &&
            got.ws_col == 132);

        /* With the slave closed, the master reads end of file. */
        close(s);
        CHECK(read(m, buf, sizeof(buf)) == 0);
        close(m);
        /* With the master closed, the slave's name is gone. */
        CHECK(stat(name, &st) == -1 && errno == ENOENT);
        free(name);
}

static volatile int hups, ints;

static void
on_hup(int sig)
{
        (void)sig;
        hups++;
}

static void
on_int(int sig)
{
        (void)sig;
        ints++;
}

/* A new pair, unlocked, with the slave's name in name. */
static int
new_pair(char *name, size_t size)
{
        int m = posix_openpt(O_RDWR | O_NOCTTY);

        if (m < 0 || unlockpt(m) != 0 || ptsname_r(m, name, size) != 0)
                return -1;
        return m;
}

/* Stop status of a background child that touches the terminal. */
static int
background(int s, int what)
{
        int status;
        pid_t pid;

        if ((pid = fork()) == 0) {
                char c;

                setpgid(0, 0);
                if (what == 'r')
                        _exit(read(s, &c, 1) == -1 && errno == EIO ? 40 : 41);
                if (what == 'i') {
                        signal(SIGTTIN, SIG_IGN);
                        _exit(read(s, &c, 1) == -1 && errno == EIO ? 42 : 43);
                }
                _exit(write(s, "w", 1) == 1 ? 44 : 45);
        }
        if (waitpid(pid, &status, WUNTRACED) != pid)
                return -1;
        if (WIFSTOPPED(status)) {
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
        }
        return status;
}

/*
 * In a session of its own: the slave becomes its controlling terminal
 * on open; background reads and writes stop; ^C reaches the foreground;
 * closing the master hangs it up.
 */
static void
session(void *arg)
{
        char name[64], buf[16];
        struct termios t;
        struct sigaction sa;
        int m, s, tty, st;

        (void)arg;
        CHECK(setsid() == getpid());
        CHECK((m = new_pair(name, sizeof(name))) >= 0);
        CHECK((s = open(name, O_RDWR)) >= 0);
        CHECK(tcgetpgrp(s) == getpgrp() && tcgetsid(s) == getpid());
        CHECK((tty = open("/dev/tty", O_RDWR)) >= 0);
        CHECK(isatty(tty) && tcgetpgrp(tty) == getpgrp());
        close(tty);
        /* A second terminal does not become ours. */
        {
                char other[64];
                int m2 = new_pair(other, sizeof(other)), s2 = -1;

                CHECK(m2 >= 0 && (s2 = open(other, O_RDWR)) >= 0);
                CHECK(tcgetsid(s2) == -1 && errno == ENOTTY);
                close(s2);
                close(m2);
        }

        /* Background groups: a read stops with SIGTTIN, or EIO. */
        st = background(s, 'r');
        CHECK(WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTIN);
        st = background(s, 'i');
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);
        /* A write goes through, unless TOSTOP: then SIGTTOU. */
        st = background(s, 'w');
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 44);
        CHECK(tcgetattr(s, &t) == 0);
        t.c_lflag |= TOSTOP;
        t.c_lflag &= ~(tcflag_t)ECHO;
        CHECK(tcsetattr(s, TCSANOW, &t) == 0);
        st = background(s, 'w');
        CHECK(WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTOU);
        (void)drain(m, buf, sizeof(buf));

        /* ^C typed on the master: SIGINT to the foreground group. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_int;
        CHECK(sigaction(SIGINT, &sa, NULL) == 0);
        sa.sa_handler = on_hup;
        CHECK(sigaction(SIGHUP, &sa, NULL) == 0);
        CHECK(write(m, "\003", 1) == 1);
        (void)getpid();
        CHECK(ints == 1);

        /* The master closes: SIGHUP, end of file, EIO; no terminal. */
        close(m);
        (void)getpid();
        CHECK(hups >= 1);
        CHECK(read(s, buf, 1) == 0);
        CHECK(write(s, "x", 1) == -1 && errno == EIO);
        CHECK(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO);
        close(s);
}

/*
 * The session leader exits: its foreground group gets SIGHUP. The
 * leader makes a second group the foreground, and that group's member
 * tells the parent, through the pipe, what came.
 */
struct leader_arg {
        int     pipe[2];
};

static void
leader(void *arg)
{
        struct leader_arg *la = arg;
        char name[64];
        int m, s, ready[2];
        pid_t pid;
        char c;

        CHECK(setsid() == getpid());
        CHECK((m = new_pair(name, sizeof(name))) >= 0);
        CHECK((s = open(name, O_RDWR)) >= 0);
        CHECK(pipe(ready) == 0);
        if ((pid = fork()) == 0) {
                struct sigaction sa;

                memset(&sa, 0, sizeof(sa));
                sa.sa_handler = on_hup;
                sigaction(SIGHUP, &sa, NULL);
                setpgid(0, 0);
                (void)write(ready[1], "r", 1);  /* the handler is in */
                while (hups == 0)
                        pause();
                (void)write(la->pipe[1], "H", 1);
                _exit(0);
        }
        setpgid(pid, pid);
        CHECK(read(ready[0], &c, 1) == 1);
        CHECK(tcsetpgrp(s, pid) == 0);
        /* Exiting now; the master stays open, held by the child. */
}

/*
 * An orphaned group with a stopped member: in a new session, a process
 * in a group of its own makes a child there that stops; when it exits,
 * nothing in the session ties the group, so the stopped child gets
 * SIGHUP and SIGCONT, and tells the parent through the pipe.
 */
static void
orphan(void *arg)
{
        struct leader_arg *la = arg;
        int status;
        pid_t a;

        CHECK(setsid() == getpid());
        if ((a = fork()) == 0) {
                pid_t b;

                setpgid(0, 0);
                if ((b = fork()) == 0) {
                        struct sigaction sa;

                        memset(&sa, 0, sizeof(sa));
                        sa.sa_handler = on_hup;
                        sigaction(SIGHUP, &sa, NULL);
                        kill(getpid(), SIGSTOP);
                        /* Continued: SIGHUP should have come first. */
                        (void)write(la->pipe[1], hups ? "O" : "o", 1);
                        _exit(0);
                }
                /* Exit once b is stopped. */
                (void)waitpid(b, &status, WUNTRACED);
                _exit(WIFSTOPPED(status) ? 0 : 1);
        }
        CHECK(waitpid(a, &status, 0) == a && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0);
}

int
main(void)
{
        struct leader_arg la;
        char c = 0;

        setvbuf(stdout, NULL, _IONBF, 0);
        test_devfs();
        test_pair();
        CHECK(in_child(session, NULL) == 0);

        CHECK(pipe(la.pipe) == 0);
        CHECK(in_child(leader, &la) == 0);
        alarm(10);
        CHECK(read(la.pipe[0], &c, 1) == 1 && c == 'H');
        CHECK(in_child(orphan, &la) == 0);
        CHECK(read(la.pipe[0], &c, 1) == 1 && c == 'O');
        alarm(0);
        close(la.pipe[0]);
        close(la.pipe[1]);

        printf("ptytest: %d failures\n", failures);
        return failures;
}
