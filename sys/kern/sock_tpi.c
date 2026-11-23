/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * Sockets over TPI providers: socket(AF_INET, SOCK_DGRAM) opens a
 * stream on /dev/udp, and the socket calls become TPI messages on it
 * (<sys/tihdr.h>): bind a T_BIND_REQ, connect a T_CONN_REQ, sendto a
 * T_UNITDATA_REQ, and recvfrom takes a T_UNITDATA_IND. Requests are one
 * at a time per socket (ts_lock), each waiting for its acknowledgement,
 * the one high priority message at the head; data is the head's
 * ordinary messages, which readers take whole. See doc/design/
 * decisions.md, "UDP and sockets over TPI".
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/specvn.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/systm.h>
#include <sys/tihdr.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <netinet/in.h>

#define TPI_REQSZ       64      /* request blocks, big enough for replies */
#define TPI_MAXDGRAM    (65535 - 20 - 8)

/* The providers, by the family, type and protocol that name them. */
static const struct tpiproto {
        int             tp_family;
        int             tp_type;
        int             tp_proto;
        const char      *tp_dev;
} tpiprotos[] = {
        { AF_INET, SOCK_DGRAM, IPPROTO_UDP, "udp" },
};

struct tpisock {
        struct specattr ts_attr;        /* first, for the spec operations */
        struct vnode    ts_vnode;
        struct vnode    *ts_stream;     /* held */
        struct stdata   *ts_sd;
        struct mutex    ts_lock;        /* one request at a time; below */
        int             ts_bound;
        int             ts_conn;
        struct sockaddr_in ts_local;
        struct sockaddr_in ts_peer;
};

static const struct vnodeops tpi_ops;

int
tpisock_is(const struct vnode *vp)
{
        return vp->v_op == &tpi_ops;
}

/* --- Requests and their acknowledgements. --- */

/* A request block for prim, of size bytes (an address after them). */
static mblk_t *
tpi_reqblk(size_t size)
{
        mblk_t *mp = allocb_wait(size > TPI_REQSZ ? size : TPI_REQSZ);

        mp->b_datap->db_type = M_PROTO;
        memset(mp->b_wptr, 0, size);
        mp->b_wptr += size;
        return mp;
}

static int
tpi_errno(const struct T_error_ack *ea)
{
        switch (ea->TLI_error) {
        case TSYSERR:
                return ea->UNIX_error != 0 ? ea->UNIX_error : EPROTO;
        case TADDRBUSY:
                return EADDRINUSE;
        case TACCES:
                return EACCES;
        case TBADADDR:
        case TOUTSTATE:
                return EINVAL;
        case TNOADDR:
                return EADDRNOTAVAIL;
        case TNOTSUPPORT:
                return EOPNOTSUPP;
        default:
                return EPROTO;
        }
}

/*
 * ts_lock held: send req (freed), and wait for its acknowledgement:
 * want, in *ackp, or T_ERROR_ACK's error.
 */
static int
tpi_request(struct tpisock *ts, mblk_t *req, t_scalar_t want, mblk_t **ackp)
{
        struct stdata *sd = ts->ts_sd;
        union T_primitives pr;
        mblk_t *mp;
        int error;

        *ackp = NULL;
        /* An acknowledgement a signal stopped us waiting for: discarded. */
        while (str_kgetmsg(sd, 1, IO_NONBLOCK, 0, &mp) == 0 && mp != NULL)
                freemsg(mp);
        if ((error = str_kputmsg(sd, req, 0)) != 0)
                return error;
        if ((error = str_kgetmsg(sd, 1, 0, 0, &mp)) != 0)
                return error;
        if (mp == NULL)
                return ENXIO;           /* hung up */
        memset(&pr, 0, sizeof(pr));
        memcpy(&pr, mp->b_rptr, MBLKL(mp) < sizeof(pr) ? MBLKL(mp) :
            sizeof(pr));
        if (pr.type == T_ERROR_ACK && MBLKL(mp) >= sizeof(pr.error_ack)) {
                error = tpi_errno(&pr.error_ack);
                freemsg(mp);
                return error;
        }
        if (pr.type != want) {
                freemsg(mp);
                return EPROTO;
        }
        *ackp = mp;
        return 0;
}

/* The address len bytes at off in mp's first block, if it is one. */
static int
tpi_getaddr(const mblk_t *mp, t_scalar_t len, t_scalar_t off,
    struct sockaddr_in *sin)
{
        size_t have = MBLKL(mp);

        memset(sin, 0, sizeof(*sin));
        if (len != sizeof(*sin) || off < 0 || (size_t)off > have ||
            (size_t)len > have - (size_t)off)
                return EPROTO;
        memcpy(sin, mp->b_rptr + off, sizeof(*sin));
        return 0;
}

/* ts_lock held: bind to sin, or (NULL) to whatever port is free. */
static int
tpi_bind(struct tpisock *ts, const struct sockaddr_in *sin)
{
        struct T_bind_req br;
        struct T_bind_ack ba;
        mblk_t *req, *ack;
        int error;

        memset(&br, 0, sizeof(br));
        br.PRIM_type = T_BIND_REQ;
        if (sin != NULL) {
                br.ADDR_length = sizeof(*sin);
                br.ADDR_offset = sizeof(br);
        }
        req = tpi_reqblk(sizeof(br) + sizeof(*sin));
        memcpy(req->b_rptr, &br, sizeof(br));
        if (sin != NULL)
                memcpy(req->b_rptr + sizeof(br), sin, sizeof(*sin));
        if ((error = tpi_request(ts, req, T_BIND_ACK, &ack)) != 0)
                return error;
        if (MBLKL(ack) < sizeof(ba)) {
                freemsg(ack);
                return EPROTO;
        }
        memcpy(&ba, ack->b_rptr, sizeof(ba));
        error = tpi_getaddr(ack, ba.ADDR_length, ba.ADDR_offset,
            &ts->ts_local);
        freemsg(ack);
        if (error == 0)
                ts->ts_bound = 1;
        return error;
}

/* ts_lock held: a request answered by T_OK_ACK. */
static int
tpi_okreq(struct tpisock *ts, mblk_t *req, t_scalar_t prim)
{
        struct T_ok_ack ok;
        mblk_t *ack;
        int error;

        if ((error = tpi_request(ts, req, T_OK_ACK, &ack)) != 0)
                return error;
        memset(&ok, 0, sizeof(ok));
        memcpy(&ok, ack->b_rptr, MBLKL(ack) < sizeof(ok) ? MBLKL(ack) :
            sizeof(ok));
        freemsg(ack);
        return ok.CORRECT_prim == prim ? 0 : EPROTO;
}

static int
tpi_disconnect(struct tpisock *ts)
{
        struct T_discon_req dr = { T_DISCON_REQ, 0 };
        mblk_t *req = tpi_reqblk(sizeof(dr));
        int error;

        memcpy(req->b_rptr, &dr, sizeof(dr));
        if ((error = tpi_okreq(ts, req, T_DISCON_REQ)) == 0)
                ts->ts_conn = 0;
        return error;
}

/* --- Addresses from and to the user. --- */

/* An AF_INET address from the user; *unspec if it is AF_UNSPEC. */
static int
tpi_addrin(uint64_t uaddr, uint64_t len, struct sockaddr_in *sin, int *unspec)
{
        int error;

        if (unspec != NULL)
                *unspec = 0;
        if (len < sizeof(sin->sin_family) || len > 128)
                return EINVAL;
        memset(sin, 0, sizeof(*sin));
        if ((error = copyin((const void *)(uintptr_t)uaddr, sin,
            len < sizeof(*sin) ? (size_t)len : sizeof(*sin))) != 0)
                return error;
        if (sin->sin_family == AF_UNSPEC && unspec != NULL) {
                *unspec = 1;
                return 0;
        }
        if (sin->sin_family != AF_INET)
                return EAFNOSUPPORT;
        if (len < sizeof(*sin))
                return EINVAL;
        memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
        return 0;
}

/*
 * An address out to the user, as accept and getsockname give one: up
 * to *ulen bytes of it, and its length in *ulen (a 32-bit socklen_t).
 */
int
sockaddr_out(const void *addr, uint32_t len, uint64_t uaddr, uint64_t ulen)
{
        uint32_t room;
        int error;

        if (uaddr == 0 || ulen == 0)
                return 0;
        if ((error = copyin((const void *)(uintptr_t)ulen, &room,
            sizeof(room))) != 0)
                return error;
        if (room > len)
                room = len;
        if (room > 0 && (error = copyout(addr, (void *)(uintptr_t)uaddr,
            room)) != 0)
                return error;
        return copyout(&len, (void *)(uintptr_t)ulen, sizeof(len));
}

/* --- The socket calls. --- */

static struct tpisock *
tpi_sock(struct vnode *vp)
{
        return vp->v_data;
}

int
tpisock_bind(struct vnode *vp, uint64_t uaddr, uint64_t len)
{
        struct tpisock *ts = tpi_sock(vp);
        struct sockaddr_in sin;
        int error;

        if ((error = tpi_addrin(uaddr, len, &sin, NULL)) != 0)
                return error;
        mutex_lock(&ts->ts_lock);
        error = ts->ts_bound ? EINVAL : tpi_bind(ts, &sin);
        mutex_unlock(&ts->ts_lock);
        return error;
}

/* connect: the peer datagrams go to and alone come from; AF_UNSPEC none. */
int
tpisock_connect(struct vnode *vp, uint64_t uaddr, uint64_t len)
{
        struct tpisock *ts = tpi_sock(vp);
        struct sockaddr_in sin;
        struct T_conn_req cr;
        int unspec, error;
        mblk_t *req;

        if ((error = tpi_addrin(uaddr, len, &sin, &unspec)) != 0)
                return error;
        if (!unspec && sin.sin_port == 0)
                return EINVAL;
        if (sin.sin_addr.s_addr == INADDR_ANY)
                sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* this host */
        mutex_lock(&ts->ts_lock);
        if (ts->ts_conn)
                error = tpi_disconnect(ts);
        if (error == 0 && !unspec && !ts->ts_bound)
                error = tpi_bind(ts, NULL);
        if (error == 0 && !unspec) {
                memset(&cr, 0, sizeof(cr));
                cr.PRIM_type = T_CONN_REQ;
                cr.DEST_length = sizeof(sin);
                cr.DEST_offset = sizeof(cr);
                req = tpi_reqblk(sizeof(cr) + sizeof(sin));
                memcpy(req->b_rptr, &cr, sizeof(cr));
                memcpy(req->b_rptr + sizeof(cr), &sin, sizeof(sin));
                if ((error = tpi_okreq(ts, req, T_CONN_REQ)) == 0) {
                        ts->ts_peer = sin;
                        ts->ts_conn = 1;
                }
        }
        mutex_unlock(&ts->ts_lock);
        return error;
}

static int
tpi_ioflag(int flags, int fflag)
{
        return (flags & MSG_DONTWAIT) != 0 || (fflag & FNONBLOCK) != 0 ?
            IO_NONBLOCK : 0;
}

/* A datagram of uio's bytes, to the address (if uaddr) or the peer. */
static int
tpi_send(struct tpisock *ts, struct uio *uio, int flags, int fflag,
    uint64_t uaddr, uint64_t len)
{
        size_t n = (size_t)uio->uio_resid;
        struct sockaddr_in sin;
        mblk_t *mp, *dp;
        int conn, error = 0;

        if (uaddr != 0) {
                if ((error = tpi_addrin(uaddr, len, &sin, NULL)) != 0)
                        return error;
                if (sin.sin_port == 0)
                        return EINVAL;
                if (sin.sin_addr.s_addr == INADDR_ANY)
                        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        if (n > TPI_MAXDGRAM)
                return EMSGSIZE;
        mutex_lock(&ts->ts_lock);
        if (!ts->ts_bound)
                error = tpi_bind(ts, NULL);
        conn = ts->ts_conn;
        mutex_unlock(&ts->ts_lock);
        if (error != 0)
                return error;
        if (uaddr == 0 && !conn)
                return EDESTADDRREQ;
        dp = allocb_wait(n);
        if (n > 0 && (error = uiomove(dp->b_wptr, n, UIO_WRITE, uio)) != 0) {
                freemsg(dp);
                return error;
        }
        dp->b_wptr += n;
        if (uaddr != 0) {
                struct T_unitdata_req ur;

                memset(&ur, 0, sizeof(ur));
                ur.PRIM_type = T_UNITDATA_REQ;
                ur.DEST_length = sizeof(sin);
                ur.DEST_offset = sizeof(ur);
                mp = allocb_wait(sizeof(ur) + sizeof(sin));
                mp->b_datap->db_type = M_PROTO;
                memcpy(mp->b_wptr, &ur, sizeof(ur));
                memcpy(mp->b_wptr + sizeof(ur), &sin, sizeof(sin));
                mp->b_wptr += sizeof(ur) + sizeof(sin);
                mp->b_cont = dp;
        } else {
                mp = dp;                /* to the peer */
        }
        return str_kputmsg(ts->ts_sd, mp, tpi_ioflag(flags, fflag));
}

/*
 * The next datagram into uio, its source into *from (when it has one).
 * *total is its length, which is more than was copied if it was cut.
 */
static int
tpi_recv(struct tpisock *ts, struct uio *uio, int flags, int fflag,
    struct sockaddr_in *from, int *hasfrom, size_t *total)
{
        mblk_t *mp, *dp;
        int error;

        *hasfrom = 0;
        *total = 0;
        for (;;) {
                union T_primitives pr;

                if ((error = str_kgetmsg(ts->ts_sd, 0, tpi_ioflag(flags,
                    fflag), (flags & MSG_PEEK) != 0, &mp)) != 0)
                        return error;
                if (mp == NULL)
                        return 0;
                if (DB_TYPE(mp) == M_DATA) {
                        dp = mp;
                        break;
                }
                memset(&pr, 0, sizeof(pr));
                memcpy(&pr, mp->b_rptr, MBLKL(mp) < sizeof(pr) ?
                    MBLKL(mp) : sizeof(pr));
                if (pr.type == T_UNITDATA_IND) {
                        *hasfrom = tpi_getaddr(mp, pr.unitdata_ind.SRC_length,
                            pr.unitdata_ind.SRC_offset, from) == 0;
                        dp = mp->b_cont;
                        break;
                }
                freemsg(mp);
                if (pr.type == T_UDERROR_IND)
                        return pr.uderror_ind.ERROR_type != 0 ?
                            pr.uderror_ind.ERROR_type : EIO;
                /* Anything else is not for a socket: passed over. */
        }
        for (mblk_t *bp = dp; bp != NULL; bp = bp->b_cont) {
                size_t n = MBLKL(bp);

                *total += n;
                if (n > (size_t)uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if (n > 0 && error == 0)
                        error = uiomove(bp->b_rptr, n, UIO_READ, uio);
        }
        freemsg(mp);                    /* the rest of it is discarded */
        return error;
}

int
tpisock_sendto(struct file *fp, const uint64_t *args, uint64_t *rv)
{
        struct iovec iov = { (void *)(uintptr_t)args[1], (size_t)args[2] };
        struct uio uio = { .uio_iov = &iov, .uio_iovcnt = 1,
            .uio_segflg = UIO_USERSPACE, .uio_rw = UIO_WRITE,
            .uio_limit = UIO_NOLIMIT };
        int flags = (int)args[3], error;

        if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0)
                return EOPNOTSUPP;
        if (iov.iov_len > INT64_MAX)
                return EINVAL;
        uio.uio_resid = (ssize_t)iov.iov_len;
        error = tpi_send(tpi_sock(fp->f_vnode), &uio, flags, fp->f_flag,
            args[4], args[5]);
        if (error == 0)
                *rv = args[2];
        return error;
}

int
tpisock_recvfrom(struct file *fp, const uint64_t *args, uint64_t *rv)
{
        struct iovec iov = { (void *)(uintptr_t)args[1], (size_t)args[2] };
        struct uio uio = { .uio_iov = &iov, .uio_iovcnt = 1,
            .uio_segflg = UIO_USERSPACE, .uio_rw = UIO_READ,
            .uio_limit = UIO_NOLIMIT };
        int flags = (int)args[3], hasfrom, error;
        struct sockaddr_in from;
        size_t total;

        if ((flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC)) != 0)
                return EOPNOTSUPP;
        if (iov.iov_len > INT64_MAX)
                return EINVAL;
        uio.uio_resid = (ssize_t)iov.iov_len;
        uio_prefault(iov.iov_base, iov.iov_len, UIO_READ);
        error = tpi_recv(tpi_sock(fp->f_vnode), &uio, flags, fp->f_flag,
            &from, &hasfrom, &total);
        if (error != 0)
                return error;
        if (args[4] != 0)
                error = sockaddr_out(&from, hasfrom ? sizeof(from) : 0,
                    args[4], args[5]);
        /* MSG_TRUNC: the datagram's length, however much was copied. */
        *rv = (flags & MSG_TRUNC) != 0 ? total :
            args[2] - (uint64_t)uio.uio_resid;
        return error;
}

int
tpisock_name(struct vnode *vp, int peer, uint64_t uaddr, uint64_t ulen)
{
        struct tpisock *ts = tpi_sock(vp);
        struct sockaddr_in sin;
        int error = 0;

        mutex_lock(&ts->ts_lock);
        if (peer && !ts->ts_conn)
                error = ENOTCONN;
        sin = peer ? ts->ts_peer : ts->ts_local;
        mutex_unlock(&ts->ts_lock);
        if (error != 0)
                return error;
        sin.sin_family = AF_INET;       /* unbound: 0.0.0.0, port 0 */
        return sockaddr_out(&sin, sizeof(sin), uaddr, ulen);
}

/* --- The socket's vnode: read, write and poll on the stream's terms. --- */

static int
tpi_close(struct vnode *vp, int flags, struct cred *cr)
{
        struct tpisock *ts = tpi_sock(vp);

        return VOP_CLOSE(ts->ts_stream, flags, cr);
}

static int
tpi_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct tpisock *ts = tpi_sock(vp);

        VN_RELE(ts->ts_stream);         /* the stream's last close */
        vn_destroy(vp);
        kmem_free(ts, sizeof(*ts));
        return 0;
}

static int
tpi_read(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct sockaddr_in from;
        size_t total;
        int hasfrom;

        return tpi_recv(tpi_sock(vp), uio, 0,
            (ioflag & IO_NONBLOCK) != 0 ? FNONBLOCK : 0, &from, &hasfrom,
            &total);
}

static int
tpi_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        return tpi_send(tpi_sock(vp), uio, 0,
            (ioflag & IO_NONBLOCK) != 0 ? FNONBLOCK : 0, 0, 0);
}

static int
tpi_poll(struct vnode *vp, short events, short *revents,
    struct pollhead **ph, struct cred *cr)
{
        struct tpisock *ts = tpi_sock(vp);

        return VOP_POLL(ts->ts_stream, events, revents, ph, cr);
}

static const struct vnodeops tpi_ops = {
        NULL, tpi_close, tpi_read, tpi_write, spec_getattr, spec_setattr,
        spec_access, spec_lookup, spec_create, spec_name1, spec_link,
        spec_rename, spec_mkdir, spec_name1, spec_readdir, spec_symlink,
        spec_readlink, spec_fsync, tpi_inactive, spec_putpage, spec_reclaim,
        spec_getpage, tpi_poll, NULL
};

/*
 * socket() for a family TPI serves: EAFNOSUPPORT if none of its kinds
 * is, EPROTONOSUPPORT if not this one. The new file in *fpp.
 */
int
tpisock_socket(int family, int type, int proto, int fflag, struct file **fpp)
{
        const struct tpiproto *tp = NULL;
        struct tpisock *ts;
        struct vnode *svp;
        int known = 0, error;

        for (size_t i = 0; i < sizeof(tpiprotos) / sizeof(tpiprotos[0]);
            i++) {
                if (tpiprotos[i].tp_family != family)
                        continue;
                known = 1;
                if (tpiprotos[i].tp_type == type && (proto == 0 ||
                    proto == tpiprotos[i].tp_proto))
                        tp = &tpiprotos[i];
        }
        if (!known)
                return EAFNOSUPPORT;
        if (tp == NULL)
                return EPROTONOSUPPORT;
        if ((error = str_kopen(tp->tp_dev, FREAD | FWRITE, CRED(), &svp)) != 0)
                return error;
        ts = kmem_zalloc(sizeof(*ts), KM_SLEEP);
        ts->ts_stream = svp;
        ts->ts_sd = svp->v_stream;
        mutex_init(&ts->ts_lock, "tpisock");
        ts->ts_local.sin_family = AF_INET;
        specattr_init(&ts->ts_attr, 0600, CRED()->cr_uid, CRED()->cr_gid, 0);
        vn_init(&ts->ts_vnode, NULL, VSOCK, &tpi_ops, &ts->ts_attr);
        VN_HOLD(&ts->ts_vnode);
        *fpp = falloc(&ts->ts_vnode, FREAD | FWRITE | fflag);
        return 0;
}
