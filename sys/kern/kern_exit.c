/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * exit and the wait family.
 *
 * waitsys() is the System V call; the old wait() returns the process id
 * and a status word in the two return registers. Only exited children
 * can be waited for until there are signals (stopped and continued
 * children need job control).
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/synch.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/wait.h>

int
sys_exit(const uint64_t *args, uint64_t *rv __unused)
{
        proc_exit(CLD_EXITED, (int)(args[0] & 0xff));
}

/*
 * Wait for a child matching idtype and id to exit, or, as options ask,
 * to stop or continue. Returns 0 with its process id (0 if WNOHANG and
 * nothing has happened yet), code, and status. A signal ends the wait
 * with EINTR, or ERESTART if its handler asked for restarts; a stop and
 * continue of the waiter just goes back to waiting.
 */
static int
wait_common(int idtype, pid_t id, int options, pid_t *pidp, int *codep,
    int *datap)
{
        struct proc *p = curproc;

        if (idtype != P_PID && idtype != P_PGID && idtype != P_ALL)
                return EINVAL;
        if ((options & (WEXITED | WTRAPPED | WSTOPPED | WCONTINUED)) == 0 ||
            (options & ~(WEXITED | WTRAPPED | WSTOPPED | WCONTINUED |
            WNOHANG | WNOWAIT)) != 0)
                return EINVAL;
        mutex_lock(&proctree_lock);
        for (;;) {
                int found = 0;

                for (struct proc *c = p->p_children; c != NULL;
                    c = c->p_sibling) {
                        if ((idtype == P_PID && c->p_pid != id) ||
                            (idtype == P_PGID && c->p_pgid != id))
                                continue;
                        found = 1;
                        if (c->p_stat == SZOMB && (options & WEXITED) != 0) {
                                *pidp = c->p_pid;
                                *codep = c->p_wcode;
                                *datap = c->p_wdata;
                                if ((options & WNOWAIT) != 0)
                                        mutex_unlock(&proctree_lock);
                                else
                                        proc_reap_locked(c);
                                return 0;
                        }
                        if (c->p_stat == SZOMB)
                                continue;
                        if (c->p_wstop && (options & WSTOPPED) != 0) {
                                *pidp = c->p_pid;
                                *codep = CLD_STOPPED;
                                *datap = c->p_stopsig;
                                if ((options & WNOWAIT) == 0)
                                        c->p_wstop = 0;
                                mutex_unlock(&proctree_lock);
                                return 0;
                        }
                        if (c->p_wcont && (options & WCONTINUED) != 0) {
                                *pidp = c->p_pid;
                                *codep = CLD_CONTINUED;
                                *datap = SIGCONT;
                                if ((options & WNOWAIT) == 0)
                                        c->p_wcont = 0;
                                mutex_unlock(&proctree_lock);
                                return 0;
                        }
                }
                if (!found) {
                        mutex_unlock(&proctree_lock);
                        return ECHILD;
                }
                if ((options & WNOHANG) != 0) {
                        mutex_unlock(&proctree_lock);
                        *pidp = 0;
                        return 0;
                }
                /*
                 * A signal ends the wait only once the children have been
                 * looked at again: the SIGCHLD of the very child awaited
                 * must not hide its exit.
                 */
                if (curthread->kt_sigpend) {
                        mutex_unlock(&proctree_lock);
                        if (sig_sleep_check())
                                return sig_restart(EINTR);
                        mutex_lock(&proctree_lock);
                        continue;
                }
                (void)sv_wait_mutex_sig(&p->p_waitsv, &proctree_lock);
                mutex_lock(&proctree_lock);
        }
}

/* wait(): the process id, and the old status word. */
int
sys_wait(const uint64_t *args __unused, uint64_t *rv)
{
        pid_t pid;
        int code, data, error;

        if ((error = wait_common(P_ALL, 0, WEXITED | WTRAPPED, &pid, &code,
            &data)) != 0)
                return error;
        rv[0] = (uint64_t)pid;
        rv[1] = code == CLD_EXITED ? (uint64_t)(data & 0xff) << 8 :
            (uint64_t)(data & 0x7f);
        return 0;
}

/* waitsys(idtype, id, siginfo_t *, options) */
int
sys_waitsys(const uint64_t *args, uint64_t *rv __unused)
{
        siginfo_t si;
        pid_t pid;
        int code = 0, data = 0, error;

        if ((error = wait_common((int)args[0], (pid_t)args[1], (int)args[3],
            &pid, &code, &data)) != 0)
                return error;
        memset(&si, 0, sizeof(si));
        if (pid != 0) {
                si.si_signo = SIGCHLD;
                si.si_code = code;
                si.si_pid = pid;
                si.si_status = data;
        }
        if (args[2] != 0)
                error = copyout(&si, (void *)(uintptr_t)args[2], sizeof(si));
        return error;
}
