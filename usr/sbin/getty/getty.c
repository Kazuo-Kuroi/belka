/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * getty [type] line: open /dev/line as the controlling terminal of a
 * new session, set sane modes, print a banner and "login: ", read a
 * name, and run login(1) with it. init starts it for each line /etc/ttys
 * turns on, appending the line's name. The type (a gettytab entry on
 * BSD) is accepted and ignored: the line keeps its speed, and TERM
 * comes from the environment, or is "vt100".
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>
#include <utmp.h>

#define LOGIN           "/usr/bin/login"
#define NAMEMAX         64

static void
put(int fd, const char *s)
{
        size_t len = strlen(s);

        while (len > 0) {
                ssize_t n = write(fd, s, len);

                if (n < 0 && errno == EINTR)
                        continue;
                if (n <= 0)
                        return;
                s += n;
                len -= (size_t)n;
        }
}

static void
modes(int fd)
{
        struct termios t;

        if (tcgetattr(fd, &t) != 0)
                return;
        t.c_iflag = BRKINT | ICRNL | IXON | IMAXBEL;
        t.c_oflag = OPOST | ONLCR;
        t.c_cflag = (t.c_cflag & (CBAUD | CSIZE | CLOCAL)) | CREAD | HUPCL;
        if ((t.c_cflag & CSIZE) == 0)
                t.c_cflag |= CS8;
        t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE |
            IEXTEN;
        t.c_cc[VINTR] = 0x03;
        t.c_cc[VQUIT] = 0x1c;
        t.c_cc[VERASE] = 0x7f;
        t.c_cc[VKILL] = 0x15;
        t.c_cc[VEOF] = 0x04;
        t.c_cc[VSUSP] = 0x1a;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        (void)tcsetattr(fd, TCSAFLUSH, &t);
}

/* A login name: up to NAMEMAX - 1 bytes of a line, trimmed; 0 if none. */
static int
getname(int fd, char *name)
{
        char line[256];
        ssize_t n;
        size_t i = 0, len;

        n = read(fd, line, sizeof(line) - 1);
        if (n < 0 && errno == EINTR)
                return 0;
        if (n < 0)
                fprintf(stderr, "getty: read: %s\n", strerror(errno));
        if (n <= 0)
                exit(0);        /* end of file or hang-up: init starts anew */
        line[n] = '\0';
        while (line[i] == ' ' || line[i] == '\t')
                i++;
        len = strcspn(line + i, " \t\r\n");
        if (len == 0 || len >= NAMEMAX || line[i] == '-')
                return 0;
        memcpy(name, line + i, len);
        name[len] = '\0';
        return 1;
}

int
main(int argc, char **argv)
{
        char path[PATH_MAX], name[NAMEMAX], banner[512];
        const char *line;
        struct utsname u;
        int fd;

        if (argc < 2) {
                fprintf(stderr, "usage: getty [type] line\n");
                return 2;
        }
        line = argv[argc - 1];
        if (line[0] == '/')
                snprintf(path, sizeof(path), "%s", line);
        else
                snprintf(path, sizeof(path), "/dev/%s", line);
        signal(SIGHUP, SIG_IGN);
        if ((fd = open(path, O_RDWR | O_NOCTTY)) < 0)
                return 1;
        if (login_tty(fd) != 0)
                return 1;
        signal(SIGHUP, SIG_DFL);
        modes(0);
        if (uname(&u) != 0)
                strcpy(u.nodename, "belka");
        if (getenv("TERM") == NULL)
                setenv("TERM", "vt100", 1);
        snprintf(banner, sizeof(banner), "\r\n%.64s %.64s (%.64s) (%.64s)\r\n\r\n",
            u.sysname, u.release, u.nodename, line);
        put(1, banner);
        for (;;) {
                put(1, "login: ");
                if (getname(0, name))
                        break;
        }
        execl(LOGIN, "login", "--", name, (char *)NULL);
        put(2, "getty: cannot run " LOGIN "\r\n");
        sleep(5);
        return 1;
}
