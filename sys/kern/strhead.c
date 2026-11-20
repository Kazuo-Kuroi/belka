/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/* STREAMS signal registration and the initial loopback endpoint. */

#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/mkdev.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/specvn.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define STRLOOP_SIZE    4096
#define STRSIG_EVENTS   (S_INPUT | S_HIPRI | S_OUTPUT | S_MSG | S_ERROR | \
    S_HANGUP | S_RDNORM | S_RDBAND | S_WRBAND | S_BANDURG)

struct strsig {
        struct strsig   *ss_next;
        struct proc     *ss_proc;       /* detached on descriptor close */
        int             ss_events;
};

struct stdata {
        struct mutex    sd_lock;
        struct strsig   *sd_signals;
};

struct strloop {
        struct stdata   sl_stream;
        struct specattr sl_attr;
        struct vnode    sl_vnode;
        struct pollhead sl_poll;
        struct sv       sl_readable;
        struct sv       sl_writable;
        size_t          sl_head;
        size_t          sl_count;
        char            sl_data[STRLOOP_SIZE];
};

static const struct vnodeops strloop_ops;

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

/* A band-zero event, with the stream locked through signal posting. */
static void
strsig_notify(struct stdata *st, int events, int code)
{
        siginfo_t info = { .si_signo = SIGPOLL, .si_code = code };

        for (struct strsig *ss = st->sd_signals; ss != NULL; ss = ss->ss_next)
                if ((ss->ss_events & events) != 0)
                        sigtoproc(ss->ss_proc, SIGPOLL, &info);
}

int
strloop_clone(struct vnode **vpp, struct cred *cr)
{
        struct strloop *loop = kmem_zalloc(sizeof(*loop), KM_NOSLEEP);

        if (loop == NULL)
                return ENOSR;
        mutex_init(&loop->sl_stream.sd_lock, "stream");
        sv_init(&loop->sl_readable, "strread");
        sv_init(&loop->sl_writable, "strwrite");
        pollhead_init(&loop->sl_poll);
        specattr_init(&loop->sl_attr, 0600, cr->cr_uid, cr->cr_gid,
            makedev(DEV_STRLOOP_MAJOR, 0));
        vn_init(&loop->sl_vnode, (*vpp)->v_vfsp, VCHR, &strloop_ops,
            &loop->sl_attr);
        loop->sl_vnode.v_stream = &loop->sl_stream;
        VN_HOLD(&loop->sl_vnode);
        VN_RELE(*vpp);
        *vpp = &loop->sl_vnode;
        return 0;
}

static int
strloop_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
strloop_close(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
strloop_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct strloop *loop = (struct strloop *)vp->v_stream;

        vn_destroy(vp);
        kmem_free(loop, sizeof(*loop));
        return 0;
}

static int
strloop_rw(struct vnode *vp, struct uio *uio, int flags, enum uio_rw rw)
{
        struct strloop *loop = (struct strloop *)vp->v_stream;
        struct mutex *lock = &loop->sl_stream.sd_lock;
        int reading = rw == UIO_READ, error = 0;

        mutex_lock(lock);
        while (uio->uio_resid > 0) {
                size_t count = loop->sl_count;
                size_t avail = reading ? count : STRLOOP_SIZE - count;
                size_t pos, n;

                if (avail == 0) {
                        struct sv *sv = reading ? &loop->sl_readable :
                            &loop->sl_writable;

                        if ((flags & (IO_NONBLOCK | IO_NDELAY)) != 0) {
                                error = (flags & IO_NONBLOCK) != 0 ? EAGAIN : 0;
                                break;
                        }
                        error = sv_wait_mutex_sig(sv, lock) != 0 ?
                            sig_sleep_check() : 0;
                        mutex_lock(lock);
                        if (error != 0) {
                                error = error == 2 ? ERESTART : sig_restart(EINTR);
                                break;
                        }
                        continue;
                }
                pos = reading ? loop->sl_head :
                    (loop->sl_head + count) % STRLOOP_SIZE;
                n = (size_t)uio->uio_resid;
                if (n > avail)
                        n = avail;
                if (n > STRLOOP_SIZE - pos)
                        n = STRLOOP_SIZE - pos;
                ssize_t resid = uio->uio_resid;

                error = uiomove(loop->sl_data + pos, n, rw, uio);
                n = (size_t)(resid - uio->uio_resid);
                if (reading) {
                        loop->sl_head = (pos + n) % STRLOOP_SIZE;
                        loop->sl_count -= n;
                        if (n != 0 && count == STRLOOP_SIZE) {
                                sv_broadcast(&loop->sl_writable);
                                pollwakeup(&loop->sl_poll);
                                strsig_notify(&loop->sl_stream, S_OUTPUT, POLL_OUT);
                        }
                } else {
                        loop->sl_count += n;
                        if (n != 0 && count == 0) {
                                sv_broadcast(&loop->sl_readable);
                                pollwakeup(&loop->sl_poll);
                                strsig_notify(&loop->sl_stream, S_INPUT | S_RDNORM,
                                    POLL_IN);
                        }
                }
                if (error != 0 || (reading && loop->sl_count == 0))
                        break;
        }
        mutex_unlock(lock);
        return error;
}

static int
strloop_read(struct vnode *vp, struct uio *uio, int flags,
    struct cred *cr __unused)
{
        return strloop_rw(vp, uio, flags, UIO_READ);
}

static int
strloop_write(struct vnode *vp, struct uio *uio, int flags,
    struct cred *cr __unused)
{
        return strloop_rw(vp, uio, flags, UIO_WRITE);
}

static int
strloop_poll(struct vnode *vp, short events, short *revents,
    struct pollhead **ph, struct cred *cr __unused)
{
        struct strloop *loop = (struct strloop *)vp->v_stream;
        short ready = 0;

        mutex_lock(&loop->sl_stream.sd_lock);
        if (loop->sl_count != 0)
                ready |= POLLIN | POLLRDNORM;
        if (loop->sl_count < STRLOOP_SIZE)
                ready |= POLLOUT;
        mutex_unlock(&loop->sl_stream.sd_lock);
        *revents = ready & events;
        *ph = &loop->sl_poll;
        return 0;
}

static const struct vnodeops strloop_ops = {
        strloop_open, strloop_close, strloop_read, strloop_write, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, strloop_inactive,
        spec_putpage, spec_reclaim, spec_getpage, strloop_poll, NULL
};
