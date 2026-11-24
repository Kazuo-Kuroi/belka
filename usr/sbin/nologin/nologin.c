/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * nologin: the shell of an account no one logs in to (/etc/passwd's
 * daemon, nobody, dnscache). It says so, with /etc/nologin.txt's
 * text instead if there is one, and fails.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(void)
{
        static const char msg[] = "This account is currently not available.\n";
        char buf[512];
        ssize_t n;
        int fd;

        if ((fd = open("/etc/nologin.txt", O_RDONLY)) >= 0) {
                while ((n = read(fd, buf, sizeof(buf))) > 0)
                        (void)write(STDOUT_FILENO, buf, (size_t)n);
                close(fd);
        } else {
                (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        }
        return 1;
}
