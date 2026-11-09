/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * fork: a new process with a copy-on-write copy of the caller's address
 * space, its open files, current directory, and break, and of its LWPs:
 * all of them for fork and forkall (UnixWare's fork is forkall), the
 * caller only for fork1.
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

/* The parts of an LWP's signal state a child's copy inherits. */
static void
fork_copy_lwp(struct proc *p, const struct lwp *src, struct lwp *dst)
{
        int s = spin_lock(&p->p_siglock);

        dst->l_sigmask = src->l_sigmask;
        dst->l_altsp = src->l_altsp;
        dst->l_altsize = src->l_altsize;
        dst->l_altflags = src->l_altflags;
        dst->l_flags = src->l_flags & LW_DETACHED;
        if ((src->l_flags & LW_SUSPREQ) != 0)
                dst->l_flags |= LW_SUSPREQ;
        spin_unlock(&p->p_siglock, s);
}

/*
 * fork1 copies the calling LWP only, which becomes LWP 1 of the child.
 * fork and forkall copy every LWP, keeping their ids: the others are
 * parked first (a rendezvous), so their user registers are complete,
 * and their copies resume where they were; a system call one of them
 * was interrupted in is reissued in both processes.
 */
static int
fork_common(int all, uint64_t *rv)
{
        struct lwp *l = curlwp, *cl, *first;
        struct proc *p = l->l_proc;
        struct proc *cp;
        struct as *cas;
        int rendez = 0, error;

        if (all && p->p_nlwp > 1) {
                if ((error = lwp_rendezvous()) != 0)
                        return error == EINTR ? EINTR : EAGAIN;
                rendez = 1;
        }
        if ((cas = as_dup(p->p_as)) == NULL) {
                if (rendez)
                        lwp_rendezvous_done();
                return EAGAIN;
        }
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

        (void)lwp_alloc(cp, &cl);
        if (all)
                cl->l_lwpid = l->l_lwpid;
        fork_copy_lwp(p, l, cl);
        lwp_newthread(cl, lwp_user_start, NULL);
        md_lwp_fork(l, cl);
        md_set_return(cl, (uint64_t)p->p_pid, 1);
        lwp_join_ts(cl, NULL);
        first = cl;
        if (all) {
                /* The others are parked, so the list holds still. */
                for (struct lwp *o = p->p_lwps; o != NULL; o = o->l_next) {
                        struct lwp *co;

                        if (o == l)
                                continue;
                        (void)lwp_alloc(cp, &co);
                        co->l_lwpid = o->l_lwpid;
                        fork_copy_lwp(p, o, co);
                        lwp_newthread(co, lwp_user_start, NULL);
                        md_lwp_copy(o, co);
                        lwp_join_ts(co, first);
                }
                cp->p_lwpnext = p->p_lwpnext;
        }
        mutex_lock(&proctree_lock);
        cp->p_stat = SRUN;
        mutex_unlock(&proctree_lock);
        rv[0] = (uint64_t)cp->p_pid;
        rv[1] = 0;
        /* A child LWP may exit as soon as it runs: step first. */
        for (struct lwp *c = cp->p_lwps, *next; c != NULL; c = next) {
                next = c->l_next;
                lwp_run(c);
        }
        if (rendez)
                lwp_rendezvous_done();
        return 0;
}

int
sys_fork(const uint64_t *args __unused, uint64_t *rv)
{
        return fork_common(1, rv);
}

int
sys_fork1(const uint64_t *args __unused, uint64_t *rv)
{
        return fork_common(0, rv);
}
