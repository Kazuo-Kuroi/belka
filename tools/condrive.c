/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * A host tool: drive a guest's serial console, which QEMU serves on a
 * Unix socket, through a script, as a person at the terminal would.
 * Everything the guest writes is copied to the log. Script lines:
 *
 *   expect TEXT [MS]   wait (MS milliseconds, 10000 by default) until
 *                      TEXT has been written since the last match
 *   send TEXT          write TEXT; \r, \n, \t, \\, \e and \xHH work
 *   sleep MS           wait, still reading
 *   # ...              a comment; blank lines are skipped
 *
 * TEXT runs to the end of the line, but for a trailing " MS" on expect;
 * write a trailing space as \x20. Exits 0 when the script is done, 1
 * when an expect times out or the guest goes away first.
 *
 * usage: condrive socket script logfile
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int con = -1;
static FILE *logf;
static char seen[65536];
static size_t nseen;
static int gone;

static long
now_ms(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Read what the guest wrote, for up to ms; 0 if the console closed. */
static int
pump(long ms)
{
        struct pollfd pfd = { con, POLLIN, 0 };
        char buf[4096];
        ssize_t n;

        if (gone)
                return 0;
        if (poll(&pfd, 1, (int)(ms < 0 ? 0 : ms)) <= 0)
                return 1;
        if ((n = read(con, buf, sizeof(buf))) <= 0) {
                gone = 1;
                return 0;
        }
        fwrite(buf, 1, (size_t)n, logf);
        fflush(logf);
        if (nseen + (size_t)n >= sizeof(seen)) {
                /* Keep the newer half: a match may straddle the cut. */
                size_t keep = sizeof(seen) / 2;

                memmove(seen, seen + nseen - keep, keep);
                nseen = keep;
        }
        memcpy(seen + nseen, buf, (size_t)n);
        nseen += (size_t)n;
        return 1;
}

/* Find want in what was seen since the last match; drop up to it. */
static int
match(const char *want, size_t wlen)
{
        for (size_t i = 0; wlen <= nseen && i <= nseen - wlen; i++) {
                if (memcmp(seen + i, want, wlen) == 0) {
                        memmove(seen, seen + i + wlen, nseen - i - wlen);
                        nseen -= i + wlen;
                        return 1;
                }
        }
        return 0;
}

/* Decode the escapes in s, in place; the length. */
static size_t
unescape(char *s)
{
        char *o = s;

        for (char *p = s; *p != '\0'; p++) {
                if (*p != '\\' || p[1] == '\0') {
                        *o++ = *p;
                        continue;
                }
                switch (*++p) {
                case 'r':
                        *o++ = '\r';
                        break;
                case 'n':
                        *o++ = '\n';
                        break;
                case 't':
                        *o++ = '\t';
                        break;
                case 'e':
                        *o++ = '\033';
                        break;
                case 'x': {
                        char hex[3] = { 0, 0, 0 };

                        if (p[1] != '\0' && p[2] != '\0') {
                                hex[0] = p[1];
                                hex[1] = p[2];
                                p += 2;
                        }
                        *o++ = (char)strtol(hex, NULL, 16);
                        break;
                }
                default:
                        *o++ = *p;
                        break;
                }
        }
        return (size_t)(o - s);
}

int
main(int argc, char **argv)
{
        struct sockaddr_un sa;
        char line[1024];
        FILE *script;
        int lineno = 0;

        if (argc != 4) {
                fprintf(stderr, "usage: condrive socket script logfile\n");
                return 2;
        }
        if ((script = fopen(argv[2], "r")) == NULL ||
            (logf = fopen(argv[3], "w")) == NULL) {
                perror("condrive");
                return 2;
        }
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1]);
        /* QEMU makes the socket as it starts: try for a while. */
        for (int tries = 0;; tries++) {
                if ((con = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
                        perror("condrive: socket");
                        return 2;
                }
                if (connect(con, (struct sockaddr *)&sa, sizeof(sa)) == 0)
                        break;
                close(con);
                if (tries == 100) {
                        perror("condrive: connect");
                        return 2;
                }
                nanosleep(&(struct timespec){ 0, 100000000 }, NULL);
        }

        while (fgets(line, sizeof(line), script) != NULL) {
                char *arg;
                size_t len;

                lineno++;
                line[strcspn(line, "\n")] = '\0';
                if (line[0] == '\0' || line[0] == '#')
                        continue;
                arg = strchr(line, ' ');
                arg = arg != NULL ? arg + 1 : line + strlen(line);
                if (strncmp(line, "expect ", 7) == 0) {
                        long ms = 10000, end;
                        char *sp = strrchr(arg, ' ');

                        if (sp != NULL && sp[1] >= '0' && sp[1] <= '9' &&
                            strspn(sp + 1, "0123456789") == strlen(sp + 1)) {
                                ms = atol(sp + 1);
                                *sp = '\0';
                        }
                        len = unescape(arg);
                        end = now_ms() + ms;
                        while (!match(arg, len)) {
                                if (now_ms() >= end || !pump(end - now_ms())) {
                                        fprintf(stderr, "condrive: line %d: "
                                            "\"%s\" not seen%s\n", lineno,
                                            arg, gone ? " (console closed)" :
                                            "");
                                        return 1;
                                }
                        }
                } else if (strncmp(line, "send ", 5) == 0) {
                        len = unescape(arg);
                        /* A key at a time, as typed. */
                        for (size_t i = 0; i < len; i++) {
                                if (write(con, arg + i, 1) != 1) {
                                        fprintf(stderr, "condrive: line %d: "
                                            "console closed\n", lineno);
                                        return 1;
                                }
                                (void)pump(5);
                        }
                } else if (strncmp(line, "sleep ", 6) == 0) {
                        long end = now_ms() + atol(arg);

                        while (now_ms() < end && pump(end - now_ms()))
                                continue;
                } else {
                        fprintf(stderr, "condrive: line %d: what is \"%s\"?\n",
                            lineno, line);
                        return 2;
                }
        }
        /* Copy the rest, until the guest stops. */
        while (pump(2000))
                continue;
        return 0;
}
