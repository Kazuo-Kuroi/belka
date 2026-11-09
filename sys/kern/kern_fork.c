/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * fork: a new process with a copy-on-write copy of the caller's address
 * space, its open files, current directory, and break. With one LWP per
 * process, fork (all LWPs) and fork1 (the caller only) are the same.
 *
 * As in System V, the parent gets the child's process id and 0, the
 * child the parent's id and 1, in the two return registers; the C
 * library turns that into the familiar results.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/spinlock.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <machine/md_var.h>

#include <vm/as.h>

void lwp_fork_return(void *);

int
sys_fork(const uint64_t *args __unused, uint64_t *rv)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        struct proc *cp;
        struct as *cas;
        struct lwp *cl;

        if ((cas = as_dup(p->p_as)) == NULL)
                return EAGAIN;
        cp = proc_alloc(p);
        cp->p_as = cas;
        fd_dup_table(p, cp);
        mutex_lock(&p->p_lock);
        cp->p_cdir = p->p_cdir;
        if (cp->p_cdir != NULL)
                VN_HOLD(cp->p_cdir);
        cp->p_brkbase = p->p_brkbase;
        cp->p_brksize = p->p_brksize;
        memcpy(cp->p_comm, p->p_comm, PSCOMSIZ);
        mutex_unlock(&p->p_lock);

        cl = lwp_alloc(cp);
        {
                int s = spin_lock(&p->p_siglock);

                /* The mask and alternate stack; nothing pending. */
                cl->l_sigmask = l->l_sigmask;
                cl->l_altsp = l->l_altsp;
                cl->l_altsize = l->l_altsize;
                cl->l_altflags = l->l_altflags;
                spin_unlock(&p->p_siglock, s);
        }
        lwp_start_stopped(cl, lwp_fork_return, NULL);
        md_lwp_fork(l, cl);
        md_set_return(cl, (uint64_t)p->p_pid, 1);
        mutex_lock(&proctree_lock);
        cp->p_stat = SRUN;
        mutex_unlock(&proctree_lock);
        rv[0] = (uint64_t)cp->p_pid;
        rv[1] = 0;
        lwp_run_new(cl);
        return 0;
}
