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
        struct pollhead *se_notify;    /* connected pathname socket's head */
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

struct sockpending {
        struct sockpending *sk_next;
        struct file     *sk_file;
        struct sockaddr_un sk_addr;
};

struct localsock {
        struct specattr ls_attr;
        struct vnode    ls_vnode;
        struct vnode    *ls_name;
        struct localsock *ls_next;
        struct sockaddr_un ls_addr;
        struct file     *ls_connection;
        struct pollhead ls_poll;
        struct sv       ls_wait;
        struct sockpending *ls_first;
        struct sockpending *ls_last;
        unsigned int    ls_backlog;
        unsigned int    ls_pending;
};

static const struct vnodeops sock_ops, local_ops;
static struct mutex local_lock;
static struct localsock *local_names;

void
sock_init(void)
{
        mutex_init(&local_lock, "socknames");
}

/* Once connected, the front file holds the data endpoint until close. */
static struct vnode *
sock_resolve(struct vnode *vp)
{
        if (vp->v_op == &local_ops) {
                struct localsock *local = vp->v_data;

                mutex_lock(&local_lock);
                vp = local->ls_connection != NULL ?
                    local->ls_connection->f_vnode : NULL;
                mutex_unlock(&local_lock);
        }
        return vp;
}

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
        if (end->se_notify != NULL)
                pollwakeup(end->se_notify);
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

        vp = sock_resolve(vp);
        if (vp == NULL)
                return ENOTCONN;
        if (vp->v_op != &sock_ops)
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

static int
sock_type(const uint64_t *args)
{
        int flags = SOCK_NONBLOCK | SOCK_CLOEXEC;

        if ((int)args[0] != AF_UNIX)
                return EAFNOSUPPORT;
        if (((int)args[1] & ~flags) != SOCK_STREAM)
                return EOPNOTSUPP;
        if ((int)args[2] != 0)
                return EPROTONOSUPPORT;
        return 0;
}

static int
sock_make_pair(int status, struct file *files[2])
{
        struct sockpair *pair = kmem_zalloc(sizeof(*pair), KM_NOSLEEP);

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
                files[i] = falloc(&end->se_vnode, FREAD | FWRITE | status);
        }
        return 0;
}

int
sys_socketpair(const uint64_t *args, uint64_t *rv)
{
        int type = (int)args[1], fds[2], error;
        struct file *files[2];

        if ((error = sock_type(args)) != 0)
                return error;
        if ((error = sock_make_pair((type & SOCK_NONBLOCK) != 0 ?
            FNONBLOCK : 0, files)) != 0)
                return error;
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
        struct vnode *vp;
        struct iovec iov = { (void *)(uintptr_t)args[1], (size_t)args[2] };
        struct uio uio = { .uio_iov = &iov, .uio_iovcnt = 1,
            .uio_segflg = UIO_USERSPACE, .uio_rw = rw, .uio_limit = UIO_NOLIMIT };
        int flags = (int)args[3], error;
        int valid = MSG_OOB | MSG_DONTWAIT |
            (rw == UIO_READ ? MSG_PEEK : MSG_NOSIGNAL);

        if (fp == NULL)
                return EBADF;
        vp = sock_resolve(fp->f_vnode);
        if (vp == NULL) {
                error = ENOTCONN;
        } else if (vp->v_op != &sock_ops) {
                error = ENOTSOCK;
        } else if ((flags & ~valid) != 0 || iov.iov_len > INT64_MAX) {
                error = EINVAL;
        } else {
                if ((fp->f_flag & FNONBLOCK) != 0)
                        flags |= MSG_DONTWAIT;
                uio.uio_resid = (ssize_t)iov.iov_len;
                uio_prefault(iov.iov_base, iov.iov_len, rw);
                error = sock_rw(vp, &uio, flags, rw);
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
        struct vnode *vp;

        if (fp == NULL)
                return EBADF;
        vp = sock_resolve(fp->f_vnode);
        if (vp == NULL || vp->v_op != &sock_ops) {
                frele(fp);
                return ENOTTY;
        }
        end = sock_end(vp);
        mutex_lock(&end->se_pair->sp_lock);
        *rv = end->se_marked && end->se_before == 0;
        mutex_unlock(&end->se_pair->sp_lock);
        frele(fp);
        return 0;
}

/* Local names are attached to filesystem vnodes, so rename and unlink
 * have the usual pathname semantics without rewriting socket state.
 */
static int
local_close(struct vnode *vp, int flags __unused, struct cred *cr __unused)
{
        struct localsock *local = vp->v_data;
        struct sockpending *pending;

        mutex_lock(&local_lock);
        if (local->ls_name != NULL) {
                struct localsock **link = &local_names;

                while (*link != local)
                        link = &(*link)->ls_next;
                *link = local->ls_next;
        }
        mutex_unlock(&local_lock);
        if (local->ls_name != NULL)
                VN_RELE(local->ls_name);
        if (local->ls_connection != NULL) {
                struct sockend *end = sock_end(local->ls_connection->f_vnode);

                mutex_lock(&end->se_pair->sp_lock);
                end->se_notify = NULL;
                mutex_unlock(&end->se_pair->sp_lock);
                frele(local->ls_connection);
        }
        while ((pending = local->ls_first) != NULL) {
                local->ls_first = pending->sk_next;
                frele(pending->sk_file);
                kmem_free(pending, sizeof(*pending));
        }
        return 0;
}

static int
local_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct localsock *local = vp->v_data;

        vn_destroy(vp);
        kmem_free(local, sizeof(*local));
        return 0;
}

static int
local_read(struct vnode *vp, struct uio *uio, int flags, struct cred *cr)
{
        struct vnode *endpoint = sock_resolve(vp);

        return endpoint != NULL ? sock_read(endpoint, uio, flags, cr) :
            ENOTCONN;
}

static int
local_write(struct vnode *vp, struct uio *uio, int flags, struct cred *cr)
{
        struct vnode *endpoint = sock_resolve(vp);

        return endpoint != NULL ? sock_write(endpoint, uio, flags, cr) :
            ENOTCONN;
}

static int
local_poll(struct vnode *vp, short events, short *out, struct pollhead **ph,
    struct cred *cr)
{
        struct localsock *local = vp->v_data;
        struct file *connection;

        mutex_lock(&local_lock);
        connection = local->ls_connection;
        *out = local->ls_first != NULL ? events & (POLLIN | POLLRDNORM) : 0;
        *ph = &local->ls_poll;
        mutex_unlock(&local_lock);
        if (connection != NULL) {
                struct pollhead *underlying;

                return sock_poll(connection->f_vnode, events, out, &underlying,
                    cr);
        }
        return 0;
}

static const struct vnodeops local_ops = {
        NULL, local_close, local_read, local_write, spec_getattr, spec_setattr,
        spec_access, spec_lookup, spec_create, spec_name1, spec_link,
        spec_rename, spec_mkdir, spec_name1, spec_readdir, spec_symlink,
        spec_readlink, spec_fsync, local_inactive, spec_putpage, spec_reclaim,
        spec_getpage, local_poll, NULL
};

int
sys_socket(const uint64_t *args, uint64_t *rv)
{
        struct localsock *local;
        struct file *fp;
        int type = (int)args[1], fd, error;

        if ((error = sock_type(args)) != 0)
                return error;
        local = kmem_zalloc(sizeof(*local), KM_NOSLEEP);
        if (local == NULL)
                return ENOMEM;
        pollhead_init(&local->ls_poll);
        sv_init(&local->ls_wait, "listen");
        local->ls_addr.sun_family = AF_UNIX;
        specattr_init(&local->ls_attr, 0600, CRED()->cr_uid, CRED()->cr_gid, 0);
        vn_init(&local->ls_vnode, NULL, VSOCK, &local_ops, &local->ls_attr);
        VN_HOLD(&local->ls_vnode);
        fp = falloc(&local->ls_vnode, FREAD | FWRITE |
            ((type & SOCK_NONBLOCK) != 0 ? FNONBLOCK : 0));
        error = fd_alloc_flags(curproc, fp, 0,
            (type & SOCK_CLOEXEC) != 0 ? FD_CLOEXEC : 0, &fd);
        if (error != 0) {
                frele(fp);
                return error;
        }
        *rv = (uint64_t)fd;
        return 0;
}

static int
local_get(int fd, struct file **fpp, struct localsock **out)
{
        struct file *fp = fd_get(curproc, fd);
        int error;

        if (fp == NULL)
                return EBADF;
        if (fp->f_vnode->v_op == &local_ops) {
                *fpp = fp;
                *out = fp->f_vnode->v_data;
                return 0;
        }
        error = fp->f_vnode->v_op == &sock_ops ? EISCONN : ENOTSOCK;
        frele(fp);
        return error;
}

static int
local_address(uint64_t address, uint64_t length, struct sockaddr_un *out)
{
        int error;

        if (length <= offsetof(struct sockaddr_un, sun_path) ||
            length > sizeof(*out))
                return EINVAL;
        memset(out, 0, sizeof(*out));
        error = copyin((const void *)(uintptr_t)address, out, (size_t)length);
        if (error != 0)
                return error;
        if (out->sun_family != AF_UNIX)
                return EAFNOSUPPORT;
        if (out->sun_path[0] == '\0')
                return EINVAL;
        for (size_t i = 0; i < sizeof(out->sun_path); i++)
                if (out->sun_path[i] == '\0')
                        return 0;
        return ENAMETOOLONG;
}

int
sys_bind(const uint64_t *args, uint64_t *rv __unused)
{
        struct sockaddr_un addr;
        struct localsock *local;
        struct file *fp;
        int error;

        if ((error = local_get((int)args[0], &fp, &local)) != 0)
                return error;
        if ((error = local_address(args[1], args[2], &addr)) == 0) {
                mutex_lock(&local_lock);
                if (local->ls_name != NULL || local->ls_connection != NULL) {
                        error = EINVAL;
                } else {
                        error = vn_mksock(addr.sun_path, &local->ls_name);
                        if (error == EEXIST)
                                error = EADDRINUSE;
                        if (error == 0) {
                                local->ls_addr = addr;
                                local->ls_next = local_names;
                                local_names = local;
                        }
                }
                mutex_unlock(&local_lock);
        }
        frele(fp);
        return error;
}

int
sys_listen(const uint64_t *args, uint64_t *rv __unused)
{
        struct localsock *local;
        struct file *fp;
        int backlog = (int)args[1], error;

        if ((error = local_get((int)args[0], &fp, &local)) != 0)
                return error;
        mutex_lock(&local_lock);
        if (local->ls_name == NULL || local->ls_connection != NULL)
                error = EINVAL;
        else
                local->ls_backlog = backlog < 1 ? 1 : backlog > 4096 ? 4096 :
                    (unsigned int)backlog;
        mutex_unlock(&local_lock);
        frele(fp);
        return error;
}

static int
local_connect(struct localsock *client, struct vnode *name)
{
        struct sockpending *pending;
        struct localsock *server;
        struct file *files[2];
        int error;

        if (client->ls_connection != NULL)
                return EISCONN;
        if (client->ls_backlog != 0)
                return EINVAL;
        for (server = local_names; server != NULL; server = server->ls_next)
                if (server->ls_name == name)
                        break;
        if (server == NULL || server->ls_backlog == 0 ||
            server->ls_pending >= server->ls_backlog)
                return ECONNREFUSED;
        pending = kmem_zalloc(sizeof(*pending), KM_NOSLEEP);
        if (pending == NULL)
                return ENOMEM;
        error = sock_make_pair(0, files);
        if (error != 0) {
                kmem_free(pending, sizeof(*pending));
                return error;
        }
        client->ls_connection = files[0];
        sock_end(files[0]->f_vnode)->se_notify = &client->ls_poll;
        pending->sk_file = files[1];
        pending->sk_addr = client->ls_addr;
        if (server->ls_last == NULL)
                server->ls_first = pending;
        else
                server->ls_last->sk_next = pending;
        server->ls_last = pending;
        server->ls_pending++;
        pollwakeup(&client->ls_poll);
        pollwakeup(&server->ls_poll);
        sv_broadcast(&server->ls_wait);
        return 0;
}

int
sys_connect(const uint64_t *args, uint64_t *rv __unused)
{
        struct sockaddr_un addr;
        struct localsock *client;
        struct vnode *name;
        struct file *fp;
        int error;

        if ((error = local_get((int)args[0], &fp, &client)) != 0)
                return error;
        if ((error = local_address(args[1], args[2], &addr)) == 0 &&
            (error = lookupname(addr.sun_path, FOLLOW, &name)) == 0) {
                if (name->v_type != VSOCK) {
                        error = ECONNREFUSED;
                } else if ((error = VOP_ACCESS(name, VWRITE, CRED())) == 0) {
                        mutex_lock(&local_lock);
                        error = local_connect(client, name);
                        mutex_unlock(&local_lock);
                }
                VN_RELE(name);
        }
        frele(fp);
        return error;
}

/* local_lock held, and both endpoints remain unpublished on failure. */
static int
local_accept(struct localsock *server, int flags, uint64_t addr,
    uint64_t size, int *fd)
{
        struct sockpending *pending = server->ls_first;
        int error;

        if (addr != 0) {
                uint32_t room, length = offsetof(struct sockaddr_un, sun_path);

                error = copyin((const void *)(uintptr_t)size, &room,
                    sizeof(room));
                if (error != 0)
                        return error;
                if (pending->sk_addr.sun_path[0] != '\0')
                        length += (uint32_t)strlen(pending->sk_addr.sun_path) + 1;
                if (room > length)
                        room = length;
                error = copyout(&pending->sk_addr, (void *)(uintptr_t)addr, room);
                if (error == 0)
                        error = copyout(&length, (void *)(uintptr_t)size,
                            sizeof(length));
                if (error != 0)
                        return error;
        }
        pending->sk_file->f_flag = FREAD | FWRITE |
            ((flags & SOCK_NONBLOCK) != 0 ? FNONBLOCK : 0);
        crfree(pending->sk_file->f_cred);
        pending->sk_file->f_cred = CRED();
        crhold(pending->sk_file->f_cred);
        error = fd_alloc_flags(curproc, pending->sk_file, 0,
            (flags & SOCK_CLOEXEC) != 0 ? FD_CLOEXEC : 0, fd);
        if (error != 0)
                return error;
        server->ls_first = pending->sk_next;
        if (server->ls_first == NULL)
                server->ls_last = NULL;
        server->ls_pending--;
        kmem_free(pending, sizeof(*pending));
        return 0;
}

int
sys_accept(const uint64_t *args, uint64_t *rv)
{
        struct localsock *server;
        struct file *fp;
        int flags = (int)args[3], fd, error;

        if ((flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != 0)
                return EINVAL;
        if ((error = local_get((int)args[0], &fp, &server)) != 0)
                return error;
        mutex_lock(&local_lock);
        if (server->ls_backlog == 0)
                error = EINVAL;
        while (error == 0 && server->ls_first == NULL) {
                int action;

                if ((fp->f_flag & FNONBLOCK) != 0) {
                        error = EAGAIN;
                        break;
                }
                action = sv_wait_mutex_sig(&server->ls_wait, &local_lock) != 0 ?
                    sig_sleep_check() : 0;
                mutex_lock(&local_lock);
                if (action != 0)
                        error = action == 2 ? ERESTART : sig_restart(EINTR);
        }
        if (error == 0)
                error = local_accept(server, flags, args[1], args[2], &fd);
        mutex_unlock(&local_lock);
        frele(fp);
        if (error == 0)
                *rv = (uint64_t)fd;
        return error;
}
