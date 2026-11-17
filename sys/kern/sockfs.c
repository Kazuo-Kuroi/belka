/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/* Connected local stream sockets and their urgent data. */

#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/specvn.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define SOCK_BUFSIZE    4096

struct sockpair;

struct sockend {
        struct specattr se_attr;
        struct vnode    se_vnode;
        struct sockpair *se_pair;
        struct sockend  *se_peer;
        struct sockowner se_owner;
        struct pollhead se_poll;
        struct sv       se_wait;
        size_t          se_head;
        size_t          se_count;
        size_t          se_before;      /* normal bytes preceding the mark */
        int             se_closed;
        int             se_marked;
        int             se_urgent;
        char            se_oob;
        char            se_data[SOCK_BUFSIZE];
};

struct sockpair {
        struct mutex    sp_lock;
        unsigned int    sp_refs;
        struct sockend  sp_end[2];
};

static const struct vnodeops sock_ops;

/* The attributes are the first member so the spec operations share them. */
static struct sockend *
sock_end(struct vnode *vp)
{
        return vp->v_data;
}

static void
sock_wakeup(struct sockend *end)
{
        sv_broadcast(&end->se_wait);
        pollwakeup(&end->se_poll);
}

static int
sock_sleep(struct sockend *end, int flags)
{
        struct mutex *lock = &end->se_pair->sp_lock;
        int action;

        if ((flags & MSG_DONTWAIT) != 0)
                return EAGAIN;
        action = sv_wait_mutex_sig(&end->se_wait, lock) != 0 ?
            sig_sleep_check() : 0;
        mutex_lock(lock);
        return action == 0 ? 0 : action == 2 ? ERESTART : sig_restart(EINTR);
}

int
sock_fcntl(struct vnode *vp, int cmd, int arg, uint64_t *rv)
{
        struct sockowner owner, old;
        struct sockend *end;
        int error;

        if (vp->v_type != VSOCK || vp->v_op != &sock_ops)
                return ENOTSOCK;
        end = sock_end(vp);
        if (cmd == F_SETOWN &&
            (error = sockowner_set(&owner, (pid_t)arg, CRED())) != 0)
                return error;
        mutex_lock(&end->se_pair->sp_lock);
        if (cmd == F_GETOWN) {
                *rv = (uint64_t)end->se_owner.so_id;
                mutex_unlock(&end->se_pair->sp_lock);
                return 0;
        }
        old = end->se_owner;
        end->se_owner = owner;
        mutex_unlock(&end->se_pair->sp_lock);
        if (old.so_cred != NULL)
                crfree(old.so_cred);
        return 0;
}

/* The last file at this endpoint closed. The peer can drain its queue. */
static int
sock_close(struct vnode *vp, int flags __unused, struct cred *cr __unused)
{
        struct sockend *end = sock_end(vp);

        mutex_lock(&end->se_pair->sp_lock);
        end->se_closed = 1;
        sock_wakeup(end->se_peer);
        mutex_unlock(&end->se_pair->sp_lock);
        return 0;
}

static int
sock_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct sockend *end = sock_end(vp);
        struct sockpair *pair = end->se_pair;
        int last;

        if (end->se_owner.so_cred != NULL)
                crfree(end->se_owner.so_cred);
        vn_destroy(vp);
        mutex_lock(&pair->sp_lock);
        last = --pair->sp_refs == 0;
        mutex_unlock(&pair->sp_lock);
        if (last)
                kmem_free(pair, sizeof(*pair));
        return 0;
}

static int
sock_read_data(struct sockend *end, struct uio *uio, int flags)
{
        size_t n, pos;
        int error;

        if ((flags & MSG_OOB) != 0) {
                if (!end->se_urgent)
                        return EAGAIN;
                error = uiomove(&end->se_oob, 1, UIO_READ, uio);
                if (error == 0 && (flags & MSG_PEEK) == 0)
                        end->se_urgent = 0;
                return error;
        }
        while (end->se_count == 0) {
                if (end->se_peer->se_closed)
                        return 0;
                if ((error = sock_sleep(end, flags)) != 0)
                        return error;
        }
        pos = end->se_head;
        n = end->se_count;
        if (n > (size_t)uio->uio_resid)
                n = (size_t)uio->uio_resid;
        if (end->se_marked && end->se_before != 0 && n > end->se_before)
                n = end->se_before;
        ssize_t resid = uio->uio_resid;
        size_t first = SOCK_BUFSIZE - pos;

        if (first > n)
                first = n;
        error = uiomove(end->se_data + pos, first, UIO_READ, uio);
        if (error == 0 && n > first)
                error = uiomove(end->se_data, n - first, UIO_READ, uio);
        n = (size_t)(resid - uio->uio_resid);
        if (n != 0 && (flags & MSG_PEEK) == 0) {
                end->se_head = (pos + n) % SOCK_BUFSIZE;
                end->se_count -= n;
                if (end->se_marked) {
                        if (end->se_before == 0)
                                end->se_marked = 0;
                        else
                                end->se_before -= n;
                }
                sock_wakeup(end->se_peer);
        }
        return error;
}

static int
sock_write_data(struct sockend *end, struct uio *uio, int flags)
{
        struct sockend *peer = end->se_peer;
        int error = 0;

        while (uio->uio_resid > 0) {
                size_t pos, n;
                ssize_t resid;

                if (peer->se_closed)
                        return EPIPE;
                if ((flags & MSG_OOB) != 0) {
                        char byte;

                        if (uio->uio_resid != 1)
                                return EMSGSIZE;
                        if ((error = uiomove(&byte, 1, UIO_WRITE, uio)) != 0)
                                return error;
                        peer->se_oob = byte;
                        peer->se_urgent = peer->se_marked = 1;
                        peer->se_before = peer->se_count;
                        sock_wakeup(peer);
                        sockowner_signal(&peer->se_owner);
                        return 0;
                }
                if (peer->se_count == SOCK_BUFSIZE) {
                        if ((error = sock_sleep(end, flags)) != 0)
                                return error;
                        continue;
                }
                pos = (peer->se_head + peer->se_count) % SOCK_BUFSIZE;
                n = SOCK_BUFSIZE - peer->se_count;
                if (n > SOCK_BUFSIZE - pos)
                        n = SOCK_BUFSIZE - pos;
                if (n > (size_t)uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                resid = uio->uio_resid;
                error = uiomove(peer->se_data + pos, n, UIO_WRITE, uio);
                n = (size_t)(resid - uio->uio_resid);
                if (n != 0) {
                        peer->se_count += n;
                        sock_wakeup(peer);
                }
                if (error != 0)
                        break;
        }
        return error;
}

static int
sock_rw(struct vnode *vp, struct uio *uio, int flags, enum uio_rw rw)
{
        struct sockend *end = sock_end(vp);
        ssize_t resid = uio->uio_resid;
        int error;

        if (resid == 0)
                return 0;
        mutex_lock(&end->se_pair->sp_lock);
        error = rw == UIO_READ ? sock_read_data(end, uio, flags) :
            sock_write_data(end, uio, flags);
        mutex_unlock(&end->se_pair->sp_lock);
        if (error == EPIPE && uio->uio_resid == resid &&
            (flags & MSG_NOSIGNAL) == 0) {
                struct proc *p = curproc;
                int s = spin_lock(&p->p_siglock);

                sigtolwp_locked(p, curlwp, SIGPIPE, NULL);
                spin_unlock(&p->p_siglock, s);
        }
        return error;
}

static int
sock_read(struct vnode *vp, struct uio *uio, int flags, struct cred *cr __unused)
{
        return sock_rw(vp, uio, (flags & IO_NONBLOCK) != 0 ? MSG_DONTWAIT : 0,
            UIO_READ);
}

static int
sock_write(struct vnode *vp, struct uio *uio, int flags, struct cred *cr __unused)
{
        return sock_rw(vp, uio, (flags & IO_NONBLOCK) != 0 ? MSG_DONTWAIT : 0,
            UIO_WRITE);
}

static int
sock_poll(struct vnode *vp, short events, short *out, struct pollhead **ph,
    struct cred *cr __unused)
{
        struct sockend *end = sock_end(vp);
        short ready = 0;

        mutex_lock(&end->se_pair->sp_lock);
        if (end->se_count != 0)
                ready |= POLLIN | POLLRDNORM;
        if (end->se_marked || end->se_urgent)
                ready |= POLLPRI;
        if (end->se_peer->se_closed)
                ready |= POLLHUP;
        else if (end->se_peer->se_count != SOCK_BUFSIZE)
                ready |= POLLOUT;
        mutex_unlock(&end->se_pair->sp_lock);
        *out = ready & (events | POLLHUP);
        *ph = &end->se_poll;
        return 0;
}

static const struct vnodeops sock_ops = {
        NULL, sock_close, sock_read, sock_write, spec_getattr, spec_setattr,
        spec_access, spec_lookup, spec_create, spec_name1, spec_link,
        spec_rename, spec_mkdir, spec_name1, spec_readdir, spec_symlink,
        spec_readlink, spec_fsync, sock_inactive, spec_putpage, spec_reclaim,
        spec_getpage, sock_poll, NULL
};

int
sys_socketpair(const uint64_t *args, uint64_t *rv)
{
        int type = (int)args[1], flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        struct file *files[2];
        struct sockpair *pair;
        int fds[2], error;

        if ((int)args[0] != AF_UNIX)
                return EAFNOSUPPORT;
        if ((type & ~flags) != SOCK_STREAM)
                return EOPNOTSUPP;
        if ((int)args[2] != 0)
                return EPROTONOSUPPORT;
        pair = kmem_zalloc(sizeof(*pair), KM_NOSLEEP);
        if (pair == NULL)
                return ENOMEM;
        mutex_init(&pair->sp_lock, "sockpair");
        pair->sp_refs = 2;
        for (int i = 0; i < 2; i++) {
                struct sockend *end = &pair->sp_end[i];

                end->se_pair = pair;
                end->se_peer = &pair->sp_end[1 - i];
                sv_init(&end->se_wait, "socket");
                pollhead_init(&end->se_poll);
                specattr_init(&end->se_attr, 0600, CRED()->cr_uid,
                    CRED()->cr_gid, 0);
                vn_init(&end->se_vnode, NULL, VSOCK, &sock_ops, &end->se_attr);
                VN_HOLD(&end->se_vnode);
                files[i] = falloc(&end->se_vnode, FREAD | FWRITE |
                    ((type & SOCK_NONBLOCK) != 0 ? FNONBLOCK : 0));
        }
        error = fd_alloc_pair(curproc, files,
            (type & SOCK_CLOEXEC) != 0 ? FD_CLOEXEC : 0, fds);
        if (error != 0) {
                frele(files[0]);
                frele(files[1]);
                return error;
        }
        rv[0] = (uint64_t)fds[0];
        rv[1] = (uint64_t)fds[1];
        return 0;
}

static int
sock_sys_rw(const uint64_t *args, uint64_t *rv, enum uio_rw rw)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        struct iovec iov = { (void *)(uintptr_t)args[1], (size_t)args[2] };
        struct uio uio = { .uio_iov = &iov, .uio_iovcnt = 1,
            .uio_segflg = UIO_USERSPACE, .uio_rw = rw, .uio_limit = UIO_NOLIMIT };
        int flags = (int)args[3], error;
        int valid = MSG_OOB | MSG_DONTWAIT |
            (rw == UIO_READ ? MSG_PEEK : MSG_NOSIGNAL);

        if (fp == NULL)
                return EBADF;
        if (fp->f_vnode->v_op != &sock_ops) {
                error = ENOTSOCK;
        } else if ((flags & ~valid) != 0 || iov.iov_len > INT64_MAX) {
                error = EINVAL;
        } else {
                if ((fp->f_flag & FNONBLOCK) != 0)
                        flags |= MSG_DONTWAIT;
                uio.uio_resid = (ssize_t)iov.iov_len;
                uio_prefault(iov.iov_base, iov.iov_len, rw);
                error = sock_rw(fp->f_vnode, &uio, flags, rw);
                *rv = args[2] - (uint64_t)uio.uio_resid;
                if (*rv != 0)
                        error = 0;
        }
        frele(fp);
        return error;
}

int
sys_send(const uint64_t *args, uint64_t *rv)
{
        return sock_sys_rw(args, rv, UIO_WRITE);
}

int
sys_recv(const uint64_t *args, uint64_t *rv)
{
        return sock_sys_rw(args, rv, UIO_READ);
}

int
sys_sockatmark(const uint64_t *args, uint64_t *rv)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        struct sockend *end;

        if (fp == NULL)
                return EBADF;
        if (fp->f_vnode->v_op != &sock_ops) {
                frele(fp);
                return ENOTTY;
        }
        end = sock_end(fp->f_vnode);
        mutex_lock(&end->se_pair->sp_lock);
        *rv = end->se_marked && end->se_before == 0;
        mutex_unlock(&end->se_pair->sp_lock);
        frele(fp);
        return 0;
}
