/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-12
 * SPDX-License-Identifier: MIT
 */

/*
 * poll(fds, nfds, timeout), UnixWare's 87, pollsys, Belka's own (poll
 * with a timespec and a signal mask), and the poll heads behind them.
 *
 * Each pass asks every descriptor's vnode for its events (VOP_POLL),
 * which also hands back the object's poll head. A pass that finds
 * nothing registers the caller on every head, then asks once more: an
 * event between the first look and the registration shows up in that
 * second look, and any later one in pollwakeup(), which marks every
 * registered poller fired and wakes it. The caller sleeps until fired,
 * a signal (EINTR; poll is never restarted after a handler), or the
 * timeout, unregisters, and starts over.
 *
 * A poller's state lives on its stack: it is reachable only through
 * the heads it is registered on, and it unregisters from all of them
 * before returning. The files stay held for the whole call, so no head
 * goes away under a registration.
 *
 * Locking: ph_lock, then ps_lock, then the sync variable's own lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/vnode.h>

struct pollstate {
        struct spinlock ps_lock;
        struct sv       ps_sv;
        int             ps_fired;       /* a head woke it, ps_lock */
};

struct polldat {
        struct polldat  *pd_next;       /* on pd_head, its ph_lock */
        struct pollhead *pd_head;       /* NULL when not registered */
        struct pollstate *pd_ps;
};

void
pollhead_init(struct pollhead *ph)
{
        spinlock_init(&ph->ph_lock, "pollhead");
        ph->ph_list = NULL;
}

/* Something changed on the object of ph: wake everyone polling it. */
void
pollwakeup(struct pollhead *ph)
{
        int s = spin_lock(&ph->ph_lock);

        for (struct polldat *pd = ph->ph_list; pd != NULL; pd = pd->pd_next) {
                struct pollstate *ps = pd->pd_ps;

                (void)spin_lock(&ps->ps_lock);
                ps->ps_fired = 1;
                spin_unlock(&ps->ps_lock, 0);
                sv_broadcast(&ps->ps_sv);
        }
        spin_unlock(&ph->ph_lock, s);
}

static void
poll_register(struct polldat *pd, struct pollhead *ph)
{
        int s = spin_lock(&ph->ph_lock);

        pd->pd_head = ph;
        pd->pd_next = ph->ph_list;
        ph->ph_list = pd;
        spin_unlock(&ph->ph_lock, s);
}

static void
poll_unregister(struct polldat *pd)
{
        struct pollhead *ph = pd->pd_head;
        struct polldat **link;
        int s;

        if (ph == NULL)
                return;
        s = spin_lock(&ph->ph_lock);
        for (link = &ph->ph_list; *link != pd; link = &(*link)->pd_next)
                continue;
        *link = pd->pd_next;
        spin_unlock(&ph->ph_lock, s);
        pd->pd_head = NULL;
}

/*
 * VOP_POLL for objects that never wait, such as regular files and
 * directories: always readable and writable, with no poll head.
 */
int
fs_poll(struct vnode *vp __unused, short events, short *reventsp,
    struct pollhead **phpp, struct cred *cr __unused)
{
        *reventsp = events & (POLLIN | POLLRDNORM | POLLOUT);
        *phpp = NULL;
        return 0;
}

/*
 * One look at every descriptor: fill in revents and return how many
 * have any. With reg, register on each head not registered yet.
 */
static int
poll_scan(struct pollfd *fds, struct file **fps, struct polldat *pds,
    size_t nfds, int reg)
{
        int n = 0;

        for (size_t i = 0; i < nfds; i++) {
                struct pollhead *ph = NULL;
                short rev = 0;

                fds[i].revents = 0;
                if (fds[i].fd < 0)
                        continue;
                if (fps[i] == NULL) {
                        rev = POLLNVAL;
                } else {
                        if (VOP_POLL(fps[i]->f_vnode, fds[i].events, &rev,
                            &ph, kcred) != 0)
                                rev = POLLERR;
                        /* An end open only one way sees only that way. */
                        if ((fps[i]->f_flag & FREAD) == 0)
                                rev &= ~(POLL_READ | POLLHUP);
                        if ((fps[i]->f_flag & FWRITE) == 0)
                                rev &= ~(POLL_WRITE | POLLERR);
                        rev &= fds[i].events | POLLERR | POLLHUP;
                }
                if (rev != 0) {
                        fds[i].revents = rev;
                        n++;
                } else if (reg && ph != NULL && pds[i].pd_head == NULL) {
                        poll_register(&pds[i], ph);
                }
        }
        return n;
}

/*
 * The poll itself: wait up to ticks ticks (forever if negative, not at
 * all if zero) for an event on one of the nfds descriptors at ufds.
 */
static int
do_poll(uint64_t uaddr, size_t nfds, long ticks, uint64_t *rv)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        struct pollfd *ufds = (struct pollfd *)(uintptr_t)uaddr;
        struct pollfd *fds = NULL;
        struct file **fps = NULL;
        struct polldat *pds = NULL;
        struct pollstate ps;
        int64_t id = 0;
        int timing = 0, error = 0, n = 0, s, r;

        if (nfds > NOFILE_MAX)
                return EINVAL;
        if (nfds != 0) {
                fds = kmem_alloc(nfds * sizeof(*fds), KM_SLEEP);
                fps = kmem_zalloc(nfds * sizeof(*fps), KM_SLEEP);
                pds = kmem_zalloc(nfds * sizeof(*pds), KM_SLEEP);
                if ((error = copyin(ufds, fds, nfds * sizeof(*fds))) != 0)
                        goto out;
        }
        spinlock_init(&ps.ps_lock, "pollstate");
        sv_init(&ps.ps_sv, "poll");
        for (size_t i = 0; i < nfds; i++) {
                pds[i].pd_ps = &ps;
                if (fds[i].fd >= 0)
                        fps[i] = fd_get(p, fds[i].fd);
        }
        for (;;) {
                s = spin_lock(&ps.ps_lock);
                ps.ps_fired = 0;
                spin_unlock(&ps.ps_lock, s);
                n = poll_scan(fds, fps, pds, nfds, ticks != 0);
                if (n == 0 && ticks != 0)
                        n = poll_scan(fds, fps, pds, nfds, 0);
                if (n != 0 || ticks == 0 || (timing && l->l_timedout))
                        break;
                if (!timing) {
                        id = lwp_timeout_start(ticks);
                        timing = 1;
                }
                /* Sleep until fired, timed out, or a signal is due. */
                for (;;) {
                        s = spin_lock(&ps.ps_lock);
                        if (ps.ps_fired || l->l_timedout) {
                                spin_unlock(&ps.ps_lock, s);
                                break;
                        }
                        if (sv_wait_sig(&ps.ps_sv, &ps.ps_lock, s) != 0 &&
                            !l->l_timedout && (r = sig_sleep_check()) != 0) {
                                error = r == 1 ? EINTR : ERESTART;
                                break;
                        }
                }
                for (size_t i = 0; i < nfds; i++)
                        poll_unregister(&pds[i]);
                if (error != 0)
                        break;
        }
        for (size_t i = 0; i < nfds; i++)
                poll_unregister(&pds[i]);
        if (timing)
                lwp_timeout_stop(id);
        if (error == 0 && nfds != 0)
                error = copyout(fds, ufds, nfds * sizeof(*fds));
        if (error == 0)
                rv[0] = (uint64_t)n;
out:
        for (size_t i = 0; i < nfds; i++)
                if (fps[i] != NULL)
                        frele(fps[i]);
        if (nfds != 0) {
                kmem_free(fds, nfds * sizeof(*fds));
                kmem_free(fps, nfds * sizeof(*fps));
                kmem_free(pds, nfds * sizeof(*pds));
        }
        return error;
}

int
sys_poll(const uint64_t *args, uint64_t *rv)
{
        int timeout = (int)args[2];
        long ticks;

        /* Milliseconds to ticks, rounded up, and one more (see kern_sig). */
        if (timeout < 0)
                ticks = -1;
        else if (timeout == 0)
                ticks = 0;
        else
                ticks = ((long)timeout * HZ + 999) / 1000 + 1;
        return do_poll(args[0], (size_t)args[1], ticks, rv);
}

/*
 * pollsys(fds, nfds, const struct timespec *timeout, const sigset_t
 * *mask), Belka's own: poll with a timespec (NULL: no limit), and with
 * mask (unless NULL) held for the wait alone, swapped in and out in the
 * kernel so that no signal slips between. ppoll and pselect use it.
 */
int
sys_pollsys(const uint64_t *args, uint64_t *rv)
{
        long ticks = -1;
        int error;

        if (args[2] != 0 &&
            (error = lwp_timeout_ticks(args[2], &ticks)) != 0)
                return error;
        if (args[3] != 0 && (error = sig_wait_mask(args[3])) != 0)
                return error;
        error = do_poll(args[0], (size_t)args[1], ticks, rv);
        if (args[3] != 0)
                sig_wait_mask_end(error);
        return error;
}
