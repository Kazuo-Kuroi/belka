/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * UDP (RFC 768) as a multiplexing STREAMS driver, /dev/udp, and a
 * connectionless TPI provider (<sys/tihdr.h>) to its upper streams:
 * sockets (kern/sock_tpi.c) and programs by putmsg and getmsg. One
 * lower stream, an IP stream linked by I_PLINK, which UDP binds to
 * protocol 17; datagrams for us come up from it with their IP header,
 * and ours go down with one, for IP to finish (the source, if left
 * unset, and the checksum, which covers it). Every queue of UDP's is in
 * one syncq (SQLVL_MODULE), which guards the endpoints. Options come by
 * T_OPTMGMT_REQ: SO_REUSEADDR, SO_BROADCAST, SO_RCVBUF (the stream
 * head's high-water mark), and SO_SNDBUF, which is only kept. See
 * doc/design/decisions.md, "UDP and sockets over TPI" and "Socket
 * options, and connecting without waiting".
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/ipconf.h>
#include <sys/kmem.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/systm.h>
#include <sys/tihdr.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>

#define UDP_HLEN        8
#define UDP_MAXDGRAM    (65535 - IP_MINHLEN - UDP_HLEN)
#define UDP_PORT_FIRST  49152   /* the ephemeral ports, host order */
#define UDP_PORT_LAST   65535
#define UDP_REPLYSZ     64      /* replies fit in a block this big */
#define UDP_SNDBUF      65536
#define UDP_RCVBUF      65536   /* the stream head's high-water mark */
#define UDP_MINBUF      2048
#define UDP_MAXBUF      262144

struct udp_ep {
        struct udp_ep   *ue_next;
        queue_t         *ue_rq;
        int             ue_state;       /* TS_UNBND, TS_IDLE */
        int             ue_conn;        /* ue_faddr, ue_fport are the peer */
        int             ue_priv;        /* may bind below 1024 */
        in_addr_t       ue_laddr;
        in_port_t       ue_lport;       /* network order, as the rest */
        in_addr_t       ue_faddr;
        in_port_t       ue_fport;
        int             ue_reuse;       /* SO_REUSEADDR */
        int             ue_broadcast;   /* SO_BROADCAST */
        uint32_t        ue_sndbuf;      /* SO_SNDBUF: kept, and reported */
        uint32_t        ue_rcvbuf;      /* SO_RCVBUF */
};

static struct udp_ep *udp_eps;
static queue_t *udp_lower;              /* the IP stream's l_qbot */
static int udp_muxid;
static uint16_t udp_nextport = UDP_PORT_FIRST;

static int udp_open(queue_t *, dev_t *, int, int, struct cred *);
static int udp_close(queue_t *, int, struct cred *);
static int udp_uwput(queue_t *, mblk_t *);
static int udp_lrput(queue_t *, mblk_t *);
static int udp_lwput(queue_t *, mblk_t *);

static struct module_info udp_minfo = {
        0x5544, "udp", 0, INFPSZ, 65536, 8192
};
static struct qinit udp_urinit = {
        NULL, NULL, udp_open, udp_close, NULL, &udp_minfo, NULL
};
static struct qinit udp_uwinit = {
        udp_uwput, NULL, NULL, NULL, NULL, &udp_minfo, NULL
};
static struct qinit udp_lrinit = {
        udp_lrput, NULL, NULL, NULL, NULL, &udp_minfo, NULL
};
static struct qinit udp_lwinit = {
        udp_lwput, NULL, NULL, NULL, NULL, &udp_minfo, NULL
};

struct streamtab udpinfo = {
        &udp_urinit, &udp_uwinit, &udp_lrinit, &udp_lwinit
};

/* --- TPI replies. --- */

/*
 * A block of size bytes of type for a reply to mp: mp's own when it
 * can be (the socket layer sends requests in blocks big enough), else
 * a new one, mp freed. NULL, mp freed, if there is none.
 */
static mblk_t *
udp_reply(mblk_t *mp, size_t size, unsigned char type)
{
        mblk_t *rp;

        if (mp->b_datap->db_ref == 1 && MBLKSIZE(mp) >= size) {
                if (mp->b_cont != NULL) {
                        freemsg(mp->b_cont);
                        mp->b_cont = NULL;
                }
                rp = mp;
        } else {
                freemsg(mp);
                if ((rp = allocb(size, BPRI_HI)) == NULL)
                        return NULL;
        }
        rp->b_datap->db_type = type;
        rp->b_rptr = rp->b_datap->db_base;
        rp->b_wptr = rp->b_rptr + size;
        memset(rp->b_rptr, 0, size);
        return rp;
}

static void
udp_errack(queue_t *q, mblk_t *mp, t_scalar_t prim, t_scalar_t tli,
    t_scalar_t unixerr)
{
        struct T_error_ack ea = { T_ERROR_ACK, prim, tli, unixerr };

        if ((mp = udp_reply(mp, sizeof(ea), M_PCPROTO)) == NULL)
                return;
        memcpy(mp->b_rptr, &ea, sizeof(ea));
        qreply(q, mp);
}

static void
udp_okack(queue_t *q, mblk_t *mp, t_scalar_t prim)
{
        struct T_ok_ack ok = { T_OK_ACK, prim };

        if ((mp = udp_reply(mp, sizeof(ok), M_PCPROTO)) == NULL)
                return;
        memcpy(mp->b_rptr, &ok, sizeof(ok));
        qreply(q, mp);
}

/*
 * The address of a request: len bytes at off in its first block, an
 * AF_INET sockaddr_in (no more than the family, port and address need
 * be there). 0, or a TLI error.
 */
static int
udp_addr(const mblk_t *mp, t_scalar_t len, t_scalar_t off,
    struct sockaddr_in *sin)
{
        size_t have = MBLKL(mp);

        memset(sin, 0, sizeof(*sin));
        if (len < (t_scalar_t)offsetof(struct sockaddr_in, sin_zero) ||
            off < 0 || (size_t)off > have || (size_t)len > have - (size_t)off)
                return TBADADDR;
        memcpy(sin, mp->b_rptr + off, (size_t)len < sizeof(*sin) ?
            (size_t)len : sizeof(*sin));
        if (sin->sin_family != AF_INET)
                return TBADADDR;
        return 0;
}

/* --- Binding. --- */

/*
 * Whether the port (network order) at laddr would clash with a binding:
 * not if both have SO_REUSEADDR (reuse is the binder's).
 */
static int
udp_inuse(in_addr_t laddr, in_port_t port, int reuse)
{
        for (struct udp_ep *ue = udp_eps; ue != NULL; ue = ue->ue_next)
                if (ue->ue_state == TS_IDLE && ue->ue_lport == port &&
                    !(reuse && ue->ue_reuse) &&
                    (ue->ue_laddr == INADDR_ANY || laddr == INADDR_ANY ||
                    ue->ue_laddr == laddr))
                        return 1;
        return 0;
}

static void
udp_bind(queue_t *q, mblk_t *mp)
{
        struct udp_ep *ue = q->q_ptr;
        struct T_bind_req br;
        struct T_bind_ack ba;
        struct sockaddr_in sin;
        int error;

        if (MBLKL(mp) < sizeof(br)) {
                udp_errack(q, mp, T_BIND_REQ, TSYSERR, EINVAL);
                return;
        }
        memcpy(&br, mp->b_rptr, sizeof(br));
        if (ue->ue_state != TS_UNBND) {
                udp_errack(q, mp, T_BIND_REQ, TOUTSTATE, 0);
                return;
        }
        if (br.ADDR_length == 0) {
                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
        } else if ((error = udp_addr(mp, br.ADDR_length, br.ADDR_offset,
            &sin)) != 0) {
                udp_errack(q, mp, T_BIND_REQ, error, 0);
                return;
        }
        if (sin.sin_port == 0) {
                /* An ephemeral port: the next free one, round the range. */
                int span = UDP_PORT_LAST - UDP_PORT_FIRST + 1;

                for (int i = 0; i < span && sin.sin_port == 0; i++) {
                        in_port_t p = htons(udp_nextport);

                        udp_nextport = udp_nextport == UDP_PORT_LAST ?
                            UDP_PORT_FIRST : udp_nextport + 1;
                        if (!udp_inuse(sin.sin_addr.s_addr, p, 0))
                                sin.sin_port = p;
                }
                if (sin.sin_port == 0) {
                        udp_errack(q, mp, T_BIND_REQ, TNOADDR, 0);
                        return;
                }
        } else if (ntohs(sin.sin_port) < 1024 && !ue->ue_priv) {
                udp_errack(q, mp, T_BIND_REQ, TACCES, 0);
                return;
        } else if (udp_inuse(sin.sin_addr.s_addr, sin.sin_port,
            ue->ue_reuse)) {
                udp_errack(q, mp, T_BIND_REQ, TADDRBUSY, 0);
                return;
        }
        ue->ue_laddr = sin.sin_addr.s_addr;
        ue->ue_lport = sin.sin_port;
        ue->ue_state = TS_IDLE;
        ue->ue_conn = 0;

        memset(&ba, 0, sizeof(ba));
        ba.PRIM_type = T_BIND_ACK;
        ba.ADDR_length = sizeof(sin);
        ba.ADDR_offset = sizeof(ba);
        if ((mp = udp_reply(mp, sizeof(ba) + sizeof(sin), M_PCPROTO)) ==
            NULL)
                return;
        memset(sin.sin_zero, 0, sizeof(sin.sin_zero));
        memcpy(mp->b_rptr, &ba, sizeof(ba));
        memcpy(mp->b_rptr + sizeof(ba), &sin, sizeof(sin));
        qreply(q, mp);
}

static void
udp_info(queue_t *q, mblk_t *mp)
{
        struct udp_ep *ue = q->q_ptr;
        struct T_info_ack ia;

        memset(&ia, 0, sizeof(ia));
        ia.PRIM_type = T_INFO_ACK;
        ia.TSDU_size = UDP_MAXDGRAM;
        ia.ETSDU_size = T_INVALID;
        ia.CDATA_size = T_INVALID;
        ia.DDATA_size = T_INVALID;
        ia.ADDR_size = sizeof(struct sockaddr_in);
        ia.OPT_size = UDP_REPLYSZ;
        ia.TIDU_size = UDP_MAXDGRAM;
        ia.SERV_type = T_CLTS;
        ia.CURRENT_state = ue->ue_state;
        ia.PROVIDER_flag = SENDZERO;
        if ((mp = udp_reply(mp, sizeof(ia), M_PCPROTO)) == NULL)
                return;
        memcpy(mp->b_rptr, &ia, sizeof(ia));
        qreply(q, mp);
}

/*
 * T_CONN_REQ, connectionless: it names the peer, which write's data
 * goes to and which alone may send to us. T_OK_ACK is the only answer
 * (there is no T_CONN_CON).
 */
static void
udp_connect(queue_t *q, mblk_t *mp)
{
        struct udp_ep *ue = q->q_ptr;
        struct T_conn_req cr;
        struct sockaddr_in sin;
        int error;

        if (MBLKL(mp) < sizeof(cr)) {
                udp_errack(q, mp, T_CONN_REQ, TSYSERR, EINVAL);
                return;
        }
        memcpy(&cr, mp->b_rptr, sizeof(cr));
        if (ue->ue_state != TS_IDLE) {
                udp_errack(q, mp, T_CONN_REQ, TOUTSTATE, 0);
                return;
        }
        if ((error = udp_addr(mp, cr.DEST_length, cr.DEST_offset,
            &sin)) != 0 || sin.sin_port == 0 ||
            sin.sin_addr.s_addr == INADDR_ANY) {
                udp_errack(q, mp, T_CONN_REQ, TBADADDR, 0);
                return;
        }
        if (sin.sin_addr.s_addr == INADDR_BROADCAST && !ue->ue_broadcast) {
                udp_errack(q, mp, T_CONN_REQ, TACCES, 0);
                return;
        }
        ue->ue_faddr = sin.sin_addr.s_addr;
        ue->ue_fport = sin.sin_port;
        ue->ue_conn = 1;
        udp_okack(q, mp, T_CONN_REQ);
}

/* --- Datagrams. --- */

/* data (freed) to dst:dport from ue, by way of IP. */
static void
udp_output(struct udp_ep *ue, in_addr_t dst, in_port_t dport, mblk_t *data)
{
        size_t len = data != NULL ? msgdsize(data) : 0;
        struct ip *ip;
        uint8_t *uh;
        mblk_t *hp;

        if (udp_lower == NULL || len > UDP_MAXDGRAM ||
            !canputnext(udp_lower) ||
            (hp = allocb(IP_MINHLEN + UDP_HLEN, BPRI_MED)) == NULL) {
                if (data != NULL)
                        freemsg(data);  /* no way out: dropped */
                return;
        }
        memset(hp->b_wptr, 0, IP_MINHLEN + UDP_HLEN);
        ip = (struct ip *)hp->b_wptr;
        ip->ip_vhl = 0x45;
        ip->ip_p = IPPROTO_UDP;
        ip->ip_src = ue->ue_laddr;      /* IP's choice, if INADDR_ANY */
        ip->ip_dst = dst;
        uh = hp->b_wptr + IP_MINHLEN;
        memcpy(uh, &ue->ue_lport, 2);
        memcpy(uh + 2, &dport, 2);
        uh[4] = (uint8_t)((UDP_HLEN + len) >> 8);
        uh[5] = (uint8_t)(UDP_HLEN + len);
        hp->b_wptr += IP_MINHLEN + UDP_HLEN;
        hp->b_cont = data;
        (void)putnext(udp_lower, hp);
}

static void
udp_unitdata(queue_t *q, mblk_t *mp)
{
        struct udp_ep *ue = q->q_ptr;
        struct T_unitdata_req ur;
        struct sockaddr_in sin;
        mblk_t *data;

        if (MBLKL(mp) < sizeof(ur) || ue->ue_state != TS_IDLE) {
                freemsg(mp);
                return;
        }
        memcpy(&ur, mp->b_rptr, sizeof(ur));
        /* A broadcast only with SO_BROADCAST (the socket says EACCES). */
        if (udp_addr(mp, ur.DEST_length, ur.DEST_offset, &sin) != 0 ||
            sin.sin_port == 0 || (sin.sin_addr.s_addr == INADDR_BROADCAST &&
            !ue->ue_broadcast)) {
                freemsg(mp);
                return;
        }
        data = mp->b_cont;
        mp->b_cont = NULL;
        freeb(mp);
        udp_output(ue, sin.sin_addr.s_addr, sin.sin_port, data);
}

/* The stream head's high-water mark: what SO_RCVBUF says. */
static void
udp_sethiwat(struct udp_ep *ue)
{
        struct stroptions *so;
        mblk_t *mp;

        if ((mp = allocb(sizeof(*so), BPRI_MED)) == NULL)
                return;
        mp->b_datap->db_type = M_SETOPTS;
        so = (struct stroptions *)mp->b_wptr;
        memset(so, 0, sizeof(*so));
        so->so_flags = SO_HIWAT;
        so->so_hiwat = ue->ue_rcvbuf;
        mp->b_wptr += sizeof(*so);
        (void)putnext(ue->ue_rq, mp);
}

/* One option of an endpoint's (tpi_optfn). */
static int
udp_optfn(void *arg, int mgmt, int level, int name, const void *val,
    size_t len, void *out, size_t *outlen)
{
        struct udp_ep *ue = arg;
        int set = mgmt == T_NEGOTIATE, v = 0;

        if (level != SOL_SOCKET)
                return T_NOTSUPPORT;
        if (set && tpi_optint(val, len, &v) != 0 && (name == SO_REUSEADDR ||
            name == SO_BROADCAST || name == SO_SNDBUF || name == SO_RCVBUF))
                return T_FAILURE;
        switch (name) {
        case SO_REUSEADDR:
                if (set)
                        ue->ue_reuse = v != 0;
                v = ue->ue_reuse;
                break;
        case SO_BROADCAST:
                if (set)
                        ue->ue_broadcast = v != 0;
                v = ue->ue_broadcast;
                break;
        case SO_SNDBUF:
        case SO_RCVBUF: {
                uint32_t *p = name == SO_SNDBUF ? &ue->ue_sndbuf :
                    &ue->ue_rcvbuf;

                if (set) {
                        *p = v < UDP_MINBUF ? UDP_MINBUF : v > UDP_MAXBUF ?
                            UDP_MAXBUF : (uint32_t)v;
                        if (name == SO_RCVBUF)
                                udp_sethiwat(ue);
                }
                v = (int)*p;
                break;
        }
        default:
                return T_NOTSUPPORT;
        }
        tpi_optout(v, out, outlen);
        return T_SUCCESS;
}

static void
udp_tpi(queue_t *q, mblk_t *mp)
{
        struct udp_ep *ue = q->q_ptr;
        t_scalar_t prim;

        if (MBLKL(mp) < sizeof(prim)) {
                freemsg(mp);
                return;
        }
        memcpy(&prim, mp->b_rptr, sizeof(prim));
        switch (prim) {
        case T_INFO_REQ:
                udp_info(q, mp);
                return;
        case T_BIND_REQ:
                udp_bind(q, mp);
                return;
        case T_UNBIND_REQ:
                if (ue->ue_state != TS_IDLE) {
                        udp_errack(q, mp, prim, TOUTSTATE, 0);
                        return;
                }
                ue->ue_state = TS_UNBND;
                ue->ue_conn = 0;
                udp_okack(q, mp, prim);
                return;
        case T_CONN_REQ:
                udp_connect(q, mp);
                return;
        case T_DISCON_REQ:
                if (!ue->ue_conn) {
                        udp_errack(q, mp, prim, TOUTSTATE, 0);
                        return;
                }
                ue->ue_conn = 0;
                udp_okack(q, mp, prim);
                return;
        case T_UNITDATA_REQ:
                udp_unitdata(q, mp);
                return;
        case T_OPTMGMT_REQ:
                tpi_optmgmt(q, mp, udp_optfn, ue);
                return;
        default:
                udp_errack(q, mp, prim, TNOTSUPPORT, 0);
                return;
        }
}

/* A datagram from IP, its IP header first. Frees mp. */
static void
udp_input(mblk_t *mp)
{
        struct udp_ep *best = NULL;
        struct T_unitdata_ind ind;
        struct sockaddr_in sin;
        in_addr_t src, dst;
        in_port_t sport, dport;
        size_t hlen, len, ulen;
        struct ip *ip;
        uint8_t *uh;
        int score = -1, bcast;

        if (!pullupmsg(mp, IP_MINHLEN))
                goto drop;
        ip = (struct ip *)mp->b_rptr;
        hlen = IP_HLEN(ip);
        len = msgdsize(mp);
        if (hlen < IP_MINHLEN || len < hlen + UDP_HLEN ||
            !pullupmsg(mp, (ssize_t)(hlen + UDP_HLEN)))
                goto drop;
        ip = (struct ip *)mp->b_rptr;
        uh = mp->b_rptr + hlen;
        ulen = (size_t)uh[4] << 8 | uh[5];
        if (ulen < UDP_HLEN || ulen > len - hlen)
                goto drop;
        if (ulen < len - hlen)
                (void)adjmsg(mp, -(ssize_t)(len - hlen - ulen));
        src = ip->ip_src;
        dst = ip->ip_dst;
        if ((uh[6] != 0 || uh[7] != 0) && in_fold(in_sum(mp, hlen, ulen,
            in_pseudo(src, dst, IPPROTO_UDP, ulen))) != 0)
                goto drop;
        memcpy(&sport, uh, 2);
        memcpy(&dport, uh + 2, 2);
        bcast = dst == INADDR_BROADCAST;

        /* The most specific endpoint: connected, then bound to dst. */
        for (struct udp_ep *ue = udp_eps; ue != NULL; ue = ue->ue_next) {
                int s;

                if (ue->ue_state != TS_IDLE || ue->ue_lport != dport ||
                    (ue->ue_laddr != INADDR_ANY && ue->ue_laddr != dst) ||
                    (ue->ue_conn && (ue->ue_faddr != src ||
                    ue->ue_fport != sport)))
                        continue;
                s = (ue->ue_laddr != INADDR_ANY) + 2 * ue->ue_conn;
                if (s > score) {
                        score = s;
                        best = ue;
                }
        }
        if (best == NULL) {
                mblk_t *cp;
                struct ip_ctl ic = { IPCTL_UNREACH, ICMP_UNREACH_PORT, 0 };

                /* IP sends no answer to a broadcast. */
                if (bcast || udp_lower == NULL ||
                    (cp = allocb(sizeof(ic), BPRI_MED)) == NULL)
                        goto drop;
                cp->b_datap->db_type = M_CTL;
                memcpy(cp->b_wptr, &ic, sizeof(ic));
                cp->b_wptr += sizeof(ic);
                cp->b_cont = mp;
                (void)putnext(udp_lower, cp);
                return;
        }

        memset(&ind, 0, sizeof(ind));
        ind.PRIM_type = T_UNITDATA_IND;
        ind.SRC_length = sizeof(sin);
        ind.SRC_offset = sizeof(ind);
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = sport;
        sin.sin_addr.s_addr = src;
        mp->b_rptr += hlen + UDP_HLEN;

        /* A broadcast goes to every endpoint it would match. */
        for (struct udp_ep *ue = bcast ? udp_eps : best; ue != NULL;
            ue = bcast ? ue->ue_next : NULL) {
                mblk_t *imp, *dp;

                if (bcast && (ue->ue_state != TS_IDLE ||
                    ue->ue_lport != dport || ue->ue_laddr != INADDR_ANY ||
                    (ue->ue_conn && (ue->ue_faddr != src ||
                    ue->ue_fport != sport))))
                        continue;
                if (!canputnext(ue->ue_rq))
                        continue;       /* full: dropped */
                if ((imp = allocb(sizeof(ind) + sizeof(sin), BPRI_MED)) ==
                    NULL)
                        continue;
                if ((dp = dupmsg(mp)) == NULL) {
                        freeb(imp);
                        continue;
                }
                imp->b_datap->db_type = M_PROTO;
                memcpy(imp->b_wptr, &ind, sizeof(ind));
                memcpy(imp->b_wptr + sizeof(ind), &sin, sizeof(sin));
                imp->b_wptr += sizeof(ind) + sizeof(sin);
                imp->b_cont = dp;
                (void)putnext(ue->ue_rq, imp);
        }
drop:
        freemsg(mp);
}

/* --- The streams. --- */

static int
udp_open(queue_t *q, dev_t *devp __unused, int flag __unused, int sflag,
    struct cred *cr)
{
        struct udp_ep *ue;

        if (sflag != CLONEOPEN)
                return ENXIO;
        ue = kmem_zalloc(sizeof(*ue), KM_SLEEP);
        ue->ue_rq = q;
        ue->ue_state = TS_UNBND;
        ue->ue_priv = CRED_ISROOT(cr);
        ue->ue_sndbuf = UDP_SNDBUF;
        ue->ue_rcvbuf = UDP_RCVBUF;
        ue->ue_next = udp_eps;
        udp_eps = ue;
        q->q_ptr = WR(q)->q_ptr = ue;
        udp_sethiwat(ue);
        return 0;
}

static int
udp_close(queue_t *q, int flag __unused, struct cred *cr __unused)
{
        struct udp_ep *ue = q->q_ptr, **link;

        for (link = &udp_eps; *link != NULL; link = &(*link)->ue_next)
                if (*link == ue) {
                        *link = ue->ue_next;
                        break;
                }
        kmem_free(ue, sizeof(*ue));
        q->q_ptr = WR(q)->q_ptr = NULL;
        return 0;
}

/* Ask the IP stream just linked for the datagrams of protocol 17. */
static void
udp_bindproto(void)
{
        struct iocblk *ioc;
        int32_t proto = IPPROTO_UDP;
        mblk_t *mp, *dp;

        if ((mp = allocb(sizeof(*ioc), BPRI_HI)) == NULL)
                return;
        if ((dp = allocb(sizeof(proto), BPRI_HI)) == NULL) {
                freeb(mp);
                return;
        }
        mp->b_datap->db_type = M_IOCTL;
        ioc = (struct iocblk *)mp->b_wptr;
        memset(ioc, 0, sizeof(*ioc));
        ioc->ioc_cmd = IP_BINDPROTO;
        ioc->ioc_cr = kcred;
        ioc->ioc_count = sizeof(proto);
        mp->b_wptr += sizeof(*ioc);
        memcpy(dp->b_wptr, &proto, sizeof(proto));
        dp->b_wptr += sizeof(proto);
        mp->b_cont = dp;
        (void)putnext(udp_lower, mp);
}

static void
udp_ioctl(queue_t *q, mblk_t *mp)
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
        struct linkblk *lb = NULL;
        int error = 0;

        if (ioc->ioc_count != TRANSPARENT && mp->b_cont != NULL &&
            pullupmsg(mp->b_cont, -1) && MBLKL(mp->b_cont) >= sizeof(*lb))
                lb = (struct linkblk *)mp->b_cont->b_rptr;
        switch (ioc->ioc_cmd) {
        case I_LINK:
        case I_PLINK:
                if (lb == NULL)
                        error = EINVAL;
                else if (ioc->ioc_cr == NULL || !CRED_ISROOT(ioc->ioc_cr))
                        error = EPERM;
                else if (udp_lower != NULL)
                        error = EBUSY;          /* one IP stream */
                if (error != 0)
                        break;
                udp_lower = lb->l_qbot;
                udp_muxid = lb->l_index;
                udp_bindproto();
                break;
        case I_UNLINK:
        case I_PUNLINK:
                if (lb == NULL)
                        error = EINVAL;
                else if (udp_lower != NULL && lb->l_index == udp_muxid)
                        udp_lower = NULL;
                break;
        default:
                error = EINVAL;
                break;
        }
        if (error != 0) {
                miocnak(q, mp, 0, error);
                return;
        }
        if (mp->b_cont != NULL) {
                freemsg(mp->b_cont);
                mp->b_cont = NULL;
        }
        miocack(q, mp, 0, 0);
}

static int
udp_uwput(queue_t *q, mblk_t *mp)
{
        struct udp_ep *ue = q->q_ptr;

        switch (DB_TYPE(mp)) {
        case M_PROTO:
        case M_PCPROTO:
                udp_tpi(q, mp);
                return 0;
        case M_DATA:
                /* write(2): to the peer, if there is one. */
                if (ue->ue_state == TS_IDLE && ue->ue_conn)
                        udp_output(ue, ue->ue_faddr, ue->ue_fport, mp);
                else
                        freemsg(mp);
                return 0;
        case M_IOCTL:
                udp_ioctl(q, mp);
                return 0;
        case M_FLUSH:
                if ((*mp->b_rptr & FLUSHR) != 0) {
                        *mp->b_rptr &= ~FLUSHW;
                        qreply(q, mp);
                } else {
                        freemsg(mp);
                }
                return 0;
        default:
                freemsg(mp);
                return 0;
        }
}

/*
 * An ICMP error from IP (IPCTL_ICMPERR) about a datagram of ours: to
 * the endpoint connected to where it went, as a T_UDERROR_IND, which the
 * socket's next call reports; an endpoint not connected hears nothing
 * (RFC 1122 4.1.3.3 leaves it to the application; the sockets of the
 * BSDs and others tell only a connected one). Port and protocol
 * unreachable are ECONNREFUSED; the other unreachables, EHOSTUNREACH
 * (ENETUNREACH for a network). Time exceeded and parameter problems
 * are passed over, as is a need-frag: UDP leaves fragmenting to IP.
 * Frees mp.
 */
static void
udp_icmp(mblk_t *mp)
{
        struct ip_ctl ic;
        mblk_t *dp = mp->b_cont;
        const struct ip *ip;
        const uint8_t *uh;
        in_port_t sport, dport;
        int error;

        if (MBLKL(mp) < sizeof(ic) || dp == NULL ||
            !pullupmsg(dp, -1) || MBLKL(dp) < IP_MINHLEN)
                goto drop;
        memcpy(&ic, mp->b_rptr, sizeof(ic));
        ip = (const struct ip *)dp->b_rptr;
        if (ic.ic_cmd != IPCTL_ICMPERR || ip->ip_p != IPPROTO_UDP ||
            MBLKL(dp) < (size_t)IP_HLEN(ip) + UDP_HLEN ||
            (ic.ic_arg >> 8) != ICMP_UNREACH)
                goto drop;
        switch (ic.ic_arg & 0xff) {
        case ICMP_UNREACH_PROTOCOL:
        case ICMP_UNREACH_PORT:
                error = ECONNREFUSED;
                break;
        case ICMP_UNREACH_NEEDFRAG:
                goto drop;
        case 0:                         /* the network */
                error = ENETUNREACH;
                break;
        default:
                error = EHOSTUNREACH;
                break;
        }
        uh = dp->b_rptr + IP_HLEN(ip);
        memcpy(&sport, uh, 2);
        memcpy(&dport, uh + 2, 2);
        for (struct udp_ep *ue = udp_eps; ue != NULL; ue = ue->ue_next) {
                struct T_uderror_ind ind;
                struct sockaddr_in sin;
                mblk_t *ep;

                if (ue->ue_state != TS_IDLE || !ue->ue_conn ||
                    ue->ue_lport != sport || ue->ue_fport != dport ||
                    ue->ue_faddr != ip->ip_dst ||
                    (ue->ue_laddr != INADDR_ANY &&
                    ue->ue_laddr != ip->ip_src))
                        continue;
                if (!canputnext(ue->ue_rq) || (ep = allocb(sizeof(ind) +
                    sizeof(sin), BPRI_MED)) == NULL)
                        break;
                memset(&ind, 0, sizeof(ind));
                ind.PRIM_type = T_UDERROR_IND;
                ind.DEST_length = sizeof(sin);
                ind.DEST_offset = sizeof(ind);
                ind.ERROR_type = error;
                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
                sin.sin_port = dport;
                sin.sin_addr.s_addr = ip->ip_dst;
                ep->b_datap->db_type = M_PROTO;
                memcpy(ep->b_wptr, &ind, sizeof(ind));
                memcpy(ep->b_wptr + sizeof(ind), &sin, sizeof(sin));
                ep->b_wptr += sizeof(ind) + sizeof(sin);
                (void)putnext(ue->ue_rq, ep);
                break;
        }
drop:
        freemsg(mp);
}

/* Up from the IP stream. */
static int
udp_lrput(queue_t *q, mblk_t *mp)
{
        switch (DB_TYPE(mp)) {
        case M_DATA:
                if (WR(q) == udp_lower)
                        udp_input(mp);
                else
                        freemsg(mp);
                return 0;
        case M_CTL:
                if (WR(q) == udp_lower)
                        udp_icmp(mp);
                else
                        freemsg(mp);
                return 0;
        case M_IOCNAK:
                printf("udp: IP refused protocol %d\n", IPPROTO_UDP);
                freemsg(mp);
                return 0;
        case M_FLUSH:
                if ((*mp->b_rptr & FLUSHW) != 0) {
                        *mp->b_rptr &= ~FLUSHR;
                        qreply(q, mp);
                } else {
                        freemsg(mp);
                }
                return 0;
        default:
                freemsg(mp);            /* the IOCACK, among others */
                return 0;
        }
}

static int
udp_lwput(queue_t *q, mblk_t *mp)
{
        (void)putnext(q, mp);
        return 0;
}
