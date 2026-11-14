/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-13
 * SPDX-License-Identifier: MIT
 */

/*
 * The terminal test, built against mlibc: the console's termios modes
 * with the System V values, canonical input and its editing characters,
 * end of file, VMIN and VTIME, the signal characters, output stopped and
 * started, the window size, the old termio calls, and flushing.
 *
 * Input is put on the console with TIOCSTI, as if typed, and read back
 * from descriptor 0; echo stays off so that it does not reach the log.
 * The modes are restored at the end. Run alone: other readers of the
 * console would take the input.
 *
 * Exits with the number of failed checks.
 */

#define _GNU_SOURCE             /* CBAUD, ECHOCTL, TIOC* */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("ttytest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

/* Type s on the console. */
static void
type(const char *s)
{
        for (; *s != '\0'; s++)
                CHECK(ioctl(0, TIOCSTI, s) == 0);
}

/* Read once into buf (NUL-terminated); the count. */
static ssize_t
get(char *buf, size_t size)
{
        ssize_t n = read(0, buf, size - 1);

        buf[n > 0 ? n : 0] = '\0';
        return n;
}

static long
ms_since(const struct timespec *t0)
{
        struct timespec t;

        clock_gettime(CLOCK_MONOTONIC, &t);
        return (t.tv_sec - t0->tv_sec) * 1000 +
            (t.tv_nsec - t0->tv_nsec) / 1000000;
}

static void
set(const struct termios *t)
{
        CHECK(tcsetattr(0, TCSANOW, t) == 0);
}

static volatile int got_sig[NSIG];

static void
on_sig(int sig)
{
        got_sig[sig]++;
}

/* The console's modes as it starts. */
static void
test_defaults(const struct termios *t)
{
        struct winsize ws;

        CHECK(isatty(0) && isatty(1));
        CHECK((t->c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG));
        CHECK((t->c_iflag & ICRNL) != 0);
        CHECK((t->c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR));
        CHECK((t->c_cflag & CSIZE) == CS8);
        CHECK(t->c_cc[VINTR] == CTRL('c') && t->c_cc[VERASE] == 0177 &&
            t->c_cc[VKILL] == CTRL('u') && t->c_cc[VEOF] == CTRL('d'));
        CHECK(cfgetospeed(t) == B38400);
        CHECK(ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row == 24 &&
            ws.ws_col == 80);
        /* Files are not terminals. */
        CHECK(!isatty(open("/p/bin/ttytest", O_RDONLY)) && errno == ENOTTY);
}

/* Lines, editing, and end of file. */
static void
test_canonical(void)
{
        char buf[64];
        int n;

        type("abc\n");
        CHECK(ioctl(0, FIONREAD, &n) == 0 && n == 4);
        CHECK(get(buf, sizeof(buf)) == 4 && strcmp(buf, "abc\n") == 0);

        /* One line per read, though two are waiting. */
        type("one\ntwo\n");
        CHECK(get(buf, sizeof(buf)) == 4 && strcmp(buf, "one\n") == 0);
        CHECK(get(buf, sizeof(buf)) == 4 && strcmp(buf, "two\n") == 0);

        /* A line longer than the read comes in pieces. */
        type("hello\n");
        CHECK(get(buf, 3) == 2 && strcmp(buf, "he") == 0);
        CHECK(get(buf, 3) == 2 && strcmp(buf, "ll") == 0);
        CHECK(get(buf, 3) == 2 && strcmp(buf, "o\n") == 0);

        /* Erase, kill, word erase, and a literal next character. */
        type("ab\177c\n");
        CHECK(get(buf, sizeof(buf)) == 3 && strcmp(buf, "ac\n") == 0);
        type("xyz\025ok\n");
        CHECK(get(buf, sizeof(buf)) == 3 && strcmp(buf, "ok\n") == 0);
        type("one two \027x\n");
        CHECK(get(buf, sizeof(buf)) == 6 && strcmp(buf, "one x\n") == 0);
        type("a\026\025b\n");
        CHECK(get(buf, sizeof(buf)) == 4 && buf[1] == CTRL('u'));

        /* A carriage return is a newline (ICRNL). */
        type("cr\r");
        CHECK(get(buf, sizeof(buf)) == 3 && strcmp(buf, "cr\n") == 0);

        /* EOF ends a line unread; on an empty line it reads as 0. */
        type("pq\004");
        CHECK(get(buf, sizeof(buf)) == 2 && strcmp(buf, "pq") == 0);
        type("\004");
        CHECK(get(buf, sizeof(buf)) == 0);

        /* Not blocking: no line yet. */
        type("part");
        CHECK(fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK) == 0);
        CHECK(read(0, buf, sizeof(buf)) == -1 && errno == EAGAIN);
        CHECK(fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK) == 0);
        CHECK(tcflush(0, TCIFLUSH) == 0);
        type("\n");
        CHECK(get(buf, sizeof(buf)) == 1 && strcmp(buf, "\n") == 0);
}

/* Without ICANON: VMIN and VTIME. */
static void
test_raw(const struct termios *base)
{
        struct termios t = *base;
        struct timespec t0;
        char buf[64];

        /* A line being edited is passed on when ICANON goes off. */
        type("par");
        t.c_lflag &= ~(tcflag_t)ICANON;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        set(&t);
        CHECK(get(buf, sizeof(buf)) == 3 && strcmp(buf, "par") == 0);
        /* VMIN 0, VTIME 0: what there is, maybe nothing. */
        CHECK(get(buf, sizeof(buf)) == 0);
        type("x\n");
        CHECK(get(buf, sizeof(buf)) == 2 && strcmp(buf, "x\n") == 0);

        /* VMIN 3: three characters, though more were asked for. */
        t.c_cc[VMIN] = 3;
        set(&t);
        type("abcd");
        CHECK(get(buf, sizeof(buf)) == 4 && strcmp(buf, "abcd") == 0);
        /* A read of one needs only one; the rest waits for VMIN. */
        type("ef");
        CHECK(get(buf, 2) == 1 && strcmp(buf, "e") == 0);
        type("gh");
        CHECK(get(buf, sizeof(buf)) == 3 && strcmp(buf, "fgh") == 0);

        /* VMIN 0, VTIME 2: up to 0.2 s for anything. */
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 2;
        set(&t);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(get(buf, sizeof(buf)) == 0);
        CHECK(ms_since(&t0) >= 150 && ms_since(&t0) < 2000);
        type("q");
        CHECK(get(buf, sizeof(buf)) == 1);

        /* VMIN 5, VTIME 1: two characters, then 0.1 s of nothing. */
        t.c_cc[VMIN] = 5;
        t.c_cc[VTIME] = 1;
        set(&t);
        type("yz");
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(get(buf, sizeof(buf)) == 2 && strcmp(buf, "yz") == 0);
        CHECK(ms_since(&t0) >= 50 && ms_since(&t0) < 2000);

        /* Characters keep their value: no ICRNL, no editing. */
        t.c_iflag &= ~(tcflag_t)ICRNL;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        set(&t);
        type("\r\177\025");
        CHECK(get(buf, sizeof(buf)) == 3 && buf[0] == '\r' &&
            buf[1] == '\177' && buf[2] == CTRL('u'));
        set(base);
}

/* The signal characters go to the foreground process group. */
static void
test_signals(const struct termios *base)
{
        struct termios t = *base;
        struct sigaction sa;
        char buf[64];

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_sig;
        CHECK(sigaction(SIGINT, &sa, NULL) == 0);
        CHECK(sigaction(SIGQUIT, &sa, NULL) == 0);
        CHECK(sigaction(SIGWINCH, &sa, NULL) == 0);
        CHECK(tcsetpgrp(0, getpgrp()) == 0);
        CHECK(tcgetpgrp(0) == getpgrp());
        CHECK(tcsetpgrp(0, 99999) == -1 && errno == EPERM);

        /* The line so far is flushed. */
        type("lost\003");
        (void)getpid();
        CHECK(got_sig[SIGINT] == 1);
        type("\n");
        CHECK(get(buf, sizeof(buf)) == 1 && strcmp(buf, "\n") == 0);
        type("\034");
        (void)getpid();
        CHECK(got_sig[SIGQUIT] == 1);

        /* NOFLSH keeps it; without ISIG ^C is a character. */
        t.c_lflag |= NOFLSH;
        set(&t);
        type("kept\003\n");
        CHECK(got_sig[SIGINT] == 2);
        CHECK(get(buf, sizeof(buf)) == 5 && strcmp(buf, "kept\n") == 0);
        t.c_lflag &= ~(tcflag_t)ISIG;
        set(&t);
        type("\003\n");
        CHECK(got_sig[SIGINT] == 2);
        CHECK(get(buf, sizeof(buf)) == 2 && buf[0] == CTRL('c'));
        set(base);

        /* A window size change. */
        {
                struct winsize ws = { 30, 100, 0, 0 }, old, now;

                CHECK(ioctl(0, TIOCGWINSZ, &old) == 0);
                CHECK(ioctl(0, TIOCSWINSZ, &ws) == 0);
                (void)getpid();
                CHECK(got_sig[SIGWINCH] == 1);
                CHECK(ioctl(0, TIOCGWINSZ, &now) == 0 && now.ws_row == 30 &&
                    now.ws_col == 100);
                CHECK(ioctl(0, TIOCSWINSZ, &old) == 0);
        }
        /* Only a session leader takes a controlling terminal. */
        if (getsid(0) != getpid())
                CHECK(ioctl(0, TIOCSCTTY, 0) == -1 && errno == EPERM);
}

/* Output stopped and started, the old calls, and flushing. */
static void
test_misc(const struct termios *base)
{
        struct termios t;
        char buf[64];
        int n;
        struct {
                unsigned short  c_iflag, c_oflag, c_cflag, c_lflag;
                char            c_line;
                unsigned char   c_cc[8];
        } old;

        /*
         * tcflow: a write to stopped output would wait. Nothing may be
         * printed until output is started again.
         */
        {
                int fl = fcntl(1, F_GETFL), off, wrote, e;

                off = tcflow(1, TCOOFF);
                (void)fcntl(1, F_SETFL, fl | O_NONBLOCK);
                wrote = (int)write(1, "x", 1);
                e = errno;
                (void)fcntl(1, F_SETFL, fl);
                CHECK(tcflow(1, TCOON) == 0);
                CHECK(off == 0 && wrote == -1 && e == EAGAIN);
        }
        CHECK(tcdrain(1) == 0);

        /* TCGETA and TCSETA: the low 16 bits, and 8 characters. */
        CHECK(ioctl(0, TCGETA, &old) == 0);
        CHECK(old.c_lflag == (unsigned short)base->c_lflag &&
            old.c_iflag == (unsigned short)base->c_iflag &&
            old.c_cc[VKILL] == base->c_cc[VKILL]);
        old.c_cc[VERASE] = CTRL('h');
        CHECK(ioctl(0, TCSETA, &old) == 0);
        CHECK(tcgetattr(0, &t) == 0 && t.c_cc[VERASE] == CTRL('h') &&
            t.c_cc[VSUSP] == base->c_cc[VSUSP]);
        set(base);
        CHECK(tcsetattr(0, 99, base) == -1 && errno == EINVAL);

        /* TCSAFLUSH drops what was typed. */
        type("gone\n");
        CHECK(tcsetattr(0, TCSAFLUSH, base) == 0);
        CHECK(ioctl(0, FIONREAD, &n) == 0 && n == 0);
        type("here\n");
        CHECK(get(buf, sizeof(buf)) == 5);
}

int
main(void)
{
        struct termios orig, quiet;

        setvbuf(stdout, NULL, _IONBF, 0);
        if (tcgetattr(0, &orig) != 0) {
                printf("ttytest: tcgetattr: errno %d\n", errno);
                return 1;
        }
        test_defaults(&orig);
        /* Anything typed before now would be taken for the checks' input. */
        CHECK(tcflush(0, TCIFLUSH) == 0);
        quiet = orig;
        quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ECHONL);
        set(&quiet);
        test_canonical();
        test_raw(&quiet);
        test_signals(&quiet);
        test_misc(&quiet);
        CHECK(tcsetattr(0, TCSAFLUSH, &orig) == 0);
        printf("ttytest: %d failures\n", failures);
        return failures;
}
