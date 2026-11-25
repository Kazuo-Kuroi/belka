/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * uadmin(cmd, fcn, mdep), UnixWare's 55: the superuser stops the
 * system. fcn is AD_HALT, AD_BOOT, AD_IBOOT or AD_POWEROFF.
 *
 *   A_REBOOT    at once, with nothing written back
 *   A_SHUTDOWN  every other process but init gets SIGKILL and a few
 *               seconds to go, the file systems are written back and
 *               left clean (vfs_shutdown), and then the same
 *
 * The caller and init are left as they are: neither runs again.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/uadmin.h>
#include <sys/vfs.h>

#define SHUTDOWN_WAIT   (5 * HZ)        /* for the killed to exit */

/* Is p one A_SHUTDOWN kills (and waits for)? */
static int
shutdown_victim(const struct proc *p)
{
        return p != curproc && p != initproc &&
            (p->p_flags & P_SYSTEM) == 0 && p->p_stat != SZOMB;
}

struct kill_state {
        int     ks_sig;
        int     ks_left;
};

static void
shutdown_one(struct proc *p, void *arg)
{
        struct kill_state *ks = arg;

        if (!shutdown_victim(p))
                return;
        if (ks->ks_sig != 0)
                psignal(p, ks->ks_sig);
        ks->ks_left++;
}

/* Send sig (if not 0) to the other processes; how many are left. */
static int
shutdown_kill(int sig)
{
        struct kill_state ks = { sig, 0 };

        mutex_lock(&proctree_lock);
        proc_iterate_locked(shutdown_one, &ks);
        mutex_unlock(&proctree_lock);
        return ks.ks_left;
}

int
sys_uadmin(const uint64_t *args, uint64_t *rv __unused)
{
        int cmd = (int)args[0], fcn = (int)args[1];

        if (!CRED_ISROOT(CRED()))
                return EPERM;
        if (cmd != A_REBOOT && cmd != A_SHUTDOWN)
                return EINVAL;
        if (fcn != AD_HALT && fcn != AD_BOOT && fcn != AD_IBOOT &&
            fcn != AD_POWEROFF)
                return EINVAL;
        if (cmd == A_SHUTDOWN) {
                printf("uadmin: shutting down\n");
                (void)shutdown_kill(SIGKILL);
                for (int t = 0; t < SHUTDOWN_WAIT; t += HZ / 10) {
                        if (shutdown_kill(0) == 0)
                                break;
                        delay(HZ / 10);
                }
                vfs_shutdown();
        }
        mdboot(fcn, 0);
        return 0;
}
