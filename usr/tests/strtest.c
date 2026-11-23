/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * The stream head, from user mode, through /dev/strloop (the loopback
 * driver) and the test modules strpass and strup: getmsg and putmsg,
 * getpmsg and putpmsg with bands, the read modes and options, I_PEEK,
 * I_NREAD, I_FLUSH, I_STR and its timeout, pushing and popping, and
 * what a driver's M_HANGUP, M_ERROR and M_PCSIG do at the head.
 * (I_SETSIG and flow control are libctest's.) Exits with the number of
 * failed checks.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

/* strloop's ioctls, and strup's (sys/kern/str_loop.c, str_mods.c). */
#define LOOP_ECHO       (('L' << 8) | 1)
#define LOOP_HANGUP     (('L' << 8) | 2)
#define LOOP_ERROR      (('L' << 8) | 3)
#define LOOP_SIG        (('L' << 8) | 5)
#define LOOP_SILENT     (('L' << 8) | 6)
#define LOOP_MARK       (('L' << 8) | 7)
#define STRUP_COUNT     (('U' << 8) | 1)

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("strtest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

static int
loop(int flags)
{
        int fd = open("/dev/strloop", O_RDWR | flags);

        CHECK(fd >= 0);
        return fd;
}

static int
put(int fd, const char *ctl, const char *data, int band, int flags)
{
        struct strbuf c = { 0, ctl != NULL ? (int)strlen(ctl) : -1,
            (char *)ctl };
        struct strbuf d = { 0, data != NULL ? (int)strlen(data) : -1,
            (char *)data };

        return putpmsg(fd, &c, &d, band, flags);
}

static int
istr(int fd, int cmd, void *data, int len, int timeout)
{
        struct strioctl sio = { cmd, timeout, len, data };

        return ioctl(fd, I_STR, &sio);
}

/* Wait (at most two seconds) for something to read. */
static int
ready(int fd)
{
        struct pollfd p = { fd, POLLIN, 0 };

        return poll(&p, 1, 2000) == 1;
}

static void
test_getmsg(void)
{
        char cbuf[16], dbuf[16];
        struct strbuf c = { sizeof(cbuf), 0, cbuf };
        struct strbuf d = { sizeof(dbuf), 0, dbuf };
        int fd = loop(O_NONBLOCK), flags, band;

        if (fd < 0)
                return;
        CHECK(isastream(fd) == 1);

        CHECK(put(fd, "ctl", "data", 0, MSG_BAND) == 0);
        flags = 0;
        CHECK(getmsg(fd, &c, &d, &flags) == 0 && flags == 0 &&
            c.len == 3 && memcmp(cbuf, "ctl", 3) == 0 &&
            d.len == 4 && memcmp(dbuf, "data", 4) == 0);

        /* High priority: control only; it goes ahead of what waits. */
        CHECK(put(fd, NULL, "normal", 0, MSG_BAND) == 0);
        CHECK(put(fd, "hi", NULL, 0, MSG_HIPRI) == 0);
        CHECK(put(fd, NULL, "x", 0, MSG_HIPRI) == -1 && errno == EINVAL);
        flags = RS_HIPRI;
        CHECK(getmsg(fd, &c, &d, &flags) == 0 && flags == RS_HIPRI &&
            c.len == 2 && d.len == -1);
        flags = RS_HIPRI;
        CHECK(getmsg(fd, &c, &d, &flags) == -1 && errno == EAGAIN);
        flags = 0;
        CHECK(getmsg(fd, &c, &d, &flags) == 0 && flags == 0 &&
            c.len == -1 && d.len == 6);

        /* What does not fit stays at the head. */
        CHECK(put(fd, "12345", "abcdef", 0, MSG_BAND) == 0);
        c.maxlen = 2;
        d.maxlen = 3;
        flags = 0;
        CHECK(getmsg(fd, &c, &d, &flags) == (MORECTL | MOREDATA) &&
            c.len == 2 && d.len == 3);
        c.maxlen = sizeof(cbuf);
        d.maxlen = -1;                  /* leave the data */
        CHECK(getmsg(fd, &c, &d, &flags) == MOREDATA && c.len == 3 &&
            memcmp(cbuf, "345", 3) == 0);
        d.maxlen = sizeof(dbuf);
        CHECK(getmsg(fd, &c, &d, &flags) == 0 && c.len == -1 &&
            d.len == 3 && memcmp(dbuf, "def", 3) == 0);
        CHECK(getmsg(fd, &c, &d, &flags) == -1 && errno == EAGAIN);
        flags = 7;
        CHECK(getmsg(fd, &c, &d, &flags) == -1 && errno == EINVAL);

        /* Bands: the highest first; MSG_BAND waits for one high enough. */
        CHECK(put(fd, NULL, "b2", 2, MSG_BAND) == 0);
        CHECK(put(fd, NULL, "b0", 0, MSG_BAND) == 0);
        CHECK(put(fd, NULL, "b5", 5, MSG_BAND) == 0);
        CHECK(ioctl(fd, I_CKBAND, 2) == 1 && ioctl(fd, I_CKBAND, 3) == 0);
        CHECK(ioctl(fd, I_GETBAND, &band) == 0 && band == 5);
        {
                struct pollfd p = { fd, POLLIN | POLLRDNORM | POLLRDBAND,
                    0 };

                CHECK(poll(&p, 1, 0) == 1 &&
                    p.revents == (POLLIN | POLLRDNORM | POLLRDBAND));
        }
        band = 0;
        flags = MSG_ANY;
        CHECK(getpmsg(fd, &c, &d, &band, &flags) == 0 && band == 5 &&
            flags == MSG_BAND && memcmp(dbuf, "b5", 2) == 0);
        band = 1;
        flags = MSG_BAND;
        CHECK(getpmsg(fd, &c, &d, &band, &flags) == 0 && band == 2 &&
            memcmp(dbuf, "b2", 2) == 0);
        band = 1;
        flags = MSG_BAND;
        CHECK(getpmsg(fd, &c, &d, &band, &flags) == -1 && errno == EAGAIN);
        band = 0;
        flags = MSG_ANY;
        CHECK(getpmsg(fd, &c, &d, &band, &flags) == 0 && band == 0 &&
            memcmp(dbuf, "b0", 2) == 0);
        CHECK(put(fd, NULL, "x", 256, MSG_BAND) == -1 && errno == EINVAL);
        CHECK(put(fd, "x", NULL, 1, MSG_HIPRI) == -1 && errno == EINVAL);

        /* Neither part: no message. */
        CHECK(putmsg(fd, NULL, NULL, 0) == 0);
        CHECK(ioctl(fd, I_NREAD, &band) == 0);
        close(fd);

        fd = open("/dev/null", O_RDWR);
        CHECK(isastream(fd) == 0);
        flags = 0;
        CHECK(getmsg(fd, &c, &d, &flags) == -1 && errno == ENOSTR);
        CHECK(putmsg(fd, &c, &d, 0) == -1 && errno == ENOSTR);
        close(fd);
}

static void
test_read_modes(void)
{
        char buf[16];
        int fd = loop(O_NONBLOCK), opt, n;

        if (fd < 0)
                return;
        CHECK(ioctl(fd, I_GRDOPT, &opt) == 0 && opt == (RNORM | RPROTNORM));

        /* Byte stream: a read goes on across messages. */
        CHECK(write(fd, "abc", 3) == 3 && write(fd, "def", 3) == 3);
        CHECK(ioctl(fd, I_NREAD, &n) == 2 && n == 3);
        CHECK(read(fd, buf, sizeof(buf)) == 6 && memcmp(buf, "abcdef", 6) == 0);

        /* Message non-discard: one message; the rest stays. */
        CHECK(ioctl(fd, I_SRDOPT, RMSGN) == 0);
        CHECK(write(fd, "abc", 3) == 3 && write(fd, "def", 3) == 3);
        CHECK(read(fd, buf, sizeof(buf)) == 3 && memcmp(buf, "abc", 3) == 0);
        CHECK(read(fd, buf, 2) == 2 && memcmp(buf, "de", 2) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == 1 && buf[0] == 'f');

        /* Message discard: one message; the rest is thrown away. */
        CHECK(ioctl(fd, I_SRDOPT, RMSGD) == 0);
        CHECK(write(fd, "ghijk", 5) == 5);
        CHECK(read(fd, buf, 2) == 2 && memcmp(buf, "gh", 2) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == -1 && errno == EAGAIN);
        CHECK(ioctl(fd, I_SRDOPT, RMODEMASK) == -1 && errno == EINVAL);

        /* Protocol messages: an error, or discarded, or read as data. */
        CHECK(ioctl(fd, I_SRDOPT, RNORM) == 0);
        CHECK(put(fd, "C", "D", 0, MSG_BAND) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == -1 && errno == EBADMSG);
        CHECK(ioctl(fd, I_SRDOPT, RNORM | RPROTDIS) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == 1 && buf[0] == 'D');
        CHECK(ioctl(fd, I_SRDOPT, RNORM | RPROTDAT) == 0);
        CHECK(put(fd, "C", "D", 0, MSG_BAND) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == 2 && memcmp(buf, "CD", 2) == 0);
        CHECK(ioctl(fd, I_GRDOPT, &opt) == 0 && opt == (RNORM | RPROTDAT));
        CHECK(ioctl(fd, I_SRDOPT, RNORM) == 0);

        /* Zero-length writes are sent only with SNDZERO; they read as 0. */
        CHECK(write(fd, "", 0) == 0);
        CHECK(ioctl(fd, I_NREAD, &n) == 0);
        CHECK(ioctl(fd, I_SWROPT, SNDZERO) == 0);
        CHECK(ioctl(fd, I_GWROPT, &opt) == 0 && opt == SNDZERO);
        CHECK(write(fd, "", 0) == 0);
        CHECK(ioctl(fd, I_NREAD, &n) == 1 && n == 0);
        CHECK(write(fd, "z", 1) == 1);
        CHECK(read(fd, buf, sizeof(buf)) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == 1 && buf[0] == 'z');
        CHECK(ioctl(fd, I_SWROPT, 8) == -1 && errno == EINVAL);

        /* I_PEEK leaves the message; I_FLUSH takes everything. */
        CHECK(put(fd, "pc", "pd", 0, MSG_BAND) == 0);
        {
                char cb[4], db[4];
                struct strpeek pk = { { sizeof(cb), 0, cb },
                    { sizeof(db), 0, db }, 0 };

                CHECK(ioctl(fd, I_PEEK, &pk) == 1 && pk.ctlbuf.len == 2 &&
                    pk.databuf.len == 2 && memcmp(cb, "pc", 2) == 0 &&
                    memcmp(db, "pd", 2) == 0 && pk.flags == 0);
                pk.flags = RS_HIPRI;
                CHECK(ioctl(fd, I_PEEK, &pk) == 0);
        }
        CHECK(ioctl(fd, I_NREAD, &n) == 1 && n == 2);
        CHECK(write(fd, "more", 4) == 4);
        CHECK(ioctl(fd, I_FLUSH, FLUSHRW) == 0);
        CHECK(ioctl(fd, I_NREAD, &n) == 0 && n == 0);
        CHECK(ioctl(fd, I_FLUSH, 0) == -1 && errno == EINVAL);

        /* The mark. */
        CHECK(istr(fd, LOOP_MARK, NULL, 0, 0) == 0);
        CHECK(write(fd, "m1", 2) == 2 && write(fd, "m2", 2) == 2);
        CHECK(ioctl(fd, I_ATMARK, ANYMARK) == 1);
        CHECK(ioctl(fd, I_ATMARK, LASTMARK) == 1);
        CHECK(ioctl(fd, I_SRDOPT, RMSGN) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == 2);
        CHECK(ioctl(fd, I_ATMARK, ANYMARK) == 0);
        CHECK(ioctl(fd, I_ATMARK, 4) == -1 && errno == EINVAL);
        CHECK(read(fd, buf, sizeof(buf)) == 2);

        CHECK(ioctl(fd, I_CANPUT, 0) == 1);
        CHECK(ioctl(fd, I_SETCLTIME, 500) == 0);
        CHECK(ioctl(fd, I_GETCLTIME, &n) == 0 && n == 500);
        CHECK(ioctl(fd, I_SETCLTIME, 0) == -1 && errno == EINVAL);
        close(fd);
}

static void
test_ioctl(void)
{
        char buf[16] = "ping";
        struct strioctl sio = { LOOP_ECHO, 0, 4, buf };
        struct timespec t0, t1;
        int fd = loop(0);

        if (fd < 0)
                return;
        CHECK(ioctl(fd, I_STR, &sio) == 4 && sio.ic_len == 4 &&
            memcmp(buf, "ping", 4) == 0);
        CHECK(istr(fd, ('L' << 8) | 99, NULL, 0, 0) == -1 && errno == EINVAL);
        CHECK(istr(fd, LOOP_ECHO, buf, -1, 0) == -1 && errno == EINVAL);
        CHECK(istr(fd, LOOP_ECHO, NULL, 4, 0) == -1 && errno == EFAULT);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(istr(fd, LOOP_SILENT, NULL, 0, 1) == -1 && errno == ETIME);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        CHECK(t1.tv_sec - t0.tv_sec >= 0 && t1.tv_sec - t0.tv_sec <= 3);
        /* The stream still works after a timed-out ioctl. */
        sio.ic_len = 4;
        CHECK(ioctl(fd, I_STR, &sio) == 4);
        close(fd);
}

static void
test_push(void)
{
        char buf[16], name[FMNAMESZ + 1];
        int fd = loop(0), count, i;

        if (fd < 0)
                return;
        CHECK(ioctl(fd, I_LOOK, name) == -1 && errno == EINVAL);
        CHECK(ioctl(fd, I_POP, 0) == -1 && errno == EINVAL);
        CHECK(ioctl(fd, I_PUSH, "nosuch") == -1 && errno == EINVAL);
        CHECK(ioctl(fd, I_FIND, "nosuch") == -1 && errno == EINVAL);

        CHECK(ioctl(fd, I_PUSH, "strup") == 0);
        CHECK(ioctl(fd, I_LOOK, name) == 0 && strcmp(name, "strup") == 0);
        CHECK(ioctl(fd, I_FIND, "strup") == 1);
        CHECK(ioctl(fd, I_FIND, "strpass") == 0);
        CHECK(write(fd, "hello", 5) == 5);
        CHECK(ready(fd) && read(fd, buf, sizeof(buf)) == 5 &&
            memcmp(buf, "HELLO", 5) == 0);
        /* strup answers its ioctl; strloop's pass through it. */
        count = 0;
        CHECK(istr(fd, STRUP_COUNT, &count, sizeof(count), 0) == 0 &&
            count == 1);
        CHECK(istr(fd, LOOP_ECHO, buf, 2, 0) == 2);

        CHECK(ioctl(fd, I_PUSH, "strpass") == 0);
        CHECK(ioctl(fd, I_LOOK, name) == 0 && strcmp(name, "strpass") == 0);
        CHECK(write(fd, "abc", 3) == 3);
        CHECK(ready(fd) && read(fd, buf, sizeof(buf)) == 3 &&
            memcmp(buf, "ABC", 3) == 0);
        CHECK(ioctl(fd, I_POP, 0) == 0 && ioctl(fd, I_POP, 0) == 0);
        CHECK(ioctl(fd, I_LOOK, name) == -1 && errno == EINVAL);
        CHECK(write(fd, "xyz", 3) == 3);
        CHECK(read(fd, buf, sizeof(buf)) == 3 && memcmp(buf, "xyz", 3) == 0);

        /* At most nine modules. */
        for (i = 0; i < 9; i++)
                CHECK(ioctl(fd, I_PUSH, i % 2 ? "strup" : "strpass") == 0);
        CHECK(ioctl(fd, I_PUSH, "strpass") == -1 && errno == EINVAL);
        CHECK(write(fd, "deep", 4) == 4);
        CHECK(ready(fd) && read(fd, buf, sizeof(buf)) == 4 &&
            memcmp(buf, "DEEP", 4) == 0);
        close(fd);                      /* pops them all */

        /* Another stream, with strup's shared syncq, at the same time. */
        fd = loop(0);
        if (fd >= 0) {
                int fd2 = loop(0);

                CHECK(ioctl(fd, I_PUSH, "strup") == 0);
                if (fd2 >= 0) {
                        CHECK(ioctl(fd2, I_PUSH, "strup") == 0);
                        CHECK(write(fd2, "two", 3) == 3);
                        CHECK(write(fd, "one", 3) == 3);
                        CHECK(ready(fd2) && read(fd2, buf, 3) == 3 &&
                            memcmp(buf, "TWO", 3) == 0);
                        close(fd2);
                }
                CHECK(ready(fd) && read(fd, buf, 3) == 3 &&
                    memcmp(buf, "ONE", 3) == 0);
                close(fd);
        }
}

static void
test_driver_messages(void)
{
        char buf[8];
        unsigned char err = EIO, sig = SIGPOLL;
        struct pollfd p;
        sigset_t set, old;
        siginfo_t info;
        const struct timespec now = { 0, 0 };
        int fd = loop(O_NONBLOCK);

        if (fd < 0)
                return;
        /* Hang-up: what is there is read, then end of file. */
        CHECK(write(fd, "left", 4) == 4);
        CHECK(istr(fd, LOOP_HANGUP, NULL, 0, 0) == 0);
        CHECK(write(fd, "x", 1) == -1 && errno == ENXIO);
        p = (struct pollfd){ fd, POLLIN | POLLOUT, 0 };
        CHECK(poll(&p, 1, 0) == 1 && p.revents == (POLLIN | POLLHUP));
        CHECK(read(fd, buf, sizeof(buf)) == 4);
        CHECK(read(fd, buf, sizeof(buf)) == 0);
        CHECK(ioctl(fd, I_PUSH, "strpass") == -1 && errno == ENXIO);
        close(fd);

        /* An error: reads and writes fail with it. */
        if ((fd = loop(O_NONBLOCK)) < 0)
                return;
        CHECK(istr(fd, LOOP_ERROR, &err, 1, 0) == 0);
        CHECK(read(fd, buf, sizeof(buf)) == -1 && errno == EIO);
        CHECK(write(fd, "x", 1) == -1 && errno == EIO);
        CHECK(put(fd, "c", NULL, 0, MSG_BAND) == -1 && errno == EIO);
        p = (struct pollfd){ fd, POLLIN, 0 };
        CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLERR) != 0);
        CHECK(istr(fd, LOOP_ECHO, buf, 1, 0) == -1 && errno == EIO);
        close(fd);

        /* M_PCSIG with SIGPOLL: S_MSG. */
        if ((fd = loop(O_NONBLOCK)) < 0)
                return;
        sigemptyset(&set);
        sigaddset(&set, SIGPOLL);
        CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0);
        CHECK(ioctl(fd, I_SETSIG, S_MSG) == 0);
        CHECK(istr(fd, LOOP_SIG, &sig, 1, 0) == 0);
        CHECK(sigtimedwait(&set, &info, &now) == SIGPOLL &&
            info.si_code == POLL_MSG);
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
        close(fd);
}

/* A blocked reader is woken by a writer on another descriptor. */
static void
test_blocking(void)
{
        char buf[8];
        int fd = loop(0), status;
        pid_t child;

        if (fd < 0)
                return;
        child = fork();
        if (child == 0) {
                usleep(100000);
                _exit(write(fd, "late", 4) == 4 ? 0 : 1);
        }
        CHECK(read(fd, buf, sizeof(buf)) == 4 && memcmp(buf, "late", 4) == 0);
        CHECK(child > 0 && waitpid(child, &status, 0) == child &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(fd);
}

int
main(void)
{
        test_getmsg();
        test_read_modes();
        test_ioctl();
        test_push();
        test_driver_messages();
        test_blocking();
        printf("strtest: %d failures\n", failures);
        return failures;
}
