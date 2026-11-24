/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * Sockets over TPI providers: socket(AF_INET, SOCK_DGRAM) opens a
 * stream on /dev/udp, SOCK_STREAM one on /dev/tcp, and the socket calls
 * become TPI messages on it (<sys/tihdr.h>). For datagrams: bind a
 * T_BIND_REQ, connect a T_CONN_REQ naming the peer, sendto a
 * T_UNITDATA_REQ, and recvfrom takes a T_UNITDATA_IND. For streams:
 * listen binds again with a backlog, accept takes a T_CONN_IND and
 * answers it with T_CONN_RES naming a new stream, connect waits for
 * T_CONN_CON or T_DISCON_IND, data is M_DATA both ways, shutdown sends
 * T_ORDREL_REQ, and T_ORDREL_IND is the end of the data.
 *
 * Requests are one at a time per socket (ts_lock), each waiting for its
 * acknowledgement, the one high priority message at the head. Data and
 * indications are the head's ordinary messages, which readers take
 * whole, one reader at a time on a stream socket (ts_rdlock) so what is
 * left of a message goes back in order. See doc/design/decisions.md,
 * "UDP and sockets over TPI" and "TCP and stream sockets over TPI".
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
#include <sys/signal.h>
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
#define TPI_CHUNK       16384   /* a stream's writes, a message apiece */
#define TPI_MAXBACKLOG  128
#define TPI_OPTMAX      16      /* an option's value, at most */

/* The providers, by the family, type and protocol that name them. */
static const struct tpiproto {
        int             tp_family;
        int             tp_type;
        int             tp_proto;
        const char      *tp_dev;
} tpiprotos[] = {
        { AF_INET, SOCK_DGRAM, IPPROTO_UDP, "udp" },
        { AF_INET, SOCK_STREAM, IPPROTO_TCP, "tcp" },
};

struct tpisock {
        struct specattr ts_attr;        /* first, for the spec operations */
        struct vnode    ts_vnode;
        struct vnode    *ts_stream;     /* held */
        struct stdata   *ts_sd;
        struct mutex    ts_rdlock;      /* a stream's one reader */
        struct mutex    ts_lock;        /* one request at a time; below */
        int             ts_type;        /* SOCK_DGRAM, SOCK_STREAM */
        int             ts_bound;
        int             ts_conn;
        int             ts_connecting;  /* T_CONN_CON or T_DISCON_IND due */
        int             ts_used;        /* has connected, or tried to */
        int             ts_listening;
        int             ts_eof;         /* the peer's data has ended */
        int             ts_rdshut;
        int             ts_wrshut;
        int             ts_soerror;     /* SO_ERROR: pending, said once */
        int             ts_broadcast;   /* SO_BROADCAST, as set */
        struct sockaddr_in ts_local;
        struct sockaddr_in ts_peer;
};

static const struct vnodeops tpi_ops;

static struct tpisock *
tpi_sock(struct vnode *vp)
{
        return vp->v_data;
}

static int tpi_sconnect(struct tpisock *, const struct sockaddr_in *, int);
static void tpi_tidy(struct tpisock *);

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
        case TBADSEQ:
                return ECONNABORTED;    /* the connection has gone */
        case TBADF:
        case TBADOPT:
        case TBADFLAG:
                return EINVAL;
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

/*
 * ts_lock held: bind to sin, or (NULL) to whatever port is free; with
 * a backlog of conind connections to accept, if more than none.
 */
static int
tpi_bind(struct tpisock *ts, const struct sockaddr_in *sin, unsigned conind)
{
        struct T_bind_req br;
        struct T_bind_ack ba;
        mblk_t *req, *ack;
        int error;

        memset(&br, 0, sizeof(br));
        br.PRIM_type = T_BIND_REQ;
        br.CONIND_number = conind;
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

int
tpisock_bind(struct vnode *vp, uint64_t uaddr, uint64_t len)
{
        struct tpisock *ts = tpi_sock(vp);
        struct sockaddr_in sin;
        int error;

        if ((error = tpi_addrin(uaddr, len, &sin, NULL)) != 0)
                return error;
        mutex_lock(&ts->ts_lock);
        error = ts->ts_bound ? EINVAL : tpi_bind(ts, &sin, 0);
        mutex_unlock(&ts->ts_lock);
        return error;
}

/*
 * connect: the peer datagrams go to and alone come from; AF_UNSPEC none.
 * A stream socket's connection (tpi_sconnect).
 */
int
tpisock_connect(struct file *fp, uint64_t uaddr, uint64_t len)
{
        struct tpisock *ts = tpi_sock(fp->f_vnode);
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
        if (ts->ts_type == SOCK_STREAM)
                return unspec ? EAFNOSUPPORT : tpi_sconnect(ts, &sin,
                    fp->f_flag);
        mutex_lock(&ts->ts_lock);
        if (ts->ts_conn)
                error = tpi_disconnect(ts);
        if (error == 0 && !unspec && !ts->ts_bound)
                error = tpi_bind(ts, NULL, 0);
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

/* --- Stream sockets. --- */

static int
tpi_prim(const mblk_t *mp, union T_primitives *pr)
{
        memset(pr, 0, sizeof(*pr));
        if (DB_TYPE(mp) == M_DATA)
                return -1;
        memcpy(pr, mp->b_rptr, MBLKL(mp) < sizeof(*pr) ? MBLKL(mp) :
            sizeof(*pr));
        return pr->type;
}

/*
 * The socket's state from an indication: T_CONN_CON and the ends. For
 * T_DISCON_IND, its error: the reason it gives, else ECONNREFUSED for
 * a connection being made and ECONNRESET for one made.
 */
static int
tpi_indication(struct tpisock *ts, const union T_primitives *pr)
{
        int error = 0;

        mutex_lock(&ts->ts_lock);
        switch (pr->type) {
        case T_CONN_CON:
                ts->ts_connecting = 0;
                ts->ts_conn = 1;
                break;
        case T_ORDREL_IND:
                ts->ts_eof = 1;
                break;
        case T_DISCON_IND:
                error = pr->discon_ind.DISCON_reason != 0 ?
                    pr->discon_ind.DISCON_reason : ts->ts_connecting ?
                    ECONNREFUSED : ECONNRESET;
                ts->ts_connecting = 0;
                ts->ts_conn = 0;
                ts->ts_eof = 1;
                break;
        }
        mutex_unlock(&ts->ts_lock);
        /* A poll that saw the message left this state to change: again. */
        pollwakeup(&ts->ts_sd->sd_poll);
        return error;
}

/* The pending error (SO_ERROR), taken. */
static int
tpi_takeerror(struct tpisock *ts)
{
        int error;

        mutex_lock(&ts->ts_lock);
        error = ts->ts_soerror;
        ts->ts_soerror = 0;
        mutex_unlock(&ts->ts_lock);
        return error;
}

/*
 * Without waiting, the messages at the front of the head that change
 * the socket's state, for poll and the calls that ask after it: a
 * stream socket's connection made or refused, or the peer's end
 * (T_DISCON_IND's error is then pending, for SO_ERROR or the next
 * call); a listener's notices of connections it was never given; a
 * datagram socket's T_UDERROR_IND. Not while a reader has the stream.
 */
static void
tpi_tidy(struct tpisock *ts)
{
        union T_primitives pr;
        mblk_t *mp;

        if (!mutex_trylock(&ts->ts_rdlock))
                return;
        while (str_kgetmsg(ts->ts_sd, 0, IO_NONBLOCK, 0, &mp) == 0 &&
            mp != NULL) {
                int prim = tpi_prim(mp, &pr), error = 0;

                if (ts->ts_type != SOCK_STREAM) {
                        if (prim != T_UDERROR_IND) {
                                str_kunget(ts->ts_sd, mp);
                                break;
                        }
                        error = pr.uderror_ind.ERROR_type != 0 ?
                            pr.uderror_ind.ERROR_type : EIO;
                } else if (ts->ts_listening) {
                        if (prim == T_CONN_IND) {
                                str_kunget(ts->ts_sd, mp);
                                break;
                        }
                } else if (prim == T_CONN_CON || prim == T_ORDREL_IND ||
                    prim == T_DISCON_IND) {
                        error = tpi_indication(ts, &pr);
                } else {
                        str_kunget(ts->ts_sd, mp);      /* data first */
                        break;
                }
                freemsg(mp);
                if (error != 0) {
                        mutex_lock(&ts->ts_lock);
                        ts->ts_soerror = error;
                        mutex_unlock(&ts->ts_lock);
                }
        }
        mutex_unlock(&ts->ts_rdlock);
}

/*
 * Wait for the connection being made to be made: T_CONN_CON, or
 * T_DISCON_IND's reason. The first ordinary message is the one.
 */
static int
tpi_settle(struct tpisock *ts)
{
        union T_primitives pr;
        mblk_t *mp;
        int error = 0;

        mutex_lock(&ts->ts_rdlock);
        while (ts->ts_connecting) {
                if ((error = str_kgetmsg(ts->ts_sd, 0, 0, 0, &mp)) != 0)
                        break;
                if (mp == NULL) {
                        error = ECONNRESET;
                        break;
                }
                if (tpi_prim(mp, &pr) == T_CONN_CON ||
                    pr.type == T_DISCON_IND) {
                        error = tpi_indication(ts, &pr);
                        freemsg(mp);
                        break;
                }
                str_kunget(ts->ts_sd, mp);      /* not ours to take */
                break;
        }
        mutex_unlock(&ts->ts_rdlock);
        return error;
}

/*
 * connect for a stream socket: it waits until the connection is made,
 * unless the file is non-blocking: then EINPROGRESS, poll says when it
 * is writable, and SO_ERROR (or connect again) says how it went.
 */
static int
tpi_sconnect(struct tpisock *ts, const struct sockaddr_in *sin, int fflag)
{
        struct T_conn_req cr;
        mblk_t *req;
        int error = 0;

        tpi_tidy(ts);                   /* how an earlier one went */
        mutex_lock(&ts->ts_lock);
        if (ts->ts_soerror != 0) {
                error = ts->ts_soerror; /* it failed: said once */
                ts->ts_soerror = 0;
        } else if (ts->ts_conn) {
                error = EISCONN;
        } else if (ts->ts_connecting) {
                error = EALREADY;
        } else if (ts->ts_listening || ts->ts_used) {
                error = EINVAL;
        }
        if (error == 0 && !ts->ts_bound)
                error = tpi_bind(ts, NULL, 0);
        if (error == 0) {
                memset(&cr, 0, sizeof(cr));
                cr.PRIM_type = T_CONN_REQ;
                cr.DEST_length = sizeof(*sin);
                cr.DEST_offset = sizeof(cr);
                req = tpi_reqblk(sizeof(cr) + sizeof(*sin));
                memcpy(req->b_rptr, &cr, sizeof(cr));
                memcpy(req->b_rptr + sizeof(cr), sin, sizeof(*sin));
                if ((error = tpi_okreq(ts, req, T_CONN_REQ)) == 0) {
                        ts->ts_peer = *sin;
                        ts->ts_connecting = 1;
                        ts->ts_used = 1;
                }
        }
        mutex_unlock(&ts->ts_lock);
        if (error != 0)
                return error;
        if ((fflag & FNONBLOCK) == 0)
                return tpi_settle(ts);
        tpi_tidy(ts);                   /* made already, or refused */
        mutex_lock(&ts->ts_lock);
        if (ts->ts_connecting) {
                error = EINPROGRESS;
        } else {
                error = ts->ts_soerror;
                ts->ts_soerror = 0;
        }
        mutex_unlock(&ts->ts_lock);
        return error;
}

/* A SIGPIPE for writing where the stream's sending has ended. */
static void
tpi_sigpipe(int flags)
{
        struct proc *p = curproc;
        int s;

        if ((flags & MSG_NOSIGNAL) != 0)
                return;
        s = spin_lock(&p->p_siglock);
        sigtolwp_locked(p, curlwp, SIGPIPE, NULL);
        spin_unlock(&p->p_siglock, s);
}

/* Bytes down a stream socket, a chunk at a time; *done how many. */
static int
tpi_ssend(struct tpisock *ts, struct uio *uio, int flags, int fflag,
    size_t *done)
{
        int ioflag = tpi_ioflag(flags, fflag), error = 0;

        *done = 0;
        mutex_lock(&ts->ts_lock);
        if (ts->ts_soerror != 0) {
                error = ts->ts_soerror; /* the connection failed */
                ts->ts_soerror = 0;
        } else if (ts->ts_wrshut || (ts->ts_eof && !ts->ts_conn)) {
                error = EPIPE;
        } else if (!ts->ts_conn && !ts->ts_connecting) {
                error = ENOTCONN;
        }
        mutex_unlock(&ts->ts_lock);
        while (error == 0 && uio->uio_resid > 0) {
                size_t n = (size_t)uio->uio_resid;
                mblk_t *dp;

                if (n > TPI_CHUNK)
                        n = TPI_CHUNK;
                if ((error = str_kwaitwrite(ts->ts_sd, ioflag)) != 0)
                        break;
                dp = allocb_wait(n);
                if ((error = uiomove(dp->b_wptr, n, UIO_WRITE, uio)) != 0) {
                        freemsg(dp);
                        break;
                }
                dp->b_wptr += n;
                if ((error = str_ksend(ts->ts_sd, dp)) != 0)
                        break;
                *done += n;
        }
        if (*done > 0)
                return 0;               /* what was sent */
        if (error == EPIPE)
                tpi_sigpipe(flags);
        return error;
}

/*
 * Bytes from a stream socket: what is there, waiting only if nothing
 * is (with MSG_WAITALL, until the request is filled, the peer's data
 * ends, it disconnects, or a signal comes). 0 bytes and no error at
 * the end of the peer's data.
 */
static int
tpi_srecv(struct tpisock *ts, struct uio *uio, int flags, int fflag)
{
        ssize_t start = uio->uio_resid;
        int error = 0, got = 0;
        int waitall = (flags & (MSG_WAITALL | MSG_PEEK)) == MSG_WAITALL;

        mutex_lock(&ts->ts_lock);
        if (ts->ts_soerror != 0) {
                error = ts->ts_soerror; /* what ended it, once */
                ts->ts_soerror = 0;
        } else if (ts->ts_rdshut || (ts->ts_eof && !ts->ts_conn &&
            !ts->ts_connecting)) {
                error = -1;             /* nothing more: 0 */
        } else if (!ts->ts_conn && !ts->ts_connecting) {
                error = ENOTCONN;
        }
        mutex_unlock(&ts->ts_lock);
        if (error != 0)
                return error < 0 ? 0 : error;
        mutex_lock(&ts->ts_rdlock);
        while (uio->uio_resid > 0) {
                int peek = (flags & MSG_PEEK) != 0 && !got;
                union T_primitives pr;
                mblk_t *mp;

                if (ts->ts_eof)
                        break;
                error = str_kgetmsg(ts->ts_sd, 0, got && !waitall ?
                    IO_NONBLOCK : tpi_ioflag(flags, fflag), peek, &mp);
                if (error != 0) {
                        if (got)
                                error = 0;
                        break;
                }
                if (mp == NULL)
                        break;
                if (DB_TYPE(mp) != M_DATA && peek) {
                        /* Indications are taken, even by a peek. */
                        freemsg(mp);
                        if (str_kgetmsg(ts->ts_sd, 0, IO_NONBLOCK, 0,
                            &mp) != 0 || mp == NULL)
                                break;
                }
                if (DB_TYPE(mp) == M_DATA) {
                        mblk_t *bp;

                        for (bp = mp; bp != NULL && uio->uio_resid > 0 &&
                            error == 0; bp = bp->b_cont) {
                                size_t n = MBLKL(bp);
                                ssize_t before = uio->uio_resid;

                                if (n > (size_t)uio->uio_resid)
                                        n = (size_t)uio->uio_resid;
                                if (n == 0)
                                        continue;
                                error = uiomove(bp->b_rptr, n, UIO_READ, uio);
                                bp->b_rptr += (size_t)(before -
                                    uio->uio_resid);
                        }
                        got = uio->uio_resid != start;
                        if (peek) {
                                freemsg(mp);
                                break;
                        }
                        while (mp != NULL && MBLKL(mp) == 0) {
                                bp = mp->b_cont;
                                freeb(mp);
                                mp = bp;
                        }
                        if (mp != NULL)
                                str_kunget(ts->ts_sd, mp);
                        if (error != 0) {
                                if (got)
                                        error = 0;
                                break;
                        }
                        continue;
                }
                switch (tpi_prim(mp, &pr)) {
                case T_CONN_CON:
                case T_ORDREL_IND:
                        tpi_indication(ts, &pr);
                        freemsg(mp);
                        continue;
                case T_DISCON_IND:
                        if (got) {
                                str_kunget(ts->ts_sd, mp);
                                break;
                        }
                        error = tpi_indication(ts, &pr);
                        freemsg(mp);
                        break;
                default:
                        freemsg(mp);
                        continue;
                }
                break;
        }
        mutex_unlock(&ts->ts_rdlock);
        return error;
}

/* listen: bind again, with the backlog, at the same address. */
int
tpisock_listen(struct vnode *vp, int backlog)
{
        struct tpisock *ts = tpi_sock(vp);
        struct T_unbind_req ur = { T_UNBIND_REQ };
        struct sockaddr_in sin;
        int error = 0;

        if (ts->ts_type != SOCK_STREAM)
                return EOPNOTSUPP;
        if (backlog < 1)
                backlog = 1;
        if (backlog > TPI_MAXBACKLOG)
                backlog = TPI_MAXBACKLOG;
        mutex_lock(&ts->ts_lock);
        if (ts->ts_conn || ts->ts_connecting || ts->ts_used) {
                error = EINVAL;
        } else if (!ts->ts_listening) {
                if (ts->ts_bound) {
                        mblk_t *req = tpi_reqblk(sizeof(ur));

                        memcpy(req->b_rptr, &ur, sizeof(ur));
                        sin = ts->ts_local;
                        if ((error = tpi_okreq(ts, req, T_UNBIND_REQ)) == 0) {
                                ts->ts_bound = 0;
                                error = tpi_bind(ts, &sin, (unsigned)backlog);
                        }
                } else {
                        error = tpi_bind(ts, NULL, (unsigned)backlog);
                }
                if (error == 0)
                        ts->ts_listening = 1;
        }
        mutex_unlock(&ts->ts_lock);
        return error;
}

static int tpi_new(const struct tpiproto *, int, struct file **,
    struct tpisock **);

/* accept: the next connection, on a new stream, as a new file. */
int
tpisock_accept(struct file *fp, const uint64_t *args, uint64_t *rv)
{
        struct tpisock *ts = tpi_sock(fp->f_vnode), *nts;
        int flags = (int)args[3], error = 0, fd;
        struct file *nfp = NULL;
        struct sockaddr_in from;

        if ((flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != 0)
                return EINVAL;
        if (ts->ts_type != SOCK_STREAM)
                return EOPNOTSUPP;
        if (!ts->ts_listening)
                return EINVAL;
        mutex_lock(&ts->ts_rdlock);
        for (;;) {
                struct T_capability_req cq = { T_CAPABILITY_REQ,
                    TC1_ACCEPTOR_ID };
                struct T_capability_ack ca;
                struct T_conn_res cres;
                union T_primitives pr;
                t_scalar_t seq;
                mblk_t *mp, *req;

                if ((error = str_kgetmsg(ts->ts_sd, 0, tpi_ioflag(0,
                    fp->f_flag), 0, &mp)) != 0)
                        break;
                if (mp == NULL) {
                        error = ECONNABORTED;
                        break;
                }
                if (tpi_prim(mp, &pr) != T_CONN_IND ||
                    tpi_getaddr(mp, pr.conn_ind.SRC_length,
                    pr.conn_ind.SRC_offset, &from) != 0) {
                        freemsg(mp);    /* a T_DISCON_IND, for one */
                        continue;
                }
                seq = pr.conn_ind.SEQ_number;
                freemsg(mp);

                /* The stream that takes it, and its id. */
                if ((error = tpi_new(&tpiprotos[1], (flags &
                    SOCK_NONBLOCK) != 0 ? FNONBLOCK : 0, &nfp, &nts)) != 0)
                        break;
                req = tpi_reqblk(sizeof(cq));
                memcpy(req->b_rptr, &cq, sizeof(cq));
                mutex_lock(&nts->ts_lock);
                error = tpi_request(nts, req, T_CAPABILITY_ACK, &mp);
                mutex_unlock(&nts->ts_lock);
                if (error == 0) {
                        memset(&ca, 0, sizeof(ca));
                        memcpy(&ca, mp->b_rptr, MBLKL(mp) < sizeof(ca) ?
                            MBLKL(mp) : sizeof(ca));
                        freemsg(mp);
                        memset(&cres, 0, sizeof(cres));
                        cres.PRIM_type = T_CONN_RES;
                        cres.ACCEPTOR_id = ca.ACCEPTOR_id;
                        cres.SEQ_number = seq;
                        req = tpi_reqblk(sizeof(cres));
                        memcpy(req->b_rptr, &cres, sizeof(cres));
                        mutex_lock(&ts->ts_lock);
                        error = tpi_okreq(ts, req, T_CONN_RES);
                        mutex_unlock(&ts->ts_lock);
                }
                if (error == 0)
                        break;
                frele(nfp);
                nfp = NULL;
                if (error != ECONNABORTED)
                        break;
                /* That one was reset before it could be taken: the next. */
        }
        mutex_unlock(&ts->ts_rdlock);
        if (error != 0)
                return error;
        nts->ts_bound = nts->ts_conn = nts->ts_used = 1;
        nts->ts_local = ts->ts_local;
        nts->ts_peer = from;
        error = fd_alloc_flags(curproc, nfp, 0,
            (flags & SOCK_CLOEXEC) != 0 ? FD_CLOEXEC : 0, &fd);
        if (error != 0) {
                frele(nfp);
                return error;
        }
        *rv = (uint64_t)fd;
        /* The descriptor is the caller's now, address or no address. */
        (void)sockaddr_out(&from, sizeof(from), args[1], args[2]);
        return 0;
}

/* shutdown: no more reading, or writing (a T_ORDREL_REQ), or both. */
int
tpisock_shutdown(struct vnode *vp, int how)
{
        struct tpisock *ts = tpi_sock(vp);
        int error = 0;

        if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
                return EINVAL;
        mutex_lock(&ts->ts_lock);
        if (!ts->ts_conn && !ts->ts_connecting) {
                error = ENOTCONN;
        } else {
                if (how != SHUT_WR)
                        ts->ts_rdshut = 1;
                if (how != SHUT_RD && !ts->ts_wrshut) {
                        ts->ts_wrshut = 1;
                        if (ts->ts_type == SOCK_STREAM) {
                                struct T_ordrel_req or = { T_ORDREL_REQ };
                                mblk_t *mp = allocb_wait(sizeof(or));

                                mp->b_datap->db_type = M_PROTO;
                                memcpy(mp->b_wptr, &or, sizeof(or));
                                mp->b_wptr += sizeof(or);
                                error = str_ksend(ts->ts_sd, mp);
                        }
                }
        }
        mutex_unlock(&ts->ts_lock);
        return error;
}

/* ts_lock held: a stream socket's own address and its peer's, from TCP. */
static int
tpi_addrs(struct tpisock *ts, struct sockaddr_in *loc, struct sockaddr_in *rem)
{
        struct T_addr_req ar = { T_ADDR_REQ };
        struct T_addr_ack aa;
        mblk_t *req = tpi_reqblk(sizeof(ar)), *ack;
        int error;

        memcpy(req->b_rptr, &ar, sizeof(ar));
        if ((error = tpi_request(ts, req, T_ADDR_ACK, &ack)) != 0)
                return error;
        memset(&aa, 0, sizeof(aa));
        memcpy(&aa, ack->b_rptr, MBLKL(ack) < sizeof(aa) ? MBLKL(ack) :
            sizeof(aa));
        if (aa.LOCADDR_length == 0 || tpi_getaddr(ack, aa.LOCADDR_length,
            aa.LOCADDR_offset, loc) != 0)
                memset(loc, 0, sizeof(*loc));
        if (aa.REMADDR_length == 0 || tpi_getaddr(ack, aa.REMADDR_length,
            aa.REMADDR_offset, rem) != 0)
                memset(rem, 0, sizeof(*rem));
        freemsg(ack);
        return 0;
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
                /* UDP drops it too; the error is the socket's to give. */
                if (sin.sin_addr.s_addr == INADDR_BROADCAST &&
                    !ts->ts_broadcast)
                        return EACCES;
        }
        if (n > TPI_MAXDGRAM)
                return EMSGSIZE;
        mutex_lock(&ts->ts_lock);
        if (!ts->ts_bound)
                error = tpi_bind(ts, NULL, 0);
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
        if ((error = tpi_takeerror(ts)) != 0)
                return error;           /* a T_UDERROR_IND, taken before */
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
        struct tpisock *ts = tpi_sock(fp->f_vnode);
        int flags = (int)args[3], error;

        if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0)
                return EOPNOTSUPP;
        if (iov.iov_len > INT64_MAX)
                return EINVAL;
        uio.uio_resid = (ssize_t)iov.iov_len;
        if (ts->ts_type == SOCK_STREAM) {
                size_t done;

                if (args[4] != 0)
                        return EISCONN;
                uio_prefault(iov.iov_base, iov.iov_len, UIO_WRITE);
                error = tpi_ssend(ts, &uio, flags, fp->f_flag, &done);
                if (error == 0)
                        *rv = done;
                return error;
        }
        error = tpi_send(ts, &uio, flags, fp->f_flag, args[4], args[5]);
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
        struct tpisock *ts = tpi_sock(fp->f_vnode);
        int flags = (int)args[3], hasfrom, error;
        struct sockaddr_in from;
        size_t total;

        if ((flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC |
            MSG_WAITALL)) != 0)
                return EOPNOTSUPP;
        if (iov.iov_len > INT64_MAX)
                return EINVAL;
        uio.uio_resid = (ssize_t)iov.iov_len;
        uio_prefault(iov.iov_base, iov.iov_len, UIO_READ);
        if (ts->ts_type == SOCK_STREAM) {
                if ((flags & MSG_TRUNC) != 0)
                        return EOPNOTSUPP;
                if ((error = tpi_srecv(ts, &uio, flags, fp->f_flag)) != 0)
                        return error;
                if (args[4] != 0)       /* a connection's has no address */
                        error = sockaddr_out(&from, 0, args[4], args[5]);
                *rv = args[2] - (uint64_t)uio.uio_resid;
                return error;
        }
        error = tpi_recv(ts, &uio, flags, fp->f_flag, &from, &hasfrom,
            &total);
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
        /* A connection's own address is IP's choice: TCP knows it. */
        if (error == 0 && !peer && ts->ts_type == SOCK_STREAM &&
            ts->ts_bound) {
                struct sockaddr_in rem;

                error = tpi_addrs(ts, &sin, &rem);
        }
        mutex_unlock(&ts->ts_lock);
        if (error != 0)
                return error;
        sin.sin_family = AF_INET;       /* unbound: 0.0.0.0, port 0 */
        return sockaddr_out(&sin, sizeof(sin), uaddr, ulen);
}

/* --- Options. --- */

/*
 * One option by T_OPTMGMT_REQ: mgmt T_NEGOTIATE to set it to val (n
 * bytes), T_CURRENT for its value, into out (up to *outlen bytes;
 * *outlen then says how many).
 */
static int
tpi_option(struct tpisock *ts, t_scalar_t mgmt, int level, int name,
    const void *val, size_t n, void *out, size_t *outlen)
{
        struct T_optmgmt_req req;
        struct T_optmgmt_ack oa;
        struct t_opthdr oh;
        size_t have, vlen;
        mblk_t *mp, *ack;
        int error;

        memset(&req, 0, sizeof(req));
        req.PRIM_type = T_OPTMGMT_REQ;
        req.OPT_length = (t_scalar_t)(sizeof(oh) + n);
        req.OPT_offset = sizeof(req);
        req.MGMT_flags = mgmt;
        memset(&oh, 0, sizeof(oh));
        oh.len = (t_uscalar_t)(sizeof(oh) + n);
        oh.level = (t_uscalar_t)level;
        oh.name = (t_uscalar_t)name;
        mp = tpi_reqblk(sizeof(req) + sizeof(oh) + n);
        memcpy(mp->b_rptr, &req, sizeof(req));
        memcpy(mp->b_rptr + sizeof(req), &oh, sizeof(oh));
        if (n > 0)
                memcpy(mp->b_rptr + sizeof(req) + sizeof(oh), val, n);
        mutex_lock(&ts->ts_lock);
        error = tpi_request(ts, mp, T_OPTMGMT_ACK, &ack);
        mutex_unlock(&ts->ts_lock);
        if (error != 0)
                return error;
        have = MBLKL(ack);
        memset(&oa, 0, sizeof(oa));
        memcpy(&oa, ack->b_rptr, have < sizeof(oa) ? have : sizeof(oa));
        if (oa.OPT_offset < 0 || oa.OPT_length < (t_scalar_t)sizeof(oh) ||
            (size_t)oa.OPT_offset > have ||
            (size_t)oa.OPT_length > have - (size_t)oa.OPT_offset) {
                freemsg(ack);
                return EPROTO;
        }
        memcpy(&oh, ack->b_rptr + oa.OPT_offset, sizeof(oh));
        if (oh.len < sizeof(oh) || oh.len > (size_t)oa.OPT_length) {
                freemsg(ack);
                return EPROTO;
        }
        switch (oh.status) {
        case T_SUCCESS:
        case T_PARTSUCCESS:
                vlen = oh.len - sizeof(oh);
                if (out != NULL) {
                        if (vlen > *outlen)
                                vlen = *outlen;
                        memcpy(out, ack->b_rptr + oa.OPT_offset + sizeof(oh),
                            vlen);
                        *outlen = vlen;
                }
                break;
        case T_FAILURE:
                error = EINVAL;         /* a value it will not take */
                break;
        default:
                error = ENOPROTOOPT;    /* T_NOTSUPPORT, T_READONLY */
                break;
        }
        freemsg(ack);
        return error;
}

/*
 * An option's value out to the user, as getsockopt gives one: up to
 * *ulen bytes of it, and how many in *ulen (a 32-bit socklen_t).
 */
int
sockopt_out(const void *val, uint32_t len, uint64_t uval, uint64_t ulen)
{
        uint32_t room;
        int error;

        if ((error = copyin((const void *)(uintptr_t)ulen, &room,
            sizeof(room))) != 0)
                return error;
        if (room > len)
                room = len;
        if (room > 0 && (error = copyout(val, (void *)(uintptr_t)uval,
            room)) != 0)
                return error;
        return copyout(&room, (void *)(uintptr_t)ulen, sizeof(room));
}

/*
 * setsockopt: SO_TYPE, SO_ERROR and SO_ACCEPTCONN are the socket's to
 * report, not set; the rest are the provider's.
 */
int
tpisock_setsockopt(struct vnode *vp, int level, int name, uint64_t uval,
    uint64_t len)
{
        struct tpisock *ts = tpi_sock(vp);
        uint8_t val[TPI_OPTMAX];
        size_t n = len < sizeof(val) ? (size_t)len : sizeof(val);
        int error, v;

        if (len > UINT32_MAX)
                return EINVAL;
        if (level == SOL_SOCKET && (name == SO_TYPE || name == SO_ERROR ||
            name == SO_ACCEPTCONN))
                return ENOPROTOOPT;
        memset(val, 0, sizeof(val));
        if (n > 0 && (error = copyin((const void *)(uintptr_t)uval, val,
            n)) != 0)
                return error;
        error = tpi_option(ts, T_NEGOTIATE, level, name, val, n, NULL, NULL);
        if (error == 0 && level == SOL_SOCKET && name == SO_BROADCAST &&
            n >= sizeof(v)) {
                memcpy(&v, val, sizeof(v));
                ts->ts_broadcast = v != 0;
        }
        return error;
}

int
tpisock_getsockopt(struct vnode *vp, int level, int name, uint64_t uval,
    uint64_t ulen)
{
        struct tpisock *ts = tpi_sock(vp);
        uint8_t out[TPI_OPTMAX];
        size_t outlen = sizeof(out);
        int error, v;

        if (level == SOL_SOCKET && (name == SO_TYPE || name == SO_ERROR ||
            name == SO_ACCEPTCONN)) {
                if (name == SO_TYPE) {
                        v = ts->ts_type;
                } else if (name == SO_ACCEPTCONN) {
                        v = ts->ts_listening;
                } else {
                        tpi_tidy(ts);   /* an error waiting at the head */
                        v = tpi_takeerror(ts);
                }
                return sockopt_out(&v, sizeof(v), uval, ulen);
        }
        memset(out, 0, sizeof(out));
        if ((error = tpi_option(ts, T_CURRENT, level, name, NULL, 0, out,
            &outlen)) != 0)
                return error;
        return sockopt_out(out, (uint32_t)outlen, uval, ulen);
}

/* --- The socket's vnode: read, write and poll on the stream's terms. --- */

/*
 * A socket's close does not wait, as a stream's does, for its output to
 * drain (up to 15 s, for a peer whose window is shut): what is left of
 * it goes on after, the provider's to send (TCP takes what is still on
 * its queue into the connection when the stream closes).
 */
static int
tpi_close(struct vnode *vp, int flags, struct cred *cr)
{
        struct tpisock *ts = tpi_sock(vp);

        return VOP_CLOSE(ts->ts_stream, flags | FNONBLOCK, cr);
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

        struct tpisock *ts = tpi_sock(vp);
        int fflag = (ioflag & IO_NONBLOCK) != 0 ? FNONBLOCK : 0;

        if (ts->ts_type == SOCK_STREAM)
                return tpi_srecv(ts, uio, 0, fflag);
        return tpi_recv(ts, uio, 0, fflag, &from, &hasfrom, &total);
}

static int
tpi_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct tpisock *ts = tpi_sock(vp);
        int fflag = (ioflag & IO_NONBLOCK) != 0 ? FNONBLOCK : 0;
        size_t done;

        if (ts->ts_type == SOCK_STREAM)
                return tpi_ssend(ts, uio, 0, fflag, &done);
        return tpi_send(ts, uio, 0, fflag, 0, 0);
}

/*
 * The stream head's readiness, corrected by the socket's state: the
 * head holds acknowledgements and indications as well as data, and a
 * socket can be readable or writable with nothing there (read finds the
 * end, write fails at once). A stream socket's state is brought up to
 * date first (tpi_tidy).
 */
static int
tpi_poll(struct vnode *vp, short events, short *revents,
    struct pollhead **ph, struct cred *cr)
{
        struct tpisock *ts = tpi_sock(vp);
        short ready;
        int error;

        if (ts->ts_type == SOCK_STREAM)
                tpi_tidy(ts);
        if ((error = VOP_POLL(ts->ts_stream, POLL_READ | POLL_WRITE |
            POLLRDNORM, &ready, ph, cr)) != 0)
                return error;
        ready &= ~(POLLPRI | POLLRDBAND | POLLWRBAND);  /* no bands */
        mutex_lock(&ts->ts_lock);
        if (ts->ts_type == SOCK_STREAM) {
                if (ts->ts_connecting) {
                        /* Only T_CONN_CON or T_DISCON_IND can be there. */
                        ready &= ~(POLLIN | POLLRDNORM | POLLOUT);
                } else if (ts->ts_listening) {
                        ready &= ~POLLOUT;      /* POLLIN: T_CONN_IND */
                } else if (!ts->ts_conn) {
                        ready |= POLLOUT | POLLHUP;
                }
                if (ts->ts_eof || ts->ts_rdshut)
                        ready |= POLLIN | POLLRDNORM;
                if (ts->ts_wrshut)
                        ready |= POLLOUT;
                if (ts->ts_eof && ts->ts_wrshut)
                        ready |= POLLHUP;
        }
        if (ts->ts_soerror != 0)
                ready |= POLLERR;
        mutex_unlock(&ts->ts_lock);
        *revents = ready & (events | POLLERR | POLLHUP);
        return 0;
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
        int known = 0;

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
        return tpi_new(tp, fflag, fpp, &ts);
}

/* A new socket of tp's: its stream, its vnode, and a file for it. */
static int
tpi_new(const struct tpiproto *tp, int fflag, struct file **fpp,
    struct tpisock **tsp)
{
        struct tpisock *ts;
        struct vnode *svp;
        int error;

        if ((error = str_kopen(tp->tp_dev, FREAD | FWRITE, CRED(), &svp)) != 0)
                return error;
        ts = kmem_zalloc(sizeof(*ts), KM_SLEEP);
        ts->ts_stream = svp;
        ts->ts_sd = svp->v_stream;
        ts->ts_type = tp->tp_type;
        mutex_init(&ts->ts_rdlock, "tpisockrd");
        mutex_init(&ts->ts_lock, "tpisock");
        ts->ts_local.sin_family = AF_INET;
        specattr_init(&ts->ts_attr, 0600, CRED()->cr_uid, CRED()->cr_gid, 0);
        vn_init(&ts->ts_vnode, NULL, VSOCK, &tpi_ops, &ts->ts_attr);
        VN_HOLD(&ts->ts_vnode);
        *fpp = falloc(&ts->ts_vnode, FREAD | FWRITE | fflag);
        *tsp = ts;
        return 0;
}
