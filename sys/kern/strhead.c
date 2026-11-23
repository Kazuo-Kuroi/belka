/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/*
 * The stream head: opening a STREAMS device, pushing and popping
 * modules, read, write, getmsg, putmsg, poll, the stream ioctls, and
 * I_SETSIG's signals.
 *
 * The head is a queue pair of its own at the top of the stream, with a
 * read put procedure (strrput), which takes what comes up, and a write
 * service procedure (strwsrv), which runs when the stream below it
 * drains and wakes the writers. Messages go down with putnext from the
 * head's write queue, while the head's syncq is claimed, so that the
 * stream below cannot change under them.
 *
 * Locks: sd_plumb (plumbing), then the syncqs it claims, then sd_lock.
 * sd_rdlock (one reader) comes before sd_lock and is held while data
 * is copied to the user; sd_lock is never held across a copy.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mkdev.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/specvn.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#define STRSIG_EVENTS   (S_INPUT | S_HIPRI | S_OUTPUT | S_MSG | S_ERROR | \
    S_HANGUP | S_RDNORM | S_RDBAND | S_WRBAND | S_BANDURG)

#define STRHIGH         5120    /* the head's read queue */
#define STRLOW          1024
#define STRMAXBLK       65536   /* largest block a write makes */
#define STRCTLSZ        1024    /* largest control part putmsg takes */
#define STRIOCSZ        8192    /* largest I_STR data */
#define STRTIMOUT       15      /* I_STR's default wait, seconds */
#define STRCLTIME       15000   /* close's wait for output, ms */

struct strsig {
        struct strsig   *ss_next;
        struct proc     *ss_proc;       /* detached on descriptor close */
        int             ss_events;
};

static const struct vnodeops str_vnops;

static int strrput(queue_t *, mblk_t *);
static int strwsrv(queue_t *);
static void str_ioctimeout(void *);

static struct module_info strh_info = {
        0, "strhead", 0, INFPSZ, STRHIGH, STRLOW
};
static struct qinit strh_rinit = {
        strrput, NULL, NULL, NULL, NULL, &strh_info, NULL
};
static struct qinit strh_winit = {
        NULL, strwsrv, NULL, NULL, NULL, &strh_info, NULL
};

/* After an interrupted wait: what the system call returns. */
static int
sig_error(void)
{
        return sig_sleep_check() == 2 ? ERESTART : sig_restart(EINTR);
}

/* --- I_SETSIG. --- */

/* sd_lock held. p_lock excludes registration racing descriptor close. */
static struct strsig **
strsig_find(struct stdata *st, const struct proc *p)
{
        struct strsig **link = &st->sd_signals;

        while (*link != NULL && (*link)->ss_proc != p)
                link = &(*link)->ss_next;
        return link;
}

void
strsig_close(struct stdata *st, struct proc *p)
{
        struct strsig **link, *old;

        mutex_lock(&st->sd_lock);
        link = strsig_find(st, p);
        old = *link;
        if (old != NULL)
                *link = old->ss_next;
        mutex_unlock(&st->sd_lock);
        if (old != NULL)
                kmem_free(old, sizeof(*old));
}

int
strsig_ioctl(struct proc *p, int fd, unsigned long cmd, uint64_t arg)
{
        struct strsig *new = NULL, *old = NULL, **link;
        struct stdata *st;
        int events = 0, error = 0;

        mutex_lock(&p->p_fdt->fd_lock);
        if (fd < 0 || fd >= p->p_fdt->fd_size || p->p_fdt->fd_files[fd] == NULL) {
                error = EBADF;
                goto out;
        }
        st = p->p_fdt->fd_files[fd]->f_vnode->v_stream;
        if (st == NULL) {
                error = ENOSTR;
                goto out;
        }
        if (cmd == I_SETSIG && (arg & ~(uint64_t)STRSIG_EVENTS) != 0) {
                error = EINVAL;
                goto out;
        }
        mutex_lock(&st->sd_lock);
        link = strsig_find(st, p);
        if (cmd == I_GETSIG || arg == 0) {
                if (*link == NULL) {
                        error = EINVAL;
                } else if (cmd == I_GETSIG) {
                        events = (*link)->ss_events;
                } else {
                        old = *link;
                        *link = old->ss_next;
                }
        } else if (*link != NULL) {
                (*link)->ss_events = (int)arg;
        } else {
                new = kmem_alloc(sizeof(*new), KM_NOSLEEP);
                if (new == NULL) {
                        error = EAGAIN;
                } else {
                        new->ss_proc = p;
                        new->ss_events = (int)arg;
                        new->ss_next = NULL;
                        *link = new;
                }
        }
        mutex_unlock(&st->sd_lock);
out:
        mutex_unlock(&p->p_fdt->fd_lock);
        if (old != NULL)
                kmem_free(old, sizeof(*old));
        if (error == 0 && cmd == I_GETSIG)
                error = copyout(&events, (void *)(uintptr_t)arg, sizeof(events));
        return error;
}

/* sd_lock held: post SIGPOLL (or SIGURG) to those registered for events. */
static void
strsig_notify(struct stdata *st, int events, int code, int band)
{
        siginfo_t info = { .si_signo = SIGPOLL, .si_code = code };

        info.si_band = band;
        for (struct strsig *ss = st->sd_signals; ss != NULL; ss = ss->ss_next) {
                if ((ss->ss_events & events) == 0)
                        continue;
                if ((events & S_RDBAND) != 0 &&
                    (ss->ss_events & S_BANDURG) != 0) {
                        siginfo_t urg = { .si_signo = SIGURG };

                        sigtoproc(ss->ss_proc, SIGURG, &urg);
                } else {
                        sigtoproc(ss->ss_proc, SIGPOLL, &info);
                }
        }
}

/* sd_lock held: something changed that readers, writers or poll want. */
static void
str_wakeall(struct stdata *sd)
{
        sv_broadcast(&sd->sd_readsv);
        sv_broadcast(&sd->sd_writesv);
        pollwakeup(&sd->sd_poll);
}

/* --- The head's procedures. --- */

static void
str_setopts(struct stdata *sd, mblk_t *mp)
{
        struct stroptions *so = (struct stroptions *)mp->b_rptr;
        queue_t *rq = sd->sd_rq;

        if (MBLKL(mp) < sizeof(*so))
                return;
        mutex_lock(&sd->sd_lock);
        if ((so->so_flags & SO_READOPT) != 0) {
                int mode = so->so_readopt & RMODEMASK;
                int prot = so->so_readopt & RPROTMASK;

                if (mode != RMODEMASK)
                        sd->sd_rdopt = (sd->sd_rdopt & RPROTMASK) | mode;
                if (prot == RPROTNORM || prot == RPROTDAT || prot == RPROTDIS)
                        sd->sd_rdopt = (sd->sd_rdopt & RMODEMASK) | prot;
        }
        if ((so->so_flags & SO_WROFF) != 0)
                sd->sd_wroff = so->so_wroff;
        if ((so->so_flags & SO_MREADON) != 0)
                sd->sd_flag |= STRMREAD;
        if ((so->so_flags & SO_MREADOFF) != 0)
                sd->sd_flag &= ~STRMREAD;
        if ((so->so_flags & SO_DELIM) != 0)
                sd->sd_flag |= STRDELIM;
        if ((so->so_flags & SO_NODELIM) != 0)
                sd->sd_flag &= ~STRDELIM;
        if ((so->so_flags & SO_TOSTOP) != 0)
                sd->sd_flag |= STRTOSTOP;
        if ((so->so_flags & SO_TONSTOP) != 0)
                sd->sd_flag &= ~STRTOSTOP;
        mutex_unlock(&sd->sd_lock);
        if ((so->so_flags & SO_MINPSZ) != 0)
                (void)strqset(rq, QMINPSZ, 0, (intptr_t)so->so_minpsz);
        if ((so->so_flags & SO_MAXPSZ) != 0)
                (void)strqset(rq, QMAXPSZ, 0, (intptr_t)so->so_maxpsz);
        if ((so->so_flags & SO_HIWAT) != 0)
                (void)strqset(rq, QHIWAT, (so->so_flags & SO_BAND) != 0 ?
                    so->so_band : 0, (intptr_t)so->so_hiwat);
        if ((so->so_flags & SO_LOWAT) != 0)
                (void)strqset(rq, QLOWAT, (so->so_flags & SO_BAND) != 0 ?
                    so->so_band : 0, (intptr_t)so->so_lowat);
}

/* M_ERROR: one byte for both sides, or the read and the write errors. */
static void
str_error(struct stdata *sd, queue_t *q, mblk_t *mp)
{
        unsigned char rerr, werr;

        if (MBLKL(mp) == 0) {
                freemsg(mp);
                return;
        }
        rerr = mp->b_rptr[0];
        werr = MBLKL(mp) >= 2 ? mp->b_rptr[1] : rerr;
        mutex_lock(&sd->sd_lock);
        if (rerr != NOERROR) {
                sd->sd_rerror = rerr;
                if (rerr != 0)
                        sd->sd_flag |= STRDERR;
                else
                        sd->sd_flag &= ~STRDERR;
        }
        if (werr != NOERROR) {
                sd->sd_werror = werr;
                if (werr != 0)
                        sd->sd_flag |= STWRERR;
                else
                        sd->sd_flag &= ~STWRERR;
        }
        str_wakeall(sd);
        if ((sd->sd_flag & (STRDERR | STWRERR)) != 0)
                strsig_notify(sd, S_ERROR, POLL_ERR, 0);
        mutex_unlock(&sd->sd_lock);
        if ((rerr != 0 && rerr != NOERROR) || (werr != 0 && werr != NOERROR)) {
                /* Flush the stream, both ways, as an error does. */
                flushq(q, FLUSHDATA);
                mp->b_datap->db_type = M_FLUSH;
                mp->b_rptr[0] = FLUSHW;
                mp->b_wptr = mp->b_rptr + 1;
                qreply(q, mp);
                return;
        }
        freemsg(mp);
}

/* An ioctl's reply: the waiting ioctl's, if it is its. */
static void
str_iocreply(struct stdata *sd, mblk_t *mp)
{
        unsigned int id;
        int s;

        if (MBLKL(mp) < sizeof(struct iocblk)) {
                freemsg(mp);
                return;
        }
        if (DB_TYPE(mp) == M_COPYIN || DB_TYPE(mp) == M_COPYOUT)
                id = ((struct copyreq *)mp->b_rptr)->cq_id;
        else
                id = ((struct iocblk *)mp->b_rptr)->ioc_id;
        s = spin_lock(&sd->sd_iocspin);
        if (sd->sd_iocpending && id == sd->sd_iocid && sd->sd_iocblk == NULL) {
                sd->sd_iocblk = mp;
                mp = NULL;
                sv_broadcast(&sd->sd_iocsv);
        }
        spin_unlock(&sd->sd_iocspin, s);
        if (mp != NULL)
                freemsg(mp);
}

/* The head's read put procedure: what comes up the stream. */
static int
strrput(queue_t *q, mblk_t *mp)
{
        struct stdata *sd = q->q_stream;
        unsigned char type = DB_TYPE(mp);

        switch (type) {
        case M_DATA:
        case M_PROTO:
        case M_PCPROTO: {
                int hipri = type == M_PCPROTO, empty, events, code;
                unsigned char band = hipri ? 0 : mp->b_band;
                mblk_t *first;

                mutex_lock(&sd->sd_lock);
                first = q->q_first;
                if (hipri && first != NULL && DB_TYPE(first) >= QPCTL) {
                        /* One high priority message at a time. */
                        mutex_unlock(&sd->sd_lock);
                        freemsg(mp);
                        return 0;
                }
                if ((sd->sd_flag & STRDELIM) != 0)
                        mp->b_flag |= MSGDELIM;
                empty = first == NULL;
                if (!putq(q, mp)) {
                        mutex_unlock(&sd->sd_lock);
                        freemsg(mp);
                        return 0;
                }
                sv_broadcast(&sd->sd_readsv);
                pollwakeup(&sd->sd_poll);
                /* Signal arrivals at an empty head, and every urgent one. */
                if (hipri) {
                        events = S_HIPRI;
                        code = POLL_PRI;
                } else {
                        events = empty ? S_INPUT |
                            (band == 0 ? S_RDNORM : S_RDBAND) : 0;
                        code = POLL_IN;
                }
                if (events != 0)
                        strsig_notify(sd, events, code, band);
                mutex_unlock(&sd->sd_lock);
                return 0;
        }
        case M_FLUSH: {
                unsigned char how = MBLKL(mp) > 0 ? mp->b_rptr[0] : 0;
                int band = (how & FLUSHBAND) != 0 && MBLKL(mp) > 1 ?
                    mp->b_rptr[1] : -1;

                if ((how & FLUSHR) != 0) {
                        mutex_lock(&sd->sd_lock);
                        if (band >= 0)
                                flushband(q, (unsigned char)band, FLUSHALL);
                        else
                                flushq(q, FLUSHALL);
                        mutex_unlock(&sd->sd_lock);
                }
                if ((how & FLUSHW) != 0) {
                        mp->b_rptr[0] &= ~FLUSHR;
                        qreply(q, mp);
                } else {
                        freemsg(mp);
                }
                return 0;
        }
        case M_HANGUP:
                mutex_lock(&sd->sd_lock);
                sd->sd_flag |= STRHUP;
                str_wakeall(sd);
                strsig_notify(sd, S_HANGUP, POLL_HUP, 0);
                mutex_unlock(&sd->sd_lock);
                freemsg(mp);
                return 0;
        case M_UNHANGUP:
                mutex_lock(&sd->sd_lock);
                sd->sd_flag &= ~STRHUP;
                str_wakeall(sd);
                mutex_unlock(&sd->sd_lock);
                freemsg(mp);
                return 0;
        case M_ERROR:
                str_error(sd, q, mp);
                return 0;
        case M_SIG:
        case M_PCSIG:
                if (MBLKL(mp) > 0 && mp->b_rptr[0] == SIGPOLL) {
                        mutex_lock(&sd->sd_lock);
                        strsig_notify(sd, S_MSG, POLL_MSG, 0);
                        mutex_unlock(&sd->sd_lock);
                }
                freemsg(mp);
                return 0;
        case M_SETOPTS:
                str_setopts(sd, mp);
                freemsg(mp);
                return 0;
        case M_IOCACK:
        case M_IOCNAK:
        case M_COPYIN:
        case M_COPYOUT:
                str_iocreply(sd, mp);
                return 0;
        default:
                freemsg(mp);
                return 0;
        }
}

/* The stream below the head has drained: wake the writers. */
static int
strwsrv(queue_t *q)
{
        struct stdata *sd = q->q_stream;
        int s = spin_lock(&q->q_lock);

        q->q_flag &= ~QBACK;
        spin_unlock(&q->q_lock, s);
        mutex_lock(&sd->sd_lock);
        sd->sd_wgen++;
        sv_broadcast(&sd->sd_writesv);
        pollwakeup(&sd->sd_poll);
        strsig_notify(sd, S_OUTPUT, POLL_OUT, 0);
        mutex_unlock(&sd->sd_lock);
        return 0;
}

/* Send mp down from the head. */
static void
str_down(struct stdata *sd, mblk_t *mp)
{
        struct syncq *sq = sd->sd_wq->q_syncq;

        sq_enter(sq);
        (void)putnext(sd->sd_wq, mp);
        sq_exit(sq);
}

static int
str_canput(struct stdata *sd, int band)
{
        struct syncq *sq = sd->sd_wq->q_syncq;
        int ok;

        sq_enter(sq);
        ok = sd->sd_wq->q_next == NULL ||
            bcanputnext(sd->sd_wq, (unsigned char)band);
        sq_exit(sq);
        return ok;
}

/* --- Opening and plumbing. --- */

static struct syncq *
strdev_syncq(struct strdev *dev)
{
        static struct mutex lock;
        static int ready;
        struct syncq *sq;

        if (dev->d_sqlvl != SQLVL_MODULE)
                return sq_alloc();
        if (!ready) {
                /* First call is from a single opener before any race. */
                mutex_init(&lock, "strdevsq");
                ready = 1;
        }
        mutex_lock(&lock);
        if (dev->d_syncq == NULL)
                dev->d_syncq = sq_alloc();      /* the table's reference */
        sq = dev->d_syncq;
        sq_hold(sq);
        mutex_unlock(&lock);
        return sq;
}

static struct mutex fmod_lock;
static int fmod_ready;

static struct syncq *
fmod_syncq(struct fmodsw *f)
{
        struct syncq *sq;

        if (f->f_sqlvl != SQLVL_MODULE)
                return sq_alloc();
        mutex_lock(&fmod_lock);
        if (f->f_syncq == NULL)
                f->f_syncq = sq_alloc();
        sq = f->f_syncq;
        sq_hold(sq);
        mutex_unlock(&fmod_lock);
        return sq;
}

static void
str_setpsz(struct stdata *sd)
{
        queue_t *below = sd->sd_wq->q_next;

        sd->sd_minpsz = below != NULL ? below->q_minpsz : 0;
        sd->sd_maxpsz = below != NULL ? below->q_maxpsz : INFPSZ;
}

/* Link the pair at rq in below the head (and above what was there). */
static void
str_link(struct stdata *sd, queue_t *rq)
{
        queue_t *wq = WR(rq), *hw = sd->sd_wq, *hr = sd->sd_rq;
        queue_t *bw = hw->q_next;
        int s = spin_lock(&sd->sd_qlock);

        wq->q_next = bw;
        wq->q_back = hw;
        hw->q_next = wq;
        if (bw != NULL) {
                bw->q_back = wq;
                RD(bw)->q_next = rq;
                rq->q_back = RD(bw);
        } else {
                rq->q_back = NULL;
        }
        rq->q_next = hr;
        hr->q_back = rq;
        spin_unlock(&sd->sd_qlock, s);
}

/* Take out the pair at rq, which is just below the head. */
static void
str_unlink(struct stdata *sd, queue_t *rq)
{
        queue_t *wq = WR(rq), *hw = sd->sd_wq, *hr = sd->sd_rq;
        queue_t *bw = wq->q_next;
        int s = spin_lock(&sd->sd_qlock);

        hw->q_next = bw;
        if (bw != NULL) {
                bw->q_back = hw;
                RD(bw)->q_next = hr;
                hr->q_back = RD(bw);
        } else {
                hr->q_back = NULL;
        }
        wq->q_next = wq->q_back = NULL;
        rq->q_next = rq->q_back = NULL;
        spin_unlock(&sd->sd_qlock, s);
}

/* The syncqs to claim to change the stream at the pair below the head. */
static int
str_plumbset(struct stdata *sd, queue_t *mid, struct syncq **set)
{
        queue_t *below = mid != NULL ? WR(mid)->q_next : sd->sd_wq->q_next;
        int n = 0;

        set[n++] = sd->sd_wq->q_syncq;
        if (mid != NULL)
                set[n++] = mid->q_syncq;
        if (below != NULL)
                set[n++] = below->q_syncq;
        return n;
}

/* sd_plumb held: close and free the pair just below the head. */
static void
str_closetop(struct stdata *sd, int flag, struct cred *cr)
{
        queue_t *rq = RD(sd->sd_wq->q_next);
        struct syncq *set[3];
        int n = str_plumbset(sd, rq, set);

        sq_claim_set(set, n);
        if (rq->q_qinfo->qi_qclose != NULL)
                (void)rq->q_qinfo->qi_qclose(rq, flag, cr);
        qprocsoff(rq);
        str_unlink(sd, rq);
        sq_purge(rq->q_syncq, rq);
        str_setpsz(sd);
        sq_exit_set(set, n);
        freeq(rq);
}

static int
str_push(struct stdata *sd, const char *name, int flag, struct cred *cr)
{
        struct fmodsw *f;
        struct syncq *sq, *set[3];
        queue_t *rq;
        dev_t dev = sd->sd_attr.sa_rdev;
        int n, error;

        if (!fmod_ready) {
                mutex_init(&fmod_lock, "fmodsw");
                fmod_ready = 1;
        }
        if ((f = fmod_find(name)) == NULL)
                return EINVAL;
        mutex_lock(&sd->sd_plumb);
        if (sd->sd_pushcnt >= NSTRPUSH) {
                mutex_unlock(&sd->sd_plumb);
                return EINVAL;
        }
        if ((sd->sd_flag & STRHUP) != 0) {
                mutex_unlock(&sd->sd_plumb);
                return ENXIO;
        }
        rq = allocq();
        sq = fmod_syncq(f);
        setq(rq, f->f_str->st_rdinit, f->f_str->st_wrinit, sq, sd);
        sq_rele(sq);                    /* setq holds it */
        n = str_plumbset(sd, NULL, set);
        set[n++] = sq;
        sq_claim_set(set, n);
        str_link(sd, rq);
        qprocson(rq);
        error = rq->q_qinfo->qi_qopen != NULL ?
            rq->q_qinfo->qi_qopen(rq, &dev, flag, MODOPEN, cr) : 0;
        if (error != 0) {
                qprocsoff(rq);
                str_unlink(sd, rq);
                sq_purge(sq, rq);
        } else {
                sd->sd_pushcnt++;
                str_setpsz(sd);
        }
        sq_exit_set(set, n);
        if (error != 0)
                freeq(rq);
        mutex_unlock(&sd->sd_plumb);
        return error;
}

static int
str_pop(struct stdata *sd, int flag, struct cred *cr)
{
        mutex_lock(&sd->sd_plumb);
        if (sd->sd_pushcnt == 0) {
                mutex_unlock(&sd->sd_plumb);
                return EINVAL;
        }
        str_closetop(sd, flag, cr);
        sd->sd_pushcnt--;
        mutex_unlock(&sd->sd_plumb);
        return 0;
}

/*
 * Open the STREAMS device dev: a new stream, whose vnode replaces the
 * device's node in *vpp.
 */
int
str_clone(struct strdev *dev, struct vnode **vpp, int flags, struct cred *cr)
{
        struct stdata *sd = kmem_zalloc(sizeof(*sd), KM_SLEEP);
        struct streamtab *st = dev->d_str;
        struct syncq *hsq, *dsq, *set[2];
        queue_t *hq, *dq;
        dev_t devno;
        int error;

        mutex_init(&sd->sd_lock, "stream");
        mutex_init(&sd->sd_plumb, "strplumb");
        mutex_init(&sd->sd_rdlock, "strread");
        mutex_init(&sd->sd_iocwait, "strioctl");
        spinlock_init(&sd->sd_qlock, "strqlock");
        spinlock_init(&sd->sd_iocspin, "strioc");
        sv_init(&sd->sd_readsv, "strread");
        sv_init(&sd->sd_writesv, "strwrite");
        sv_init(&sd->sd_iocsv, "strioc");
        pollhead_init(&sd->sd_poll);
        sd->sd_dev = dev;
        sd->sd_rdopt = RNORM | RPROTNORM;
        sd->sd_closetime = STRCLTIME;

        hq = allocq();
        hsq = sq_alloc();
        setq(hq, &strh_rinit, &strh_winit, hsq, sd);
        sq_rele(hsq);
        hq[0].q_flag |= QISHEAD;
        hq[1].q_flag |= QISHEAD;
        sd->sd_rq = hq;
        sd->sd_wq = WR(hq);

        dq = allocq();
        dsq = strdev_syncq(dev);
        setq(dq, st->st_rdinit, st->st_wrinit, dsq, sd);
        sq_rele(dsq);

        devno = makedev(dev->d_major, atomic_inc_32_nv(&dev->d_nextminor));
        specattr_init(&sd->sd_attr, dev->d_mode, cr->cr_uid, cr->cr_gid,
            devno);
        vn_init(&sd->sd_vnode, (*vpp)->v_vfsp, VCHR, &str_vnops,
            &sd->sd_attr);

        set[0] = hsq;
        set[1] = dsq;
        sq_claim_set(set, 2);
        str_link(sd, dq);
        qprocson(dq);
        error = dq->q_qinfo->qi_qopen != NULL ?
            dq->q_qinfo->qi_qopen(dq, &devno, flags, CLONEOPEN, cr) : 0;
        if (error != 0) {
                qprocsoff(dq);
                str_unlink(sd, dq);
                sq_purge(dsq, dq);
        } else {
                sd->sd_attr.sa_rdev = devno;
                sd->sd_vnode.v_rdev = devno;
                str_setpsz(sd);
        }
        sq_exit_set(set, 2);
        if (error != 0) {
                freeq(dq);
                sq_enter(hsq);
                sq_purge(hsq, hq);
                sq_exit(hsq);
                freeq(hq);
                vn_destroy(&sd->sd_vnode);
                kmem_free(sd, sizeof(*sd));
                return error;
        }
        sd->sd_vnode.v_stream = sd;
        VN_HOLD(&sd->sd_vnode);
        VN_RELE(*vpp);
        *vpp = &sd->sd_vnode;
        return 0;
}

/* Whether anything is still waiting to go down the stream. */
static int
str_output_pending(struct stdata *sd)
{
        int s = spin_lock(&sd->sd_qlock), busy = 0;

        for (queue_t *q = sd->sd_wq->q_next; q != NULL && !busy;
            q = q->q_next)
                busy = q->q_first != NULL;
        spin_unlock(&sd->sd_qlock, s);
        return busy;
}

/* The last close: pop every module, close the driver, free it all. */
static void
str_close(struct stdata *sd, int flag, struct cred *cr)
{
        struct syncq *hsq = sd->sd_wq->q_syncq;

        if ((flag & (FNDELAY | FNONBLOCK)) == 0) {
                long ticks = (long)sd->sd_closetime * HZ / 1000;

                while (ticks-- > 0 && str_output_pending(sd))
                        delay(1);
        }
        mutex_lock(&sd->sd_plumb);
        sd->sd_flag |= STRCLOSE;
        while (sd->sd_wq->q_next != NULL)
                str_closetop(sd, flag, cr);
        sd->sd_pushcnt = 0;
        mutex_unlock(&sd->sd_plumb);

        sq_enter(hsq);
        qprocsoff(sd->sd_rq);
        sq_purge(hsq, sd->sd_rq);
        sq_exit(hsq);
        freeq(sd->sd_rq);

        while (sd->sd_signals != NULL) {
                struct strsig *ss = sd->sd_signals;

                sd->sd_signals = ss->ss_next;
                kmem_free(ss, sizeof(*ss));
        }
        if (sd->sd_iocblk != NULL)
                freemsg(sd->sd_iocblk);
}

/* --- Reading. --- */

/* sd_lock held: the error a read gets now, or 0. */
static int
str_rerror(struct stdata *sd)
{
        return (sd->sd_flag & STRDERR) != 0 ? sd->sd_rerror : 0;
}

static int
str_werror(struct stdata *sd)
{
        if ((sd->sd_flag & STWRERR) != 0)
                return sd->sd_werror;
        if ((sd->sd_flag & STRHUP) != 0)
                return ENXIO;
        return 0;
}

/*
 * sd_lock held: wait for something to arrive at the head. 0 when it
 * may have; else the error (EAGAIN, a signal's), with sd_lock dropped.
 */
static int
str_waitread(struct stdata *sd, int ioflag)
{
        int r;

        if ((ioflag & (IO_NONBLOCK | IO_NDELAY)) != 0) {
                mutex_unlock(&sd->sd_lock);
                return EAGAIN;
        }
        if ((sd->sd_flag & STRMREAD) != 0) {
                mblk_t *mp = allocb(sizeof(ssize_t), BPRI_HI);

                if (mp != NULL) {
                        mutex_unlock(&sd->sd_lock);
                        mp->b_datap->db_type = M_READ;
                        mp->b_wptr += sizeof(ssize_t);
                        str_down(sd, mp);
                        mutex_lock(&sd->sd_lock);
                        if (sd->sd_rq->q_first != NULL)
                                return 0;
                }
        }
        if ((r = sv_wait_mutex_sig(&sd->sd_readsv, &sd->sd_lock)) != 0)
                return sig_error();
        mutex_lock(&sd->sd_lock);
        return 0;
}

/* Copy from the blocks of mp to the user, consuming what is copied. */
static int
str_copyout_msg(mblk_t *mp, struct uio *uio, size_t *done)
{
        int error = 0;

        *done = 0;
        for (mblk_t *bp = mp; bp != NULL && uio->uio_resid > 0 && error == 0;
            bp = bp->b_cont) {
                size_t n = MBLKL(bp);
                ssize_t before = uio->uio_resid;

                if (n > (size_t)uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if (n == 0)
                        continue;
                error = uiomove(bp->b_rptr, n, UIO_READ, uio);
                n = (size_t)(before - uio->uio_resid);
                bp->b_rptr += n;
                *done += n;
        }
        return error;
}

/* Free the leading blocks of mp that are empty; what is left. */
static mblk_t *
str_trim(mblk_t *mp)
{
        while (mp != NULL && MBLKL(mp) == 0) {
                mblk_t *next = mp->b_cont;

                if (next != NULL) {
                        next->b_band = mp->b_band;
                        next->b_flag = mp->b_flag;
                }
                freeb(mp);
                mp = next;
        }
        return mp;
}

static int
str_read(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct stdata *sd = vp->v_stream;
        queue_t *rq = sd->sd_rq;
        size_t total = 0;
        int error = 0, mode, prot, delim;

        mutex_lock(&sd->sd_rdlock);
        mutex_lock(&sd->sd_lock);
        for (;;) {
                mblk_t *mp;
                unsigned char type;
                size_t n;

                if ((error = str_rerror(sd)) != 0)
                        break;
                if ((mp = getq(rq)) == NULL) {
                        if ((sd->sd_flag & STRHUP) != 0 || total > 0)
                                break;
                        if ((error = str_waitread(sd, ioflag)) != 0) {
                                if (error == EAGAIN &&
                                    (ioflag & IO_NONBLOCK) == 0)
                                        error = 0;      /* O_NDELAY: 0 */
                                mutex_unlock(&sd->sd_rdlock);
                                return error;
                        }
                        continue;
                }
                mode = sd->sd_rdopt & RMODEMASK;
                prot = sd->sd_rdopt & RPROTMASK;
                type = DB_TYPE(mp);
                if (type != M_DATA) {
                        if (prot == RPROTNORM || prot == 0) {
                                (void)putbq(rq, mp);
                                if (total == 0)
                                        error = EBADMSG;
                                break;
                        }
                        if (prot == RPROTDIS) {
                                while (mp != NULL && DB_TYPE(mp) != M_DATA) {
                                        mblk_t *next = mp->b_cont;

                                        if (next != NULL)
                                                next->b_flag = mp->b_flag;
                                        freeb(mp);
                                        mp = next;
                                }
                                if (mp == NULL)
                                        continue;
                        } else {
                                for (mblk_t *bp = mp; bp != NULL;
                                    bp = bp->b_cont)
                                        bp->b_datap->db_type = M_DATA;
                        }
                }
                if (msgsize(mp) == 0) {
                        /* A zero-length message ends a read, as end of file. */
                        if (total > 0) {
                                (void)putbq(rq, mp);
                        } else {
                                freemsg(mp);
                        }
                        break;
                }
                delim = (mp->b_flag & MSGDELIM) != 0;
                /* Copy without sd_lock; sd_rdlock keeps other readers out. */
                mutex_unlock(&sd->sd_lock);
                error = str_copyout_msg(mp, uio, &n);
                total += n;
                mp = str_trim(mp);
                mutex_lock(&sd->sd_lock);
                if (mp != NULL) {
                        if (mode == RMSGD)
                                freemsg(mp);
                        else
                                (void)putbq(rq, mp);
                }
                if (error != 0 || mode != RNORM || uio->uio_resid == 0)
                        break;
                if (mp == NULL && (delim || rq->q_first == NULL))
                        break;
        }
        mutex_unlock(&sd->sd_lock);
        mutex_unlock(&sd->sd_rdlock);
        return error;
}

/* --- Writing. --- */

/*
 * Wait until band may be sent down (0), or for an error, EAGAIN, or a
 * signal. gen is sd_wgen when the caller last found the stream full.
 */
static int
str_waitwrite(struct stdata *sd, int band, int ioflag)
{
        for (;;) {
                unsigned int gen;
                int r, error;

                mutex_lock(&sd->sd_lock);
                if ((error = str_werror(sd)) != 0) {
                        mutex_unlock(&sd->sd_lock);
                        return error;
                }
                gen = sd->sd_wgen;
                mutex_unlock(&sd->sd_lock);
                if (str_canput(sd, band))
                        return 0;
                if ((ioflag & (IO_NONBLOCK | IO_NDELAY)) != 0)
                        return EAGAIN;
                mutex_lock(&sd->sd_lock);
                if (gen != sd->sd_wgen || str_werror(sd) != 0) {
                        mutex_unlock(&sd->sd_lock);
                        continue;
                }
                if ((r = sv_wait_mutex_sig(&sd->sd_writesv, &sd->sd_lock)) != 0)
                        return sig_error();
        }
}

/* A write that failed on the stream's error: SIGPIPE, if SNDPIPE asks. */
static void
str_sigpipe(struct stdata *sd, int error)
{
        struct proc *p = curproc;
        siginfo_t si;
        int s;

        if ((sd->sd_wropt & SNDPIPE) == 0 || error == EAGAIN ||
            error == EINTR || error == ERESTART)
                return;
        memset(&si, 0, sizeof(si));
        si.si_signo = SIGPIPE;
        si.si_code = SI_USER;
        si.si_pid = p->p_pid;
        s = spin_lock(&p->p_siglock);
        sigtolwp_locked(p, curlwp, SIGPIPE, &si);
        spin_unlock(&p->p_siglock, s);
}

static int
str_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct stdata *sd = vp->v_stream;
        size_t total = (size_t)uio->uio_resid, chunk;
        int error = 0, wrote = 0;

        mutex_lock(&sd->sd_lock);
        error = str_werror(sd);
        mutex_unlock(&sd->sd_lock);
        if (error != 0) {
                str_sigpipe(sd, error);
                return error;
        }
        if (sd->sd_minpsz > 0 && total < (size_t)sd->sd_minpsz)
                return ERANGE;
        if (sd->sd_maxpsz == 0)
                return ERANGE;
        chunk = sd->sd_maxpsz == INFPSZ ? STRMAXBLK : (size_t)sd->sd_maxpsz;
        if (chunk > STRMAXBLK)
                chunk = STRMAXBLK;
        if (total == 0) {
                mblk_t *mp;

                if ((sd->sd_wropt & SNDZERO) == 0)
                        return 0;
                if ((error = str_waitwrite(sd, 0, ioflag)) != 0)
                        return error;
                mp = allocb_wait(0);
                str_down(sd, mp);
                return 0;
        }
        while (uio->uio_resid > 0) {
                size_t n = (size_t)uio->uio_resid;
                ssize_t before = uio->uio_resid;
                mblk_t *mp;

                if ((error = str_waitwrite(sd, 0, ioflag)) != 0) {
                        if (wrote && (error == EAGAIN || error == EINTR ||
                            error == ERESTART))
                                error = 0;      /* what was written */
                        break;
                }
                if (n > chunk)
                        n = chunk;
                mp = allocb_wait(n + sd->sd_wroff);
                mp->b_rptr += sd->sd_wroff;
                mp->b_wptr = mp->b_rptr;
                if ((error = uiomove(mp->b_wptr, n, UIO_WRITE, uio)) != 0) {
                        freemsg(mp);
                        break;
                }
                mp->b_wptr += (size_t)(before - uio->uio_resid);
                str_down(sd, mp);
                wrote = 1;
        }
        if (error != 0 && !wrote)
                str_sigpipe(sd, error);
        return error;
}

/* --- getmsg and putmsg. --- */

/* Copy up to sb->maxlen bytes of blocks to sb->buf; len gets the count. */
static int
str_copyout_part(mblk_t *mp, struct strbuf *sb, int retrieve, int *more)
{
        size_t room, done = 0;
        int error = 0;

        *more = 0;
        if (!retrieve) {
                *more = mp != NULL;
                return 0;
        }
        if (mp == NULL) {
                sb->len = -1;
                return 0;
        }
        room = sb->maxlen > 0 ? (size_t)sb->maxlen : 0;
        for (mblk_t *bp = mp; bp != NULL; bp = bp->b_cont) {
                size_t n = MBLKL(bp);

                if (n > room - done)
                        n = room - done;
                if (n > 0 && (error = copyout(bp->b_rptr, sb->buf + done,
                    n)) != 0)
                        return error;
                bp->b_rptr += n;
                done += n;
                if (MBLKL(bp) != 0)
                        *more = 1;
        }
        sb->len = (int)done;
        return 0;
}

static int
str_getstrbuf(uint64_t uaddr, struct strbuf *sb, int *present)
{
        int error;

        *present = 0;
        if (uaddr == 0)
                return 0;
        if ((error = copyin((const void *)(uintptr_t)uaddr, sb,
            sizeof(*sb))) != 0)
                return error;
        *present = 1;
        return 0;
}

/* Write back a strbuf's len after getmsg. */
static int
str_putlen(uint64_t uaddr, const struct strbuf *sb)
{
        if (uaddr == 0)
                return 0;
        return copyout(&sb->len, (char *)(uintptr_t)uaddr +
            offsetof(struct strbuf, len), sizeof(sb->len));
}

/* Whether mp, first at the head, is one getpmsg with flags may take. */
static int
str_msgok(const mblk_t *mp, int flags, int band)
{
        int hipri = DB_TYPE(mp) >= QPCTL;

        if ((flags & MSG_HIPRI) != 0)
                return hipri;
        if ((flags & MSG_BAND) != 0)
                return hipri || mp->b_band >= band;
        return 1;
}

static int
str_getmsg(struct stdata *sd, uint64_t uctl, uint64_t udata, int *bandp,
    int *flagsp, int ioflag, uint64_t *rv)
{
        struct strbuf ctl = { -1, -1, NULL }, data = { -1, -1, NULL };
        int hasctl, hasdata, error, flags = *flagsp, morec, mored, hipri;
        mblk_t *mp, *cp, *dp, **link;
        queue_t *rq = sd->sd_rq;
        unsigned char type, band;

        if (flags != MSG_HIPRI && flags != MSG_ANY && flags != MSG_BAND)
                return EINVAL;
        if ((error = str_getstrbuf(uctl, &ctl, &hasctl)) != 0 ||
            (error = str_getstrbuf(udata, &data, &hasdata)) != 0)
                return error;
        mutex_lock(&sd->sd_rdlock);
        mutex_lock(&sd->sd_lock);
        for (;;) {
                if ((error = str_rerror(sd)) != 0) {
                        mutex_unlock(&sd->sd_lock);
                        mutex_unlock(&sd->sd_rdlock);
                        return error;
                }
                mp = rq->q_first;
                if (mp != NULL && str_msgok(mp, flags, *bandp))
                        break;
                if (mp == NULL && (sd->sd_flag & STRHUP) != 0) {
                        /* Hung up and drained: both parts empty. */
                        mutex_unlock(&sd->sd_lock);
                        mutex_unlock(&sd->sd_rdlock);
                        ctl.len = data.len = 0;
                        if (hasctl && ctl.maxlen >= 0)
                                error = str_putlen(uctl, &ctl);
                        if (error == 0 && hasdata && data.maxlen >= 0)
                                error = str_putlen(udata, &data);
                        *flagsp = 0;
                        *rv = 0;
                        return error;
                }
                if ((error = str_waitread(sd, ioflag)) != 0) {
                        mutex_unlock(&sd->sd_rdlock);
                        return error;
                }
        }
        mp = getq(rq);
        mutex_unlock(&sd->sd_lock);

        type = DB_TYPE(mp);
        hipri = type >= QPCTL;
        band = mp->b_band;
        /* Split the leading control blocks from the data. */
        cp = NULL;
        dp = mp;
        link = &cp;
        while (dp != NULL && DB_TYPE(dp) != M_DATA) {
                *link = dp;
                link = &dp->b_cont;
                dp = dp->b_cont;
        }
        *link = NULL;
        error = str_copyout_part(cp, &ctl, hasctl && ctl.maxlen >= 0, &morec);
        if (error == 0)
                error = str_copyout_part(dp, &data,
                    hasdata && data.maxlen >= 0, &mored);
        /* Put back what is left, as the same kind of message. */
        cp = str_trim(cp);
        dp = str_trim(dp);
        if (cp != NULL) {
                linkb(cp, dp);
                mp = cp;
        } else {
                mp = dp;
        }
        mutex_lock(&sd->sd_lock);
        if (mp != NULL) {
                mp->b_band = band;
                if (hipri && DB_TYPE(mp) == M_DATA)
                        mp->b_datap->db_type = M_PCPROTO;
                (void)putbq(rq, mp);
        }
        mutex_unlock(&sd->sd_lock);
        mutex_unlock(&sd->sd_rdlock);
        if (error != 0)
                return error;
        if (hasctl && ctl.maxlen >= 0 && (error = str_putlen(uctl, &ctl)) != 0)
                return error;
        if (hasdata && data.maxlen >= 0 &&
            (error = str_putlen(udata, &data)) != 0)
                return error;
        *flagsp = hipri ? MSG_HIPRI : MSG_BAND;
        *bandp = band;
        *rv = (morec ? MORECTL : 0) | (mored ? MOREDATA : 0);
        return 0;
}

static int
str_putmsg(struct stdata *sd, uint64_t uctl, uint64_t udata, int band,
    int flags, int ioflag)
{
        struct strbuf ctl, data;
        int hasctl, hasdata, error, hipri = (flags & MSG_HIPRI) != 0;
        mblk_t *cp = NULL, *dp = NULL, *mp;

        if ((flags != MSG_HIPRI && flags != MSG_BAND) || band < 0 ||
            band > 255 || (hipri && band != 0))
                return EINVAL;
        if ((error = str_getstrbuf(uctl, &ctl, &hasctl)) != 0 ||
            (error = str_getstrbuf(udata, &data, &hasdata)) != 0)
                return error;
        hasctl = hasctl && ctl.len >= 0;
        hasdata = hasdata && data.len >= 0;
        if (hipri && !hasctl)
                return EINVAL;
        mutex_lock(&sd->sd_lock);
        error = str_werror(sd);
        mutex_unlock(&sd->sd_lock);
        if (error != 0) {
                str_sigpipe(sd, error);
                return error;
        }
        if (!hasctl && !hasdata)
                return 0;
        if (hasctl && ctl.len > STRCTLSZ)
                return ERANGE;
        if (hasdata && ((sd->sd_minpsz > 0 && data.len < sd->sd_minpsz) ||
            (sd->sd_maxpsz != INFPSZ && data.len > sd->sd_maxpsz) ||
            data.len > STRMAXBLK))
                return ERANGE;
        if (!hipri && (error = str_waitwrite(sd, band, ioflag)) != 0)
                return error;
        if (hasctl) {
                cp = allocb_wait((size_t)ctl.len);
                cp->b_datap->db_type = hipri ? M_PCPROTO : M_PROTO;
                if (ctl.len > 0 && (error = copyin(ctl.buf, cp->b_wptr,
                    (size_t)ctl.len)) != 0) {
                        freemsg(cp);
                        return error;
                }
                cp->b_wptr += ctl.len;
        }
        if (hasdata) {
                dp = allocb_wait((size_t)data.len + sd->sd_wroff);
                dp->b_rptr += sd->sd_wroff;
                dp->b_wptr = dp->b_rptr;
                if (data.len > 0 && (error = copyin(data.buf, dp->b_wptr,
                    (size_t)data.len)) != 0) {
                        if (cp != NULL)
                                freemsg(cp);
                        freemsg(dp);
                        return error;
                }
                dp->b_wptr += data.len;
        }
        if (cp != NULL) {
                cp->b_cont = dp;
                mp = cp;
        } else {
                mp = dp;
        }
        mp->b_band = (unsigned char)band;
        str_down(sd, mp);
        return 0;
}

static int
str_msgsyscall(int fd, int need, struct file **fpp, struct stdata **sdp)
{
        struct file *fp = fd_get(curproc, fd);

        if (fp == NULL)
                return EBADF;
        if ((fp->f_flag & need) == 0) {
                frele(fp);
                return EBADF;
        }
        if ((*sdp = fp->f_vnode->v_stream) == NULL) {
                frele(fp);
                return ENOSTR;
        }
        *fpp = fp;
        return 0;
}

static int
str_ioflag(const struct file *fp)
{
        return ((fp->f_flag & FNONBLOCK) != 0 ? IO_NONBLOCK : 0) |
            ((fp->f_flag & FNDELAY) != 0 ? IO_NONBLOCK : 0);
}

/* getmsg(fd, ctlptr, dataptr, flagsp), System V's 85. */
int
sys_getmsg(const uint64_t *args, uint64_t *rv)
{
        struct stdata *sd;
        struct file *fp;
        int flags, band = 0, error;

        if (args[3] == 0)
                return EFAULT;
        if ((error = copyin((const void *)(uintptr_t)args[3], &flags,
            sizeof(flags))) != 0)
                return error;
        if (flags != 0 && flags != RS_HIPRI)
                return EINVAL;
        flags = flags == RS_HIPRI ? MSG_HIPRI : MSG_ANY;
        if ((error = str_msgsyscall((int)args[0], FREAD, &fp, &sd)) != 0)
                return error;
        error = str_getmsg(sd, args[1], args[2], &band, &flags,
            str_ioflag(fp), rv);
        frele(fp);
        if (error == 0) {
                flags = flags == MSG_HIPRI ? RS_HIPRI : 0;
                error = copyout(&flags, (void *)(uintptr_t)args[3],
                    sizeof(flags));
        }
        return error;
}

/* getpmsg(fd, ctlptr, dataptr, bandp, flagsp), Belka's 246. */
int
sys_getpmsg(const uint64_t *args, uint64_t *rv)
{
        struct stdata *sd;
        struct file *fp;
        int flags, band, error;

        if (args[3] == 0 || args[4] == 0)
                return EFAULT;
        if ((error = copyin((const void *)(uintptr_t)args[3], &band,
            sizeof(band))) != 0 ||
            (error = copyin((const void *)(uintptr_t)args[4], &flags,
            sizeof(flags))) != 0)
                return error;
        if (band < 0 || band > 255 || (flags == MSG_HIPRI && band != 0))
                return EINVAL;
        if ((error = str_msgsyscall((int)args[0], FREAD, &fp, &sd)) != 0)
                return error;
        error = str_getmsg(sd, args[1], args[2], &band, &flags,
            str_ioflag(fp), rv);
        frele(fp);
        if (error == 0 && (error = copyout(&band, (void *)(uintptr_t)args[3],
            sizeof(band))) == 0)
                error = copyout(&flags, (void *)(uintptr_t)args[4],
                    sizeof(flags));
        return error;
}

/* putmsg(fd, ctlptr, dataptr, flags), System V's 86. */
int
sys_putmsg(const uint64_t *args, uint64_t *rv __unused)
{
        struct stdata *sd;
        struct file *fp;
        int flags = (int)args[3], error;

        if (flags != 0 && flags != RS_HIPRI)
                return EINVAL;
        if ((error = str_msgsyscall((int)args[0], FWRITE, &fp, &sd)) != 0)
                return error;
        error = str_putmsg(sd, args[1], args[2], 0,
            flags == RS_HIPRI ? MSG_HIPRI : MSG_BAND, str_ioflag(fp));
        frele(fp);
        return error;
}

/* putpmsg(fd, ctlptr, dataptr, band, flags), Belka's 247. */
int
sys_putpmsg(const uint64_t *args, uint64_t *rv __unused)
{
        struct stdata *sd;
        struct file *fp;
        int error;

        if ((error = str_msgsyscall((int)args[0], FWRITE, &fp, &sd)) != 0)
                return error;
        error = str_putmsg(sd, args[1], args[2], (int)args[3], (int)args[4],
            str_ioflag(fp));
        frele(fp);
        return error;
}

/* --- ioctl. --- */

/* I_STR: send an M_IOCTL down and wait for its answer. */
static int
str_istr(struct stdata *sd, uint64_t arg, int flag, struct cred *cr,
    uint64_t *rv)
{
        static volatile uint32_t nextid;
        struct strioctl sio;
        struct iocblk *ioc;
        mblk_t *mp, *reply;
        toid_t tid = 0;
        int error, s, secs, expired = 0;

        if ((error = copyin((const void *)(uintptr_t)arg, &sio,
            sizeof(sio))) != 0)
                return error;
        if (sio.ic_len < 0 || sio.ic_len > STRIOCSZ ||
            sio.ic_timout < INFTIM)
                return EINVAL;
        secs = sio.ic_timout == 0 ? STRTIMOUT : sio.ic_timout;
        mp = allocb_wait(sizeof(*ioc));
        mp->b_datap->db_type = M_IOCTL;
        mp->b_wptr += sizeof(*ioc);
        ioc = (struct iocblk *)mp->b_rptr;
        memset(ioc, 0, sizeof(*ioc));
        ioc->ioc_cmd = sio.ic_cmd;
        ioc->ioc_cr = cr;
        ioc->ioc_count = (size_t)sio.ic_len;
        ioc->ioc_flag = flag;
        if (sio.ic_len > 0) {
                mblk_t *dp = allocb_wait((size_t)sio.ic_len);

                if ((error = copyin(sio.ic_dp, dp->b_wptr,
                    (size_t)sio.ic_len)) != 0) {
                        freemsg(mp);
                        freemsg(dp);
                        return error;
                }
                dp->b_wptr += sio.ic_len;
                mp->b_cont = dp;
        }

        /* One ioctl at a time on a stream. */
        mutex_lock(&sd->sd_iocwait);
        mutex_lock(&sd->sd_lock);
        error = str_werror(sd) != 0 ? str_werror(sd) : str_rerror(sd);
        mutex_unlock(&sd->sd_lock);
        if (error != 0) {
                mutex_unlock(&sd->sd_iocwait);
                freemsg(mp);
                return error;
        }
        s = spin_lock(&sd->sd_iocspin);
        ioc->ioc_id = sd->sd_iocid = atomic_inc_32_nv(&nextid);
        sd->sd_iocpending = 1;
        sd->sd_ioctimeout = 0;
        sd->sd_iocblk = NULL;
        spin_unlock(&sd->sd_iocspin, s);
        if (secs != INFTIM)
                tid = timeout(str_ioctimeout, sd, (long)secs * HZ);
        str_down(sd, mp);

        s = spin_lock(&sd->sd_iocspin);
        while (sd->sd_iocblk == NULL && !sd->sd_ioctimeout) {
                int r = sv_wait_sig(&sd->sd_iocsv, &sd->sd_iocspin, s);

                s = spin_lock(&sd->sd_iocspin);
                if (r != 0) {
                        error = sig_error();
                        break;
                }
        }
        reply = sd->sd_iocblk;
        expired = sd->sd_ioctimeout;
        sd->sd_iocblk = NULL;
        sd->sd_iocpending = 0;
        spin_unlock(&sd->sd_iocspin, s);
        if (tid != 0)
                (void)untimeout(tid);
        mutex_unlock(&sd->sd_iocwait);

        if (reply == NULL)
                return error != 0 ? error : (expired ? ETIME : EINVAL);
        if (error != 0) {
                freemsg(reply);
                return error;
        }
        ioc = (struct iocblk *)reply->b_rptr;
        if (DB_TYPE(reply) == M_IOCNAK) {
                error = ioc->ioc_error != 0 ? ioc->ioc_error : EINVAL;
        } else if (DB_TYPE(reply) != M_IOCACK) {
                error = EINVAL;         /* transparent replies: not yet */
        } else {
                size_t n = ioc->ioc_count, done = 0;

                if (n > STRIOCSZ)
                        n = STRIOCSZ;
                for (mblk_t *bp = reply->b_cont; bp != NULL && done < n &&
                    error == 0; bp = bp->b_cont) {
                        size_t k = MBLKL(bp);

                        if (k > n - done)
                                k = n - done;
                        error = copyout(bp->b_rptr, sio.ic_dp + done, k);
                        done += k;
                }
                sio.ic_len = (int)done;
                if (error == 0)
                        error = copyout(&sio.ic_len, (char *)(uintptr_t)arg +
                            offsetof(struct strioctl, ic_len),
                            sizeof(sio.ic_len));
                *rv = (uint64_t)(int64_t)ioc->ioc_rval;
        }
        freemsg(reply);
        return error;
}

static void
str_ioctimeout(void *arg)
{
        struct stdata *sd = arg;
        int s = spin_lock(&sd->sd_iocspin);

        sd->sd_ioctimeout = 1;
        sv_broadcast(&sd->sd_iocsv);
        spin_unlock(&sd->sd_iocspin, s);
}

/* The name of the module of the pair whose write queue is wq. */
static const char *
str_modname(queue_t *wq)
{
        return wq->q_qinfo->qi_minfo->mi_idname;
}

static int
str_flush(struct stdata *sd, int how, int band)
{
        mblk_t *mp;

        if ((how & ~FLUSHRW) != 0 || how == 0)
                return EINVAL;
        if ((mp = allocb_wait(2)) == NULL)
                return EAGAIN;
        mp->b_datap->db_type = M_FLUSH;
        *mp->b_wptr++ = (unsigned char)(how | (band >= 0 ? FLUSHBAND : 0));
        *mp->b_wptr++ = (unsigned char)(band >= 0 ? band : 0);
        if ((how & FLUSHR) != 0) {
                mutex_lock(&sd->sd_lock);
                if (band >= 0)
                        flushband(sd->sd_rq, (unsigned char)band, FLUSHALL);
                else
                        flushq(sd->sd_rq, FLUSHALL);
                mutex_unlock(&sd->sd_lock);
        }
        str_down(sd, mp);
        return 0;
}

/* I_PEEK: a copy of the first message, left where it is. */
static int
str_peek(struct stdata *sd, uint64_t arg, uint64_t *rv)
{
        struct strpeek pk;
        mblk_t *mp = NULL, *cp, *dp, **link;
        int error, s, morec, mored;

        if ((error = copyin((const void *)(uintptr_t)arg, &pk,
            sizeof(pk))) != 0)
                return error;
        if (pk.flags != 0 && pk.flags != RS_HIPRI)
                return EINVAL;
        s = spin_lock(&sd->sd_rq->q_lock);
        if (sd->sd_rq->q_first != NULL && (pk.flags == 0 ||
            DB_TYPE(sd->sd_rq->q_first) >= QPCTL))
                mp = dupmsg(sd->sd_rq->q_first);
        spin_unlock(&sd->sd_rq->q_lock, s);
        if (mp == NULL) {
                *rv = 0;
                return 0;
        }
        pk.flags = DB_TYPE(mp) >= QPCTL ? RS_HIPRI : 0;
        cp = NULL;
        dp = mp;
        link = &cp;
        while (dp != NULL && DB_TYPE(dp) != M_DATA) {
                *link = dp;
                link = &dp->b_cont;
                dp = dp->b_cont;
        }
        *link = NULL;
        error = str_copyout_part(cp, &pk.ctlbuf, pk.ctlbuf.maxlen >= 0,
            &morec);
        if (error == 0)
                error = str_copyout_part(dp, &pk.databuf,
                    pk.databuf.maxlen >= 0, &mored);
        freemsg(cp);
        freemsg(dp);
        if (error == 0)
                error = copyout(&pk, (void *)(uintptr_t)arg, sizeof(pk));
        *rv = 1;
        return error;
}

int
strioctl(struct vnode *vp, unsigned long cmd, uint64_t arg, int flag,
    struct cred *cr, uint64_t *rv)
{
        struct stdata *sd = vp->v_stream;
        char name[FMNAMESZ + 1];
        int error = 0, n, s;
        size_t len;

        *rv = 0;
        switch (cmd) {
        case I_NREAD: {
                mblk_t *mp;
                int count = 0, bytes = 0;

                s = spin_lock(&sd->sd_rq->q_lock);
                for (mp = sd->sd_rq->q_first; mp != NULL; mp = mp->b_next)
                        count++;
                if (sd->sd_rq->q_first != NULL)
                        bytes = (int)msgdsize(sd->sd_rq->q_first);
                spin_unlock(&sd->sd_rq->q_lock, s);
                *rv = (uint64_t)count;
                return copyout(&bytes, (void *)(uintptr_t)arg, sizeof(bytes));
        }
        case I_PUSH:
                if ((error = copyinstr((const char *)(uintptr_t)arg, name,
                    sizeof(name), &len)) != 0)
                        return error == ENAMETOOLONG ? EINVAL : error;
                return str_push(sd, name, flag, cr);
        case I_POP:
                return str_pop(sd, flag, cr);
        case I_LOOK:
                mutex_lock(&sd->sd_plumb);
                if (sd->sd_pushcnt == 0) {
                        mutex_unlock(&sd->sd_plumb);
                        return EINVAL;
                }
                memset(name, 0, sizeof(name));
                {
                        const char *mn = str_modname(sd->sd_wq->q_next);

                        for (size_t i = 0; i < FMNAMESZ && mn[i] != 0; i++)
                                name[i] = mn[i];
                }
                mutex_unlock(&sd->sd_plumb);
                return copyout(name, (void *)(uintptr_t)arg, sizeof(name));
        case I_FIND:
                if ((error = copyinstr((const char *)(uintptr_t)arg, name,
                    sizeof(name), &len)) != 0)
                        return error == ENAMETOOLONG ? EINVAL : error;
                if (fmod_find(name) == NULL)
                        return EINVAL;
                mutex_lock(&sd->sd_plumb);
                n = 0;
                {
                        queue_t *q = sd->sd_wq->q_next;

                        for (int i = 0; i < sd->sd_pushcnt && q != NULL;
                            i++, q = q->q_next)
                                if (strcmp(str_modname(q), name) == 0)
                                        n = 1;
                }
                mutex_unlock(&sd->sd_plumb);
                *rv = (uint64_t)n;
                return 0;
        case I_FLUSH:
                return str_flush(sd, (int)arg, -1);
        case I_FLUSHBAND: {
                struct bandinfo bi;

                if ((error = copyin((const void *)(uintptr_t)arg, &bi,
                    sizeof(bi))) != 0)
                        return error;
                return str_flush(sd, bi.bi_flag, bi.bi_pri);
        }
        case I_SRDOPT: {
                int mode = (int)arg & RMODEMASK, prot = (int)arg & RPROTMASK;

                if (((int)arg & ~(RMODEMASK | RPROTMASK)) != 0 ||
                    mode == RMODEMASK || (prot != 0 && prot != RPROTNORM &&
                    prot != RPROTDAT && prot != RPROTDIS))
                        return EINVAL;
                mutex_lock(&sd->sd_lock);
                sd->sd_rdopt = mode | (prot != 0 ? prot : RPROTNORM);
                mutex_unlock(&sd->sd_lock);
                return 0;
        }
        case I_GRDOPT:
                n = sd->sd_rdopt;
                return copyout(&n, (void *)(uintptr_t)arg, sizeof(n));
        case I_SWROPT:
                if (((int)arg & ~(SNDZERO | SNDPIPE)) != 0)
                        return EINVAL;
                sd->sd_wropt = (int)arg;
                return 0;
        case I_GWROPT:
                n = sd->sd_wropt;
                return copyout(&n, (void *)(uintptr_t)arg, sizeof(n));
        case I_STR:
                return str_istr(sd, arg, flag, cr, rv);
        case I_PEEK:
                return str_peek(sd, arg, rv);
        case I_CANPUT:
                if ((int64_t)arg < 0 || arg > 255)
                        return EINVAL;
                *rv = (uint64_t)str_canput(sd, (int)arg);
                return 0;
        case I_CKBAND:
        case I_GETBAND:
        case I_ATMARK: {
                mblk_t *mp;
                int found = 0, marks = 0, first = 0, band = -1;

                if (cmd == I_ATMARK && arg != ANYMARK && arg != LASTMARK)
                        return EINVAL;
                if (cmd == I_CKBAND && arg > 255)
                        return EINVAL;
                s = spin_lock(&sd->sd_rq->q_lock);
                for (mp = sd->sd_rq->q_first; mp != NULL; mp = mp->b_next) {
                        if (mp == sd->sd_rq->q_first) {
                                band = mp->b_band;
                                first = (mp->b_flag & MSGMARK) != 0;
                        }
                        if ((mp->b_flag & MSGMARK) != 0)
                                marks++;
                        if (DB_TYPE(mp) < QPCTL && mp->b_band == arg)
                                found = 1;
                }
                spin_unlock(&sd->sd_rq->q_lock, s);
                if (cmd == I_CKBAND) {
                        *rv = (uint64_t)found;
                        return 0;
                }
                if (cmd == I_GETBAND) {
                        if (band < 0)
                                return ENODATA;
                        return copyout(&band, (void *)(uintptr_t)arg,
                            sizeof(band));
                }
                *rv = (uint64_t)(first && (arg == ANYMARK || marks == 1));
                return 0;
        }
        case I_SETCLTIME:
                if ((int64_t)arg <= 0 || arg > INT32_MAX)
                        return EINVAL;
                sd->sd_closetime = (int)arg;
                return 0;
        case I_GETCLTIME:
                n = sd->sd_closetime;
                return copyout(&n, (void *)(uintptr_t)arg, sizeof(n));
        case I_LINK:
        case I_UNLINK:
        case I_PLINK:
        case I_PUNLINK:
        case I_LIST:
        case I_SENDFD:
        case I_RECVFD:
        case I_FDINSERT:
        default:
                return EINVAL;
        }
}

/* --- The stream's vnode. --- */

static int
str_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
str_vclose(struct vnode *vp, int flags, struct cred *cr __unused)
{
        struct stdata *sd = vp->v_stream;

        /* The last close's flags decide whether it waits for output. */
        sd->sd_closeflag = flags;
        return 0;
}

static int
str_inactive(struct vnode *vp, struct cred *cr)
{
        struct stdata *sd = vp->v_stream;

        str_close(sd, sd->sd_closeflag, cr);
        vn_destroy(vp);
        kmem_free(sd, sizeof(*sd));
        return 0;
}

static int
str_poll(struct vnode *vp, short events, short *revents,
    struct pollhead **ph, struct cred *cr __unused)
{
        struct stdata *sd = vp->v_stream;
        queue_t *rq = sd->sd_rq;
        short ready = 0;
        int s;

        if ((events & POLLOUT) != 0 && str_canput(sd, 0))
                ready |= POLLOUT;
        if ((events & POLLWRBAND) != 0 && str_canput(sd, 1))
                ready |= POLLWRBAND;
        mutex_lock(&sd->sd_lock);
        s = spin_lock(&rq->q_lock);
        for (mblk_t *mp = rq->q_first; mp != NULL; mp = mp->b_next) {
                if (DB_TYPE(mp) >= QPCTL) {
                        if (mp == rq->q_first)
                                ready |= POLLPRI;
                } else if (mp->b_band == 0) {
                        ready |= POLLIN | POLLRDNORM;
                } else {
                        ready |= POLLIN | POLLRDBAND;
                }
        }
        spin_unlock(&rq->q_lock, s);
        if ((sd->sd_flag & (STRDERR | STWRERR)) != 0)
                ready |= POLLERR;
        if ((sd->sd_flag & STRHUP) != 0) {
                ready |= POLLHUP;
                ready &= ~(POLLOUT | POLLWRBAND);
        }
        mutex_unlock(&sd->sd_lock);
        *revents = ready & (events | POLLERR | POLLHUP);
        *ph = &sd->sd_poll;
        return 0;
}

static int
str_vioctl(struct vnode *vp, unsigned long cmd, uint64_t arg, int flag,
    struct cred *cr)
{
        uint64_t rv;

        return strioctl(vp, cmd, arg, flag, cr, &rv);
}

static const struct vnodeops str_vnops = {
        str_open, str_vclose, str_read, str_write, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, str_inactive,
        spec_putpage, spec_reclaim, spec_getpage, str_poll, str_vioctl
};
