/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * reboot [-nq], halt [-npq], poweroff [-nq]: stop the system, then start
 * it again (reboot), or not (halt), or turn the power off (poweroff, or
 * halt -p: ACPI's S5, or a halt where that cannot be done). Unless -q, first stop init starting logins
 * (SIGTSTP), ask every process to end (SIGTERM) and give them a moment,
 * and write back the file systems (unless -n); uadmin(A_SHUTDOWN) then
 * kills what is left and leaves the file systems clean. With -q,
 * uadmin(A_REBOOT) stops at once.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SYS_uadmin      55
#define A_REBOOT        1
#define A_SHUTDOWN      2
#define AD_HALT         0
#define AD_BOOT         1
#define AD_POWEROFF     6

/* uadmin through the system call: mlibc has no wrapper. */
static long
uadmin(long cmd, long fcn)
{
        long ret;
        unsigned char failed;

        __asm__ volatile("syscall; setc %1"
            : "=a"(ret), "=r"(failed)
            : "a"((long)SYS_uadmin), "D"(cmd), "S"(fcn), "d"(0L)
            : "rcx", "r11", "memory", "cc");
        if (failed) {
                errno = (int)ret;
                return -1;
        }
        return ret;
}

int
main(int argc, char **argv)
{
        const char *prog = strrchr(argv[0], '/');
        int halt, off, nosync = 0, quick = 0, c;

        prog = prog != NULL ? prog + 1 : argv[0];
        off = strcmp(prog, "poweroff") == 0;
        halt = off || strcmp(prog, "halt") == 0;
        while ((c = getopt(argc, argv, halt ? "npq" : "nq")) != -1) {
                switch (c) {
                case 'p':
                        off = 1;
                        break;
                case 'n':
                        nosync = 1;
                        break;
                case 'q':
                        quick = 1;
                        break;
                default:
                        fprintf(stderr, "usage: %s [-n%sq]\n", prog,
                            strcmp(prog, "halt") == 0 ? "p" : "");
                        return 2;
                }
        }
        if (geteuid() != 0) {
                fprintf(stderr, "%s: must be the superuser\n", prog);
                return 1;
        }
        if (!quick) {
                signal(SIGTERM, SIG_IGN);
                signal(SIGHUP, SIG_IGN);
                (void)kill(1, SIGTSTP);
                (void)kill(-1, SIGTERM);
                sleep(2);
                if (!nosync)
                        sync();
        }
        uadmin(quick ? A_REBOOT : A_SHUTDOWN,
            off ? AD_POWEROFF : halt ? AD_HALT : AD_BOOT);
        fprintf(stderr, "%s: uadmin: %s\n", prog, strerror(errno));
        return 1;
}
