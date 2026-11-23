/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * TCP (RFC 9293) as a multiplexing STREAMS driver, /dev/tcp, and a
 * connection-oriented TPI provider with orderly release (T_COTS_ORD) to
 * its upper streams. Its one lower stream is an IP stream linked by
 * I_PLINK, which TCP binds to protocol 6; IP fills in the source of a
 * segment sent before it is known, and computes the checksum.
 *
 * A connection's state is a tcb. A stream has at most one: bound,
 * listening or connected. A connection a listener takes is a child of
 * its tcb until T_CONN_RES gives it to the acceptor stream; one whose
 * stream has closed is an orphan, which finishes closing by itself.
 * Every queue of TCP's and its timer (a tick every 100 ms, on the lower
 * stream) are in one syncq (SQLVL_MODULE), which guards all of it.
 *
 * Options come by T_OPTMGMT_REQ: a stream's, kept in its tcp_up until
 * it binds, then in its tcb too; a child takes its listener's, and the
 * stream that accepts it takes them from the child.
 *
 * Not yet: window scaling, timestamps, SACK, fast retransmit, the
 * queueing of segments that arrive out of order (they are dropped and
 * acknowledged at rcv_nxt), Nagle's algorithm and delayed ACKs, urgent
 * data, and ICMP errors. TCP_NODELAY is kept and reported, but changes
 * nothing: without Nagle's algorithm, data goes as the window allows.
 * See doc/design/decisions.md, "TCP and stream sockets over TPI" and
 * "Socket options, and connecting without waiting".
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/ipconf.h>
#include <sys/kmem.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/systm.h>
#include <sys/tihdr.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/tcp.h>

#define TCP_HLEN        20
#define TCP_MSS         1460    /* what we take: an Ethernet's */
#define TCP_DEFMSS      536     /* what a peer takes without saying */
#define TCP_SNDBUF      65536
#define TCP_RCVBUF      65535   /* no window scaling: the largest window */
#define TCP_MINBUF      2048    /* the least SO_SNDBUF and SO_RCVBUF */
#define TCP_MAXSNDBUF   262144
#define TCP_BACKLOG     128
#define TCP_REPLYSZ     64

/* Times, in ticks of TCP's timer, ten a second. */
#define TCP_TICK        (HZ / 10)
#define TCP_RTOINIT     10
#define TCP_RTOMIN      2
#define TCP_RTOMAX      600
#define TCP_MAXRXT      12      /* retransmissions before giving up */
#define TCP_MAXSYNRXT   6
#define TCP_2MSL        600     /* TIME_WAIT: twice a 30-second MSL */
#define TCP_FW2ORPHAN   600     /* an orphan's FIN_WAIT_2, at most */
#define TCP_KEEPIDLE_DEF 7200   /* keep-alives: seconds idle before them */
#define TCP_KEEPINTVL_DEF 75    /* seconds between them */
#define TCP_KEEPCNT_DEF 8       /* unanswered before giving up */
#define TCP_KEEPMAX     32767   /* the most seconds for either */

#define TCP_PORT_FIRST  49152
#define TCP_PORT_LAST   65535

#define TH_FIN          0x01
#define TH_SYN          0x02
#define TH_RST          0x04
#define TH_PSH          0x08
#define TH_ACK          0x10

enum {
        TCPS_CLOSED, TCPS_LISTEN, TCPS_SYN_SENT, TCPS_SYN_RCVD,
        TCPS_ESTABLISHED, TCPS_CLOSE_WAIT, TCPS_FIN_WAIT_1, TCPS_CLOSING,
        TCPS_LAST_ACK, TCPS_FIN_WAIT_2, TCPS_TIME_WAIT
};

#define TCPS_SYNCED(s)  ((s) >= TCPS_ESTABLISHED)

#define SEQ_LT(a, b)    ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)   ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)    ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b)   ((int32_t)((a) - (b)) >= 0)

#define TF_ACKNOW       0x0001  /* send an ACK at the next output */
#define TF_FORCE        0x0002  /* a window probe: one byte past it */

struct tcb;

/* A stream's options. */
struct tcp_opt {
        int             to_reuseaddr;
        int             to_keepalive;
        int             to_keepidle;    /* seconds */
        int             to_keepintvl;   /* seconds */
        int             to_keepcnt;
        int             to_linger;      /* SO_LINGER's l_onoff */
        int             to_lingertime;  /* and its l_linger */
        int             to_nodelay;
        uint32_t        to_sndbuf;
        uint32_t        to_rcvbuf;
};

static const struct tcp_opt tcp_defopt = {
        .to_keepidle = TCP_KEEPIDLE_DEF,
        .to_keepintvl = TCP_KEEPINTVL_DEF,
        .to_keepcnt = TCP_KEEPCNT_DEF,
        .to_sndbuf = TCP_SNDBUF,
        .to_rcvbuf = TCP_RCVBUF,
};

/* An upper stream. */
struct tcp_up {
        struct tcp_up   *tu_next;
        queue_t         *tu_rq;
        uint32_t        tu_id;          /* the ACCEPTOR_id */
        int             tu_priv;
        struct tcb      *tu_tcb;        /* NULL: unbound */
        struct tcp_opt  tu_opt;
};

struct tcb {
        struct tcb      *tc_next;
        struct tcp_up   *tc_up;         /* NULL: a child, or an orphan */
        struct tcb      *tc_listener;   /* a child: whose */
        struct tcb      *tc_children;   /* a listener's, by tc_sibling */
        struct tcb      *tc_sibling;
        int             tc_nchildren;
        int             tc_backlog;
        t_scalar_t      tc_seqno;       /* a child's, for T_CONN_IND */
        int             tc_indicated;   /* T_CONN_IND has gone up */
        int             tc_state;
        int             tc_wasconn;     /* has been used to connect */
        int             tc_flags;
        in_addr_t       tc_laddr;       /* network order, as the ports */
        in_addr_t       tc_faddr;
        in_port_t       tc_lport;
        in_port_t       tc_fport;

        uint32_t        tc_iss;
        uint32_t        tc_snd_una;
        uint32_t        tc_snd_nxt;
        uint32_t        tc_snd_max;
        uint32_t        tc_snd_wl1;
        uint32_t        tc_snd_wl2;
        uint32_t        tc_snd_wnd;
        uint32_t        tc_sndbase;     /* the sequence of tc_sndq's first */
        mblk_t          *tc_sndq;       /* unacknowledged and unsent data */
        size_t          tc_sndcc;
        int             tc_fin;         /* our FIN follows the data */
        int             tc_finacked;
        uint32_t        tc_cwnd;
        uint32_t        tc_ssthresh;
        uint32_t        tc_mss;         /* the peer's */

        uint32_t        tc_irs;
        uint32_t        tc_rcv_nxt;
        uint32_t        tc_rcv_adv;     /* the window's right edge, as sent */
        mblk_t          *tc_rcvq;       /* for the stream, by b_next */
        mblk_t          *tc_rcvtail;
        size_t          tc_rcvcc;       /* the data bytes there */

        int             tc_rexmt;       /* ticks to retransmission; 0 off */
        int             tc_rxtshift;
        int             tc_rto;
        int             tc_rtt;         /* ticks+1 while timing; 0 not */
        uint32_t        tc_rtseq;
        int             tc_rttvalid;
        int             tc_srtt;        /* scaled by 8 */
        int             tc_rttvar;      /* scaled by 4 */
        int             tc_timer2;      /* TIME_WAIT, an orphan's FIN_WAIT_2 */
        int             tc_idle;        /* ticks since a segment came */
        int             tc_kprobes;     /* keep-alives unanswered */
        struct tcp_opt  tc_opt;
};

/* A segment that has arrived. */
struct tcp_seg {
        in_addr_t       ts_src;
        in_addr_t       ts_dst;
        in_port_t       ts_sport;
        in_port_t       ts_dport;
        uint32_t        ts_seq;
        uint32_t        ts_ack;
        uint8_t         ts_flags;
        uint32_t        ts_win;
        uint32_t        ts_mss;         /* its option, or 0 */
        size_t          ts_len;         /* data bytes */
};

static struct tcp_up *tcp_ups;
static struct tcb *tcp_tcbs;
static queue_t *tcp_lower;              /* the IP stream's l_qbot */
static int tcp_muxid;
static int64_t tcp_timer;
static uint32_t tcp_nextid = 1;
static t_scalar_t tcp_nextseqno = 1;
static uint16_t tcp_nextport = TCP_PORT_FIRST;
static uint32_t tcp_issseed;

static int tcp_open(queue_t *, dev_t *, int, int, struct cred *);
static int tcp_close(queue_t *, int, struct cred *);
static int tcp_uwput(queue_t *, mblk_t *);
static int tcp_uwsrv(queue_t *);
static int tcp_ursrv(queue_t *);
static int tcp_lrput(queue_t *, mblk_t *);
static int tcp_lwput(queue_t *, mblk_t *);
static void tcp_output(struct tcb *);

static struct module_info tcp_minfo = {
        0x5443, "tcp", 0, INFPSZ, 65536, 16384
};
static struct qinit tcp_urinit = {
        NULL, tcp_ursrv, tcp_open, tcp_close, NULL, &tcp_minfo, NULL
};
static struct qinit tcp_uwinit = {
        tcp_uwput, tcp_uwsrv, NULL, NULL, NULL, &tcp_minfo, NULL
};
static struct qinit tcp_lrinit = {
        tcp_lrput, NULL, NULL, NULL, NULL, &tcp_minfo, NULL
};
static struct qinit tcp_lwinit = {
        tcp_lwput, NULL, NULL, NULL, NULL, &tcp_minfo, NULL
};

struct streamtab tcpinfo = {
        &tcp_urinit, &tcp_uwinit, &tcp_lrinit, &tcp_lwinit
};

/* --- TPI replies and indications. --- */

/* A reply block in mp itself when it can be, as UDP's udp_reply. */
static mblk_t *
tcp_reply(mblk_t *mp, size_t size, unsigned char type)
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
tcp_errack(queue_t *q, mblk_t *mp, t_scalar_t prim, t_scalar_t tli,
    t_scalar_t unixerr)
{
        struct T_error_ack ea = { T_ERROR_ACK, prim, tli, unixerr };

        if ((mp = tcp_reply(mp, sizeof(ea), M_PCPROTO)) == NULL)
                return;
        memcpy(mp->b_rptr, &ea, sizeof(ea));
        qreply(q, mp);
}

static void
tcp_okack(queue_t *q, mblk_t *mp, t_scalar_t prim)
{
        struct T_ok_ack ok = { T_OK_ACK, prim };

        if ((mp = tcp_reply(mp, sizeof(ok), M_PCPROTO)) == NULL)
                return;
        memcpy(mp->b_rptr, &ok, sizeof(ok));
        qreply(q, mp);
}

static void
tcp_sin(struct sockaddr_in *sin, in_addr_t addr, in_port_t port)
{
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_port = port;
        sin->sin_addr.s_addr = addr;
}

/* An M_PROTO block of size bytes (zeroed) with a sockaddr_in after. */
static mblk_t *
tcp_ind(size_t size, int withaddr, in_addr_t addr, in_port_t port)
{
        size_t total = size + (withaddr ? sizeof(struct sockaddr_in) : 0);
        mblk_t *mp = allocb(total, BPRI_HI);

        if (mp == NULL)
                return NULL;
        mp->b_datap->db_type = M_PROTO;
        memset(mp->b_wptr, 0, total);
        if (withaddr) {
                struct sockaddr_in sin;

                tcp_sin(&sin, addr, port);
                memcpy(mp->b_wptr + size, &sin, sizeof(sin));
        }
        mp->b_wptr += total;
        return mp;
}

static mblk_t *
tcp_discon_ind(int reason, t_scalar_t seqno)
{
        struct T_discon_ind di = { T_DISCON_IND, reason, seqno };
        mblk_t *mp = tcp_ind(sizeof(di), 0, 0, 0);

        if (mp != NULL)
                memcpy(mp->b_rptr, &di, sizeof(di));
        return mp;
}

/* --- Endpoints. --- */

static int
tcp_addr(const mblk_t *mp, t_scalar_t len, t_scalar_t off,
    struct sockaddr_in *sin)
{
        size_t have = MBLKL(mp);

        memset(sin, 0, sizeof(*sin));
        if (len < (t_scalar_t)offsetof(struct sockaddr_in, sin_zero) ||
            off < 0 || (size_t)off > have || (size_t)len > have - (size_t)off)
                return TBADADDR;
        memcpy(sin, mp->b_rptr + off, (size_t)len < sizeof(*sin) ?
            (size_t)len : sizeof(*sin));
        return sin->sin_family == AF_INET ? 0 : TBADADDR;
}

static uint32_t
tcp_newiss(void)
{
        tcp_issseed += 64000;
        return tcp_issseed + (uint32_t)lbolt * 2500;
}

static struct tcb *
tcp_newtcb(void)
{
        struct tcb *tc = kmem_zalloc(sizeof(*tc), KM_NOSLEEP);

        if (tc == NULL)
                return NULL;
        tc->tc_rto = TCP_RTOINIT;
        tc->tc_mss = TCP_DEFMSS;
        tc->tc_opt = tcp_defopt;
        tc->tc_next = tcp_tcbs;
        tcp_tcbs = tc;
        return tc;
}

/* Fresh sequence state for a connection about to begin. */
static void
tcp_begin(struct tcb *tc)
{
        tc->tc_iss = tcp_newiss();
        tc->tc_snd_una = tc->tc_snd_nxt = tc->tc_snd_max = tc->tc_iss;
        tc->tc_sndbase = tc->tc_iss + 1;
        tc->tc_cwnd = 4 * tc->tc_mss;
        tc->tc_ssthresh = TCP_RCVBUF;
        tc->tc_rto = TCP_RTOINIT;
        tc->tc_rxtshift = 0;
        tc->tc_rttvalid = 0;
        tc->tc_rtt = 0;
        tc->tc_fin = tc->tc_finacked = 0;
        tc->tc_wasconn = 1;
}

static void
tcp_flushq(mblk_t **headp)
{
        mblk_t *mp;

        while ((mp = *headp) != NULL) {
                *headp = mp->b_next;
                mp->b_next = NULL;
                freemsg(mp);
        }
}

/* Forget a connection's data and timers. */
static void
tcp_clear(struct tcb *tc)
{
        if (tc->tc_sndq != NULL)
                freemsg(tc->tc_sndq);
        tc->tc_sndq = NULL;
        tc->tc_sndcc = 0;
        tcp_flushq(&tc->tc_rcvq);
        tc->tc_rcvtail = NULL;
        tc->tc_rcvcc = 0;
        tc->tc_rexmt = tc->tc_timer2 = tc->tc_rtt = 0;
        tc->tc_flags = 0;
}

static void
tcp_unchild(struct tcb *tc)
{
        struct tcb *l = tc->tc_listener, **link;

        if (l == NULL)
                return;
        for (link = &l->tc_children; *link != NULL;
            link = &(*link)->tc_sibling)
                if (*link == tc) {
                        *link = tc->tc_sibling;
                        l->tc_nchildren--;
                        break;
                }
        tc->tc_listener = NULL;
        tc->tc_sibling = NULL;
}

static void
tcp_free(struct tcb *tc)
{
        struct tcb **link;

        for (link = &tcp_tcbs; *link != NULL; link = &(*link)->tc_next)
                if (*link == tc) {
                        *link = tc->tc_next;
                        break;
                }
        tcp_unchild(tc);
        tcp_clear(tc);
        if (tc->tc_up != NULL)
                tc->tc_up->tu_tcb = NULL;
        kmem_free(tc, sizeof(*tc));
}

/*
 * Whether binding addr:port (network order) would clash: with a
 * listener or a bound socket always, and with a connection, ended or
 * not (in TIME_WAIT, say), unless the binder has SO_REUSEADDR.
 */
static int
tcp_inuse(in_addr_t addr, in_port_t port, int reuse)
{
        for (struct tcb *tc = tcp_tcbs; tc != NULL; tc = tc->tc_next) {
                if (tc->tc_lport != port || (tc->tc_laddr != INADDR_ANY &&
                    addr != INADDR_ANY && tc->tc_laddr != addr))
                        continue;
                if (tc->tc_state == TCPS_LISTEN ||
                    (tc->tc_state == TCPS_CLOSED && !tc->tc_wasconn) ||
                    !reuse)
                        return 1;
        }
        return 0;
}

static int
tcp_portfree(in_port_t port)
{
        for (struct tcb *tc = tcp_tcbs; tc != NULL; tc = tc->tc_next)
                if (tc->tc_lport == port)
                        return 0;
        return 1;
}

static struct tcp_up *
tcp_up_byid(uint32_t id)
{
        for (struct tcp_up *tu = tcp_ups; tu != NULL; tu = tu->tu_next)
                if (tu->tu_id == id)
                        return tu;
        return NULL;
}

/* --- Sending. --- */

static uint32_t
tcp_window(const struct tcb *tc)
{
        size_t buf = tc->tc_opt.to_rcvbuf;
        uint32_t win = tc->tc_rcvcc >= buf ? 0 :
            (uint32_t)(buf - tc->tc_rcvcc);

        /* A smaller SO_RCVBUF takes back no window already offered. */
        if (SEQ_GT(tc->tc_rcv_adv, tc->tc_rcv_nxt) &&
            tc->tc_rcv_adv - tc->tc_rcv_nxt > win)
                win = tc->tc_rcv_adv - tc->tc_rcv_nxt;
        return win;
}

/*
 * A segment, by way of IP: data (freed) after the header, opt (optlen
 * bytes, a multiple of 4) among its options.
 */
static void
tcp_xmit(in_addr_t src, in_addr_t dst, in_port_t sport, in_port_t dport,
    uint32_t seq, uint32_t ack, uint8_t flags, uint32_t win,
    const uint8_t *opt, size_t optlen, mblk_t *data)
{
        size_t hl = IP_MINHLEN + TCP_HLEN + optlen;
        struct ip *ip;
        uint8_t *th;
        mblk_t *hp;

        if (tcp_lower == NULL || !canputnext(tcp_lower) ||
            (hp = allocb(hl, BPRI_MED)) == NULL) {
                if (data != NULL)
                        freemsg(data);  /* lost: retransmission's to mend */
                return;
        }
        memset(hp->b_wptr, 0, hl);
        ip = (struct ip *)hp->b_wptr;
        ip->ip_vhl = 0x45;
        ip->ip_p = IPPROTO_TCP;
        ip->ip_src = src;
        ip->ip_dst = dst;
        th = hp->b_wptr + IP_MINHLEN;
        memcpy(th, &sport, 2);
        memcpy(th + 2, &dport, 2);
        seq = htonl(seq);
        ack = htonl(ack);
        memcpy(th + 4, &seq, 4);
        memcpy(th + 8, &ack, 4);
        th[12] = (uint8_t)(((TCP_HLEN + optlen) / 4) << 4);
        th[13] = flags;
        if (win > 65535)
                win = 65535;
        th[14] = (uint8_t)(win >> 8);
        th[15] = (uint8_t)win;
        if (optlen > 0)
                memcpy(th + TCP_HLEN, opt, optlen);
        hp->b_wptr += hl;
        hp->b_cont = data;
        (void)putnext(tcp_lower, hp);
}

/* len bytes of tc's send queue from off, in a new message. */
static mblk_t *
tcp_copydata(const struct tcb *tc, size_t off, size_t len)
{
        mblk_t *mp = allocb(len, BPRI_MED);
        size_t done = 0;

        if (mp == NULL)
                return NULL;
        for (const mblk_t *bp = tc->tc_sndq; bp != NULL && done < len;
            bp = bp->b_cont) {
                size_t n = MBLKL(bp);

                if (off >= n) {
                        off -= n;
                        continue;
                }
                n -= off;
                if (n > len - done)
                        n = len - done;
                memcpy(mp->b_wptr + done, bp->b_rptr + off, n);
                done += n;
                off = 0;
        }
        mp->b_wptr += done;
        return mp;
}

/* A segment of tc's: seq, flags, data; with tc's ACK and window. */
static void
tcp_send(struct tcb *tc, uint32_t seq, uint8_t flags, mblk_t *data)
{
        uint32_t win = tcp_window(tc);
        uint8_t opt[4];
        size_t optlen = 0;

        if ((flags & TH_SYN) != 0) {
                opt[0] = 2;             /* MSS */
                opt[1] = 4;
                opt[2] = (uint8_t)(TCP_MSS >> 8);
                opt[3] = (uint8_t)TCP_MSS;
                optlen = 4;
        }
        if ((flags & TH_ACK) != 0 && win > 0 &&
            SEQ_GT(tc->tc_rcv_nxt + win, tc->tc_rcv_adv))
                tc->tc_rcv_adv = tc->tc_rcv_nxt + win;
        tcp_xmit(tc->tc_laddr, tc->tc_faddr, tc->tc_lport, tc->tc_fport,
            seq, (flags & TH_ACK) != 0 ? tc->tc_rcv_nxt : 0, flags, win,
            opt, optlen, data);
        tc->tc_flags &= ~TF_ACKNOW;
}

/* Send what tc may: its SYN, data and FIN, or an ACK it owes. */
static void
tcp_output(struct tcb *tc)
{
        if (tc->tc_state == TCPS_SYN_SENT || tc->tc_state == TCPS_SYN_RCVD) {
                if (tc->tc_snd_nxt == tc->tc_iss) {
                        tcp_send(tc, tc->tc_iss, tc->tc_state ==
                            TCPS_SYN_SENT ? TH_SYN : TH_SYN | TH_ACK, NULL);
                        tc->tc_snd_nxt = tc->tc_iss + 1;
                        if (SEQ_GT(tc->tc_snd_nxt, tc->tc_snd_max))
                                tc->tc_snd_max = tc->tc_snd_nxt;
                        if (tc->tc_rtt == 0) {
                                tc->tc_rtt = 1;
                                tc->tc_rtseq = tc->tc_iss;
                        }
                        if (tc->tc_rexmt == 0)
                                tc->tc_rexmt = tc->tc_rto;
                } else if ((tc->tc_flags & TF_ACKNOW) != 0 &&
                    tc->tc_state == TCPS_SYN_RCVD) {
                        /* The peer's SYN again: our SYN-ACK again. */
                        tcp_send(tc, tc->tc_iss, tc->tc_snd_una ==
                            tc->tc_iss ? TH_SYN | TH_ACK : TH_ACK, NULL);
                }
                return;
        }
        if (!TCPS_SYNCED(tc->tc_state))
                return;
        for (;;) {
                uint32_t off = tc->tc_snd_nxt - tc->tc_sndbase;
                uint32_t win = tc->tc_snd_wnd < tc->tc_cwnd ?
                    tc->tc_snd_wnd : tc->tc_cwnd;
                uint32_t flight = tc->tc_snd_nxt - tc->tc_snd_una;
                size_t avail, len;
                uint8_t flags = TH_ACK;
                mblk_t *data = NULL;
                int fin;

                avail = off <= tc->tc_sndcc ? tc->tc_sndcc - off : 0;
                len = win > flight ? win - flight : 0;
                if (len == 0 && avail > 0 && (tc->tc_flags & TF_FORCE) != 0)
                        len = 1;
                if (len > avail)
                        len = avail;
                if (len > tc->tc_mss)
                        len = tc->tc_mss;
                fin = tc->tc_fin && off <= tc->tc_sndcc &&
                    off + len == tc->tc_sndcc;
                if (len == 0 && !fin) {
                        if ((tc->tc_flags & TF_ACKNOW) != 0)
                                tcp_send(tc, tc->tc_snd_nxt, TH_ACK, NULL);
                        break;
                }
                if (len > 0) {
                        if ((data = tcp_copydata(tc, off, len)) == NULL)
                                break;
                        flags |= TH_PSH;
                }
                if (fin)
                        flags |= TH_FIN;
                if (tc->tc_rtt == 0) {
                        tc->tc_rtt = 1;
                        tc->tc_rtseq = tc->tc_snd_nxt;
                }
                tcp_send(tc, tc->tc_snd_nxt, flags, data);
                tc->tc_flags &= ~TF_FORCE;
                tc->tc_snd_nxt += (uint32_t)len + (fin ? 1 : 0);
                if (SEQ_GT(tc->tc_snd_nxt, tc->tc_snd_max))
                        tc->tc_snd_max = tc->tc_snd_nxt;
                if (tc->tc_rexmt == 0)
                        tc->tc_rexmt = tc->tc_rto;
                if (fin)
                        break;
        }
        /* Data waiting on a closed window: the timer probes it. */
        if (tc->tc_snd_wnd == 0 && tc->tc_rexmt == 0 &&
            tc->tc_snd_nxt - tc->tc_sndbase < tc->tc_sndcc)
                tc->tc_rexmt = tc->tc_rto;
}

/* A reset for a segment no connection takes (RFC 9293 3.10.7.1). */
static void
tcp_reset_reply(const struct tcp_seg *sg)
{
        if ((sg->ts_flags & TH_RST) != 0 || sg->ts_dst == INADDR_BROADCAST)
                return;
        if ((sg->ts_flags & TH_ACK) != 0)
                tcp_xmit(sg->ts_dst, sg->ts_src, sg->ts_dport, sg->ts_sport,
                    sg->ts_ack, 0, TH_RST, 0, NULL, 0, NULL);
        else
                tcp_xmit(sg->ts_dst, sg->ts_src, sg->ts_dport, sg->ts_sport,
                    0, sg->ts_seq + (uint32_t)sg->ts_len +
                    ((sg->ts_flags & TH_SYN) != 0) +
                    ((sg->ts_flags & TH_FIN) != 0), TH_RST | TH_ACK, 0, NULL,
                    0, NULL);
}

/* --- To the stream. --- */

static void
tcp_rcvappend(struct tcb *tc, mblk_t *mp)
{
        mp->b_next = NULL;
        if (tc->tc_rcvtail == NULL)
                tc->tc_rcvq = mp;
        else
                tc->tc_rcvtail->b_next = mp;
        tc->tc_rcvtail = mp;
        if (DB_TYPE(mp) == M_DATA)
                tc->tc_rcvcc += msgdsize(mp);
}

/* Pass the stream what it will take, and open the window again. */
static void
tcp_push(struct tcb *tc)
{
        queue_t *rq = tc->tc_up != NULL ? tc->tc_up->tu_rq : NULL;
        mblk_t *mp;
        uint32_t edge;

        if (rq == NULL)
                return;
        while ((mp = tc->tc_rcvq) != NULL && canputnext(rq)) {
                tc->tc_rcvq = mp->b_next;
                if (tc->tc_rcvq == NULL)
                        tc->tc_rcvtail = NULL;
                mp->b_next = NULL;
                if (DB_TYPE(mp) == M_DATA)
                        tc->tc_rcvcc -= msgdsize(mp);
                (void)putnext(rq, mp);
        }
        /* A window update, once it has grown by two segments. */
        edge = tc->tc_rcv_nxt + tcp_window(tc);
        if (TCPS_SYNCED(tc->tc_state) && tc->tc_state != TCPS_TIME_WAIT &&
            SEQ_GEQ(edge, tc->tc_rcv_adv + 2 * TCP_MSS)) {
                tc->tc_flags |= TF_ACKNOW;
                tcp_output(tc);
        }
}

/* Everything to the stream now, flow control or not (it is ending). */
static void
tcp_pushall(struct tcb *tc)
{
        queue_t *rq = tc->tc_up != NULL ? tc->tc_up->tu_rq : NULL;
        mblk_t *mp;

        while ((mp = tc->tc_rcvq) != NULL) {
                tc->tc_rcvq = mp->b_next;
                mp->b_next = NULL;
                if (rq != NULL)
                        (void)putnext(rq, mp);
                else
                        freemsg(mp);
        }
        tc->tc_rcvtail = NULL;
        tc->tc_rcvcc = 0;
}

/*
 * The connection is over, with error (to the stream, a T_DISCON_IND).
 * rst: tell the peer. An attached tcb stays, closed, bound to its port;
 * a child or an orphan is freed.
 */
static void
tcp_drop(struct tcb *tc, int error, int rst)
{
        if (rst && (TCPS_SYNCED(tc->tc_state) ||
            tc->tc_state == TCPS_SYN_RCVD) && tc->tc_state != TCPS_TIME_WAIT)
                tcp_xmit(tc->tc_laddr, tc->tc_faddr, tc->tc_lport,
                    tc->tc_fport, tc->tc_snd_nxt, tc->tc_rcv_nxt,
                    TH_RST | TH_ACK, 0, NULL, 0, NULL);
        if (tc->tc_listener != NULL) {
                struct tcb *l = tc->tc_listener;
                mblk_t *mp;

                if (tc->tc_indicated && l->tc_up != NULL &&
                    (mp = tcp_discon_ind(error, tc->tc_seqno)) != NULL)
                        (void)putnext(l->tc_up->tu_rq, mp);
                tcp_free(tc);
                return;
        }
        if (tc->tc_up == NULL) {
                tcp_free(tc);
                return;
        }
        {
                mblk_t *mp = tcp_discon_ind(error, -1);

                if (mp != NULL)
                        tcp_rcvappend(tc, mp);
        }
        tcp_pushall(tc);
        tcp_clear(tc);
        tc->tc_state = TCPS_CLOSED;
}

/* A child is established: tell its listener's stream. 0: it is gone. */
static int
tcp_indicate(struct tcb *tc)
{
        struct tcb *l = tc->tc_listener;
        struct T_conn_ind ci;
        mblk_t *mp;

        if (l->tc_up == NULL ||
            (mp = tcp_ind(sizeof(ci), 1, tc->tc_faddr, tc->tc_fport)) ==
            NULL) {
                tcp_drop(tc, ECONNABORTED, 1);
                return 0;
        }
        memset(&ci, 0, sizeof(ci));
        ci.PRIM_type = T_CONN_IND;
        ci.SRC_length = sizeof(struct sockaddr_in);
        ci.SRC_offset = sizeof(ci);
        ci.SEQ_number = tc->tc_seqno;
        memcpy(mp->b_rptr, &ci, sizeof(ci));
        tc->tc_indicated = 1;
        (void)putnext(l->tc_up->tu_rq, mp);
        return 1;
}

/* An active open is established: T_CONN_CON, ahead of any data. */
static void
tcp_connected(struct tcb *tc)
{
        struct T_conn_con cc;
        mblk_t *mp = tcp_ind(sizeof(cc), 1, tc->tc_faddr, tc->tc_fport);

        if (mp == NULL)
                return;
        memset(&cc, 0, sizeof(cc));
        cc.PRIM_type = T_CONN_CON;
        cc.RES_length = sizeof(struct sockaddr_in);
        cc.RES_offset = sizeof(cc);
        memcpy(mp->b_rptr, &cc, sizeof(cc));
        tcp_rcvappend(tc, mp);
}

/* --- Arriving segments. --- */

/* The round trip just measured (RFC 6298), in ticks. */
static void
tcp_rttupdate(struct tcb *tc)
{
        int m = tc->tc_rtt - 1;

        if (!tc->tc_rttvalid) {
                tc->tc_srtt = m << 3;
                tc->tc_rttvar = m << 1;
                tc->tc_rttvalid = 1;
        } else {
                int delta = m - (tc->tc_srtt >> 3);

                tc->tc_srtt += delta;
                if (delta < 0)
                        delta = -delta;
                tc->tc_rttvar += delta - (tc->tc_rttvar >> 2);
        }
        tc->tc_rto = (tc->tc_srtt >> 3) + tc->tc_rttvar;
        if (tc->tc_rto < TCP_RTOMIN)
                tc->tc_rto = TCP_RTOMIN;
        if (tc->tc_rto > TCP_RTOMAX)
                tc->tc_rto = TCP_RTOMAX;
        tc->tc_rtt = 0;
}

/* Take the first n bytes off tc's send queue. */
static void
tcp_sndtrim(struct tcb *tc, size_t n)
{
        tc->tc_sndcc -= n;
        tc->tc_sndbase += (uint32_t)n;
        while (n > 0 && tc->tc_sndq != NULL) {
                mblk_t *bp = tc->tc_sndq;
                size_t have = MBLKL(bp);

                if (have > n) {
                        bp->b_rptr += n;
                        break;
                }
                n -= have;
                tc->tc_sndq = bp->b_cont;
                freeb(bp);
        }
        while (tc->tc_sndq != NULL && MBLKL(tc->tc_sndq) == 0) {
                mblk_t *bp = tc->tc_sndq;

                tc->tc_sndq = bp->b_cont;
                freeb(bp);
        }
}

/* An acceptable ACK of new data (or of our SYN or FIN). */
static void
tcp_newack(struct tcb *tc, uint32_t ack)
{
        if (tc->tc_rtt != 0 && SEQ_GT(ack, tc->tc_rtseq))
                tcp_rttupdate(tc);
        if (SEQ_GT(ack, tc->tc_sndbase)) {
                size_t n = ack - tc->tc_sndbase;

                if (n > tc->tc_sndcc) {
                        n = tc->tc_sndcc;
                        if (tc->tc_fin)
                                tc->tc_finacked = 1;
                }
                tcp_sndtrim(tc, n);
        }
        tc->tc_snd_una = ack;
        if (SEQ_LT(tc->tc_snd_nxt, ack))
                tc->tc_snd_nxt = ack;
        if (tc->tc_cwnd < tc->tc_ssthresh)
                tc->tc_cwnd += tc->tc_mss;
        else
                tc->tc_cwnd += tc->tc_mss * tc->tc_mss / tc->tc_cwnd + 1;
        if (tc->tc_cwnd > 4 * TCP_SNDBUF)
                tc->tc_cwnd = 4 * TCP_SNDBUF;
        tc->tc_rxtshift = 0;
        tc->tc_rexmt = tc->tc_snd_una == tc->tc_snd_max ? 0 : tc->tc_rto;
        /* Room for what the stream waits to send. */
        if (tc->tc_up != NULL && WR(tc->tc_up->tu_rq)->q_first != NULL)
                qenable(WR(tc->tc_up->tu_rq));
}

/* A SYN for a listener: a child, in SYN_RCVD, answering it. */
static void
tcp_listen_input(struct tcb *l, const struct tcp_seg *sg)
{
        struct tcb *tc;

        if ((sg->ts_flags & TH_RST) != 0)
                return;
        if ((sg->ts_flags & TH_ACK) != 0) {
                tcp_reset_reply(sg);
                return;
        }
        if ((sg->ts_flags & TH_SYN) == 0 || sg->ts_dst == INADDR_BROADCAST ||
            l->tc_nchildren >= l->tc_backlog || (tc = tcp_newtcb()) == NULL)
                return;                 /* the peer will try again */
        tc->tc_opt = l->tc_opt;
        tc->tc_laddr = sg->ts_dst;
        tc->tc_lport = sg->ts_dport;
        tc->tc_faddr = sg->ts_src;
        tc->tc_fport = sg->ts_sport;
        if (sg->ts_mss != 0)
                tc->tc_mss = sg->ts_mss < TCP_MSS ? sg->ts_mss : TCP_MSS;
        tcp_begin(tc);
        tc->tc_state = TCPS_SYN_RCVD;
        tc->tc_irs = sg->ts_seq;
        tc->tc_rcv_nxt = tc->tc_rcv_adv = sg->ts_seq + 1;
        tc->tc_snd_wnd = sg->ts_win;
        tc->tc_snd_wl1 = sg->ts_seq;
        tc->tc_listener = l;
        tc->tc_seqno = tcp_nextseqno++;
        if (tcp_nextseqno <= 0)
                tcp_nextseqno = 1;
        tc->tc_sibling = l->tc_children;
        l->tc_children = tc;
        l->tc_nchildren++;
        tcp_output(tc);
}

static void
tcp_synsent_input(struct tcb *tc, const struct tcp_seg *sg)
{
        int ackok = (sg->ts_flags & TH_ACK) != 0 &&
            SEQ_GT(sg->ts_ack, tc->tc_iss) &&
            SEQ_LEQ(sg->ts_ack, tc->tc_snd_max);

        if ((sg->ts_flags & TH_ACK) != 0 && !ackok) {
                tcp_reset_reply(sg);
                return;
        }
        if ((sg->ts_flags & TH_RST) != 0) {
                if (ackok)
                        tcp_drop(tc, ECONNREFUSED, 0);
                return;
        }
        if ((sg->ts_flags & TH_SYN) == 0)
                return;
        tc->tc_irs = sg->ts_seq;
        tc->tc_rcv_nxt = tc->tc_rcv_adv = sg->ts_seq + 1;
        if (sg->ts_mss != 0)
                tc->tc_mss = sg->ts_mss < TCP_MSS ? sg->ts_mss : TCP_MSS;
        tc->tc_cwnd = 4 * tc->tc_mss;
        tc->tc_laddr = sg->ts_dst;      /* the source IP chose for us */
        tc->tc_snd_wnd = sg->ts_win;
        tc->tc_snd_wl1 = sg->ts_seq;
        tc->tc_snd_wl2 = sg->ts_ack;
        tc->tc_flags |= TF_ACKNOW;
        if (!ackok) {
                /* Each side's SYN crossed the other's. */
                tc->tc_state = TCPS_SYN_RCVD;
                tc->tc_snd_nxt = tc->tc_iss;
                tc->tc_rexmt = 0;
                tcp_output(tc);
                return;
        }
        tcp_newack(tc, sg->ts_ack);
        tc->tc_state = tc->tc_fin ? TCPS_FIN_WAIT_1 : TCPS_ESTABLISHED;
        tcp_connected(tc);
        tcp_push(tc);
        tcp_output(tc);
}

/* Whether the segment falls in the receive window (RFC 9293 3.10.7.4). */
static int
tcp_acceptable(const struct tcb *tc, const struct tcp_seg *sg)
{
        uint32_t wnd = tcp_window(tc);
        size_t seglen = sg->ts_len + ((sg->ts_flags & TH_SYN) != 0) +
            ((sg->ts_flags & TH_FIN) != 0);
        uint32_t seq = sg->ts_seq, nxt = tc->tc_rcv_nxt;

        if (seglen == 0)
                return wnd == 0 ? seq == nxt :
                    SEQ_GEQ(seq, nxt) && SEQ_LT(seq, nxt + wnd);
        if (wnd == 0)
                return 0;
        return (SEQ_GEQ(seq, nxt) && SEQ_LT(seq, nxt + wnd)) ||
            (SEQ_GEQ(seq + (uint32_t)seglen - 1, nxt) &&
            SEQ_LT(seq + (uint32_t)seglen - 1, nxt + wnd));
}

/* The connection has ended (an attached tcb closes; others go). */
static void
tcp_closed(struct tcb *tc)
{
        if (tc->tc_up == NULL && tc->tc_listener == NULL) {
                tcp_free(tc);
                return;
        }
        tc->tc_rexmt = tc->tc_timer2 = 0;
        tc->tc_state = TCPS_CLOSED;
}

static void
tcp_timewait(struct tcb *tc)
{
        tc->tc_state = TCPS_TIME_WAIT;
        tc->tc_rexmt = 0;
        tc->tc_timer2 = TCP_2MSL;
}

static void
tcp_ordrel_ind(struct tcb *tc)
{
        struct T_ordrel_ind oi = { T_ORDREL_IND };
        mblk_t *mp = tcp_ind(sizeof(oi), 0, 0, 0);

        if (mp != NULL) {
                memcpy(mp->b_rptr, &oi, sizeof(oi));
                tcp_rcvappend(tc, mp);
        }
}

/* A segment for a synchronized connection, or one in SYN_RCVD. */
static void
tcp_conn_input(struct tcb *tc, struct tcp_seg *sg, mblk_t *data)
{
        int fin = (sg->ts_flags & TH_FIN) != 0;

        if (!tcp_acceptable(tc, sg)) {
                if ((sg->ts_flags & TH_RST) == 0) {
                        tc->tc_flags |= TF_ACKNOW;
                        tcp_output(tc);
                }
                goto drop;
        }
        if ((sg->ts_flags & TH_RST) != 0) {
                if (tc->tc_state == TCPS_SYN_RCVD && tc->tc_listener == NULL)
                        tcp_drop(tc, ECONNREFUSED, 0);
                else if (tc->tc_state == TCPS_CLOSING ||
                    tc->tc_state == TCPS_LAST_ACK ||
                    tc->tc_state == TCPS_TIME_WAIT)
                        tcp_closed(tc);
                else
                        tcp_drop(tc, ECONNRESET, 0);
                goto drop;
        }
        if ((sg->ts_flags & TH_SYN) != 0) {
                /* A SYN in the window: answer with an ACK (RFC 5961). */
                tc->tc_flags |= TF_ACKNOW;
                tcp_output(tc);
                goto drop;
        }
        if ((sg->ts_flags & TH_ACK) == 0)
                goto drop;

        if (tc->tc_state == TCPS_SYN_RCVD) {
                if (SEQ_LEQ(sg->ts_ack, tc->tc_snd_una) ||
                    SEQ_GT(sg->ts_ack, tc->tc_snd_max)) {
                        tcp_reset_reply(sg);
                        goto drop;
                }
                tc->tc_state = tc->tc_fin ? TCPS_FIN_WAIT_1 :
                    TCPS_ESTABLISHED;
                tc->tc_snd_wnd = sg->ts_win;
                tc->tc_snd_wl1 = sg->ts_seq;
                tc->tc_snd_wl2 = sg->ts_ack;
                if (tc->tc_listener != NULL) {
                        if (!tcp_indicate(tc))
                                goto drop;      /* and tc with it */
                } else {
                        tcp_connected(tc);
                }
        }
        if (SEQ_GT(sg->ts_ack, tc->tc_snd_max)) {
                tc->tc_flags |= TF_ACKNOW;
                tcp_output(tc);
                goto drop;
        }
        if (SEQ_GT(sg->ts_ack, tc->tc_snd_una))
                tcp_newack(tc, sg->ts_ack);
        if (SEQ_LT(tc->tc_snd_wl1, sg->ts_seq) ||
            (tc->tc_snd_wl1 == sg->ts_seq &&
            SEQ_LEQ(tc->tc_snd_wl2, sg->ts_ack))) {
                tc->tc_snd_wnd = sg->ts_win;
                tc->tc_snd_wl1 = sg->ts_seq;
                tc->tc_snd_wl2 = sg->ts_ack;
        }
        if (tc->tc_snd_wnd == 0)
                tc->tc_rxtshift = 0;    /* a closed window: still alive */
        if (tc->tc_finacked) {
                switch (tc->tc_state) {
                case TCPS_FIN_WAIT_1:
                        tc->tc_state = TCPS_FIN_WAIT_2;
                        if (tc->tc_up == NULL)
                                tc->tc_timer2 = TCP_FW2ORPHAN;
                        break;
                case TCPS_CLOSING:
                        tcp_timewait(tc);
                        break;
                case TCPS_LAST_ACK:
                        tcp_closed(tc);
                        goto drop;
                case TCPS_TIME_WAIT:
                        tc->tc_timer2 = TCP_2MSL;
                        break;
                }
        }

        /* The data and the FIN, in order only. */
        if (SEQ_LT(sg->ts_seq, tc->tc_rcv_nxt)) {
                uint32_t d = tc->tc_rcv_nxt - sg->ts_seq;

                if (d > sg->ts_len) {
                        fin = 0;        /* even the FIN was had before */
                        d = (uint32_t)sg->ts_len;
                }
                if (d > 0 && data != NULL)
                        (void)adjmsg(data, (ssize_t)d);
                sg->ts_len -= d;
                sg->ts_seq += d;
        }
        if (sg->ts_len > tcp_window(tc)) {
                size_t over = sg->ts_len - tcp_window(tc);

                if (data != NULL)
                        (void)adjmsg(data, -(ssize_t)over);
                sg->ts_len -= over;
                fin = 0;
        }
        if (sg->ts_seq != tc->tc_rcv_nxt) {
                /* Out of order: dropped, and our ACK says what we want. */
                if (sg->ts_len > 0 || fin)
                        tc->tc_flags |= TF_ACKNOW;
                sg->ts_len = 0;
                fin = 0;
        }
        if (sg->ts_len > 0) {
                switch (tc->tc_state) {
                case TCPS_ESTABLISHED:
                case TCPS_FIN_WAIT_1:
                case TCPS_FIN_WAIT_2:
                        tc->tc_rcv_nxt += (uint32_t)sg->ts_len;
                        tc->tc_flags |= TF_ACKNOW;
                        /* An orphan has no reader: the data is let go. */
                        if (data != NULL &&
                            (tc->tc_up != NULL || tc->tc_listener != NULL)) {
                                tcp_rcvappend(tc, data);
                                data = NULL;
                        }
                        break;
                default:
                        break;
                }
        }
        if (fin && tc->tc_state != TCPS_SYN_RCVD) {
                tc->tc_rcv_nxt++;
                tc->tc_flags |= TF_ACKNOW;
                switch (tc->tc_state) {
                case TCPS_ESTABLISHED:
                        tc->tc_state = TCPS_CLOSE_WAIT;
                        tcp_ordrel_ind(tc);
                        break;
                case TCPS_FIN_WAIT_1:
                        tcp_ordrel_ind(tc);
                        if (tc->tc_finacked)
                                tcp_timewait(tc);
                        else
                                tc->tc_state = TCPS_CLOSING;
                        break;
                case TCPS_FIN_WAIT_2:
                        tcp_ordrel_ind(tc);
                        tcp_timewait(tc);
                        break;
                case TCPS_TIME_WAIT:
                        tc->tc_timer2 = TCP_2MSL;
                        break;
                default:
                        break;
                }
        }
        tcp_push(tc);
        tcp_output(tc);
drop:
        if (data != NULL)
                freemsg(data);
}

/* A segment from IP, its IP header first. Frees mp. */
static void
tcp_input(mblk_t *mp)
{
        struct tcp_seg sg;
        struct tcb *tc, *best = NULL;
        size_t hlen, len, off;
        struct ip *ip;
        uint8_t *th;
        uint32_t v;

        if (!pullupmsg(mp, IP_MINHLEN))
                goto drop;
        ip = (struct ip *)mp->b_rptr;
        hlen = IP_HLEN(ip);
        len = msgdsize(mp);
        if (hlen < IP_MINHLEN || len < hlen + TCP_HLEN ||
            !pullupmsg(mp, (ssize_t)(hlen + TCP_HLEN)))
                goto drop;
        th = mp->b_rptr + hlen;
        off = (size_t)(th[12] >> 4) * 4;
        if (off < TCP_HLEN || hlen + off > len ||
            !pullupmsg(mp, (ssize_t)(hlen + off)))
                goto drop;
        ip = (struct ip *)mp->b_rptr;
        th = mp->b_rptr + hlen;
        memset(&sg, 0, sizeof(sg));
        sg.ts_src = ip->ip_src;
        sg.ts_dst = ip->ip_dst;
        if (in_fold(in_sum(mp, hlen, len - hlen, in_pseudo(sg.ts_src,
            sg.ts_dst, IPPROTO_TCP, len - hlen))) != 0)
                goto drop;
        if (sg.ts_dst == INADDR_BROADCAST || sg.ts_src == INADDR_ANY ||
            sg.ts_src == INADDR_BROADCAST)
                goto drop;
        memcpy(&sg.ts_sport, th, 2);
        memcpy(&sg.ts_dport, th + 2, 2);
        memcpy(&v, th + 4, 4);
        sg.ts_seq = ntohl(v);
        memcpy(&v, th + 8, 4);
        sg.ts_ack = ntohl(v);
        sg.ts_flags = th[13];
        sg.ts_win = (uint32_t)th[14] << 8 | th[15];
        sg.ts_len = len - hlen - off;
        /* The options: only MSS, and only on a SYN. */
        if ((sg.ts_flags & TH_SYN) != 0) {
                for (size_t i = TCP_HLEN; i < off;) {
                        uint8_t kind = th[i];

                        if (kind == 0)
                                break;
                        if (kind == 1) {
                                i++;
                                continue;
                        }
                        if (i + 1 >= off || th[i + 1] < 2 ||
                            i + th[i + 1] > off)
                                break;
                        if (kind == 2 && th[i + 1] == 4)
                                sg.ts_mss = (uint32_t)th[i + 2] << 8 |
                                    th[i + 3];
                        i += th[i + 1];
                }
                if (sg.ts_mss != 0 && sg.ts_mss < 64)
                        sg.ts_mss = 64;
        }
        mp->b_rptr += hlen + off;

        /* The connection; else a listener. */
        for (tc = tcp_tcbs; tc != NULL; tc = tc->tc_next) {
                if (tc->tc_lport != sg.ts_dport)
                        continue;
                if (tc->tc_state > TCPS_LISTEN &&
                    tc->tc_fport == sg.ts_sport &&
                    tc->tc_faddr == sg.ts_src &&
                    (tc->tc_laddr == sg.ts_dst || tc->tc_laddr == INADDR_ANY))
                        break;
                if (tc->tc_state == TCPS_LISTEN &&
                    (tc->tc_laddr == sg.ts_dst ||
                    (tc->tc_laddr == INADDR_ANY && best == NULL)))
                        best = tc;
        }
        if (tc == NULL)
                tc = best;
        if (tc == NULL) {
                tcp_reset_reply(&sg);
                goto drop;
        }
        tc->tc_idle = tc->tc_kprobes = 0;      /* the peer is there */
        switch (tc->tc_state) {
        case TCPS_LISTEN:
                tcp_listen_input(tc, &sg);
                break;
        case TCPS_SYN_SENT:
                tcp_synsent_input(tc, &sg);
                break;
        default:
                if (sg.ts_len == 0) {
                        freemsg(mp);
                        mp = NULL;
                }
                tcp_conn_input(tc, &sg, mp);
                return;
        }
drop:
        if (mp != NULL)
                freemsg(mp);
}

/* --- The timer. --- */

static void
tcp_timeout(struct tcb *tc)
{
        int limit = TCPS_SYNCED(tc->tc_state) ? TCP_MAXRXT : TCP_MAXSYNRXT;
        uint32_t flight = tc->tc_snd_max - tc->tc_snd_una;

        if (++tc->tc_rxtshift > limit) {
                tcp_drop(tc, ETIMEDOUT, 1);
                return;
        }
        tc->tc_rto *= 2;
        if (tc->tc_rto > TCP_RTOMAX)
                tc->tc_rto = TCP_RTOMAX;
        tc->tc_ssthresh = flight / 2 > 2 * tc->tc_mss ? flight / 2 :
            2 * tc->tc_mss;
        tc->tc_cwnd = tc->tc_mss;
        tc->tc_snd_nxt = tc->tc_snd_una;
        tc->tc_rtt = 0;                 /* no timing a retransmission */
        if (tc->tc_snd_wnd == 0 && TCPS_SYNCED(tc->tc_state))
                tc->tc_flags |= TF_FORCE;
        tcp_output(tc);
}

/*
 * SO_KEEPALIVE: once a connection with nothing in flight has been idle
 * for to_keepidle seconds, a probe every to_keepintvl seconds, a
 * segment just before the window that the peer must answer with an ACK;
 * to_keepcnt of them unanswered, and it is dropped.
 */
static void
tcp_keepalive(struct tcb *tc)
{
        const struct tcp_opt *o = &tc->tc_opt;

        if (!o->to_keepalive || (tc->tc_state != TCPS_ESTABLISHED &&
            tc->tc_state != TCPS_CLOSE_WAIT) ||
            tc->tc_snd_una != tc->tc_snd_max)
                return;
        if (++tc->tc_idle < (o->to_keepidle + tc->tc_kprobes *
            o->to_keepintvl) * 10)
                return;
        if (tc->tc_kprobes >= o->to_keepcnt) {
                tcp_drop(tc, ETIMEDOUT, 1);
                return;
        }
        tc->tc_kprobes++;
        tcp_xmit(tc->tc_laddr, tc->tc_faddr, tc->tc_lport, tc->tc_fport,
            tc->tc_snd_una - 1, tc->tc_rcv_nxt, TH_ACK, tcp_window(tc),
            NULL, 0, NULL);
}

static void
tcp_tick(void *arg __unused)
{
        struct tcb *tc, *next;

        for (tc = tcp_tcbs; tc != NULL; tc = next) {
                next = tc->tc_next;
                if (tc->tc_rtt != 0)
                        tc->tc_rtt++;
                if (tc->tc_rexmt != 0 && --tc->tc_rexmt == 0) {
                        tcp_timeout(tc);
                } else if (tc->tc_timer2 != 0 && --tc->tc_timer2 == 0) {
                        /* TIME_WAIT is over, or an orphan's patience. */
                        tcp_closed(tc);
                } else {
                        tcp_keepalive(tc);
                }
        }
        tcp_timer = tcp_lower != NULL ?
            qtimeout(RD(tcp_lower), tcp_tick, NULL, TCP_TICK) : 0;
}

/* --- Requests from the stream. --- */

static int
tcp_tpistate(const struct tcp_up *tu)
{
        const struct tcb *tc = tu->tu_tcb;

        if (tc == NULL)
                return TS_UNBND;
        switch (tc->tc_state) {
        case TCPS_SYN_SENT:
                return TS_WCON_CREQ;
        case TCPS_SYN_RCVD:
        case TCPS_ESTABLISHED:
                return TS_DATA_XFER;
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
                return TS_WIND_ORDREL;
        case TCPS_CLOSE_WAIT:
                return TS_WREQ_ORDREL;
        default:
                return TS_IDLE;
        }
}

static void
tcp_infoack(const struct tcp_up *tu, struct T_info_ack *ia)
{
        memset(ia, 0, sizeof(*ia));
        ia->PRIM_type = T_INFO_ACK;
        ia->TSDU_size = 0;              /* a byte stream */
        ia->ETSDU_size = T_INVALID;
        ia->CDATA_size = T_INVALID;
        ia->DDATA_size = T_INVALID;
        ia->ADDR_size = sizeof(struct sockaddr_in);
        ia->OPT_size = TCP_REPLYSZ;
        ia->TIDU_size = TCP_MSS;
        ia->SERV_type = T_COTS_ORD;
        ia->CURRENT_state = tcp_tpistate(tu);
        ia->PROVIDER_flag = 0;
}

static void
tcp_bind(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr;
        struct T_bind_req br;
        struct T_bind_ack ba;
        struct sockaddr_in sin;
        struct tcb *tc;
        int error;

        if (MBLKL(mp) < sizeof(br)) {
                tcp_errack(q, mp, T_BIND_REQ, TSYSERR, EINVAL);
                return;
        }
        memcpy(&br, mp->b_rptr, sizeof(br));
        if (tu->tu_tcb != NULL) {
                tcp_errack(q, mp, T_BIND_REQ, TOUTSTATE, 0);
                return;
        }
        if (br.ADDR_length == 0)
                tcp_sin(&sin, INADDR_ANY, 0);
        else if ((error = tcp_addr(mp, br.ADDR_length, br.ADDR_offset,
            &sin)) != 0) {
                tcp_errack(q, mp, T_BIND_REQ, error, 0);
                return;
        }
        if (sin.sin_port == 0) {
                int span = TCP_PORT_LAST - TCP_PORT_FIRST + 1;

                for (int i = 0; i < span && sin.sin_port == 0; i++) {
                        in_port_t p = htons(tcp_nextport);

                        tcp_nextport = tcp_nextport == TCP_PORT_LAST ?
                            TCP_PORT_FIRST : tcp_nextport + 1;
                        if (tcp_portfree(p))
                                sin.sin_port = p;
                }
                if (sin.sin_port == 0) {
                        tcp_errack(q, mp, T_BIND_REQ, TNOADDR, 0);
                        return;
                }
        } else if (ntohs(sin.sin_port) < 1024 && !tu->tu_priv) {
                tcp_errack(q, mp, T_BIND_REQ, TACCES, 0);
                return;
        } else if (tcp_inuse(sin.sin_addr.s_addr, sin.sin_port,
            tu->tu_opt.to_reuseaddr)) {
                tcp_errack(q, mp, T_BIND_REQ, TADDRBUSY, 0);
                return;
        }
        if ((tc = tcp_newtcb()) == NULL) {
                tcp_errack(q, mp, T_BIND_REQ, TSYSERR, ENOMEM);
                return;
        }
        tc->tc_laddr = sin.sin_addr.s_addr;
        tc->tc_lport = sin.sin_port;
        tc->tc_up = tu;
        tc->tc_opt = tu->tu_opt;
        tu->tu_tcb = tc;
        if (br.CONIND_number > 0) {
                tc->tc_state = TCPS_LISTEN;
                tc->tc_backlog = br.CONIND_number > TCP_BACKLOG ?
                    TCP_BACKLOG : (int)br.CONIND_number;
        }
        memset(&ba, 0, sizeof(ba));
        ba.PRIM_type = T_BIND_ACK;
        ba.ADDR_length = sizeof(sin);
        ba.ADDR_offset = sizeof(ba);
        ba.CONIND_number = (t_uscalar_t)tc->tc_backlog;
        if ((mp = tcp_reply(mp, sizeof(ba) + sizeof(sin), M_PCPROTO)) == NULL)
                return;
        tcp_sin(&sin, tc->tc_laddr, tc->tc_lport);
        memcpy(mp->b_rptr, &ba, sizeof(ba));
        memcpy(mp->b_rptr + sizeof(ba), &sin, sizeof(sin));
        qreply(q, mp);
}

/* A listener goes: its children with it. */
static void
tcp_unlisten(struct tcb *l)
{
        while (l->tc_children != NULL)
                tcp_drop(l->tc_children, ECONNABORTED, 1);
}

static void
tcp_connect(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr;
        struct tcb *tc = tu->tu_tcb;
        struct T_conn_req cr;
        struct sockaddr_in sin;

        if (MBLKL(mp) < sizeof(cr)) {
                tcp_errack(q, mp, T_CONN_REQ, TSYSERR, EINVAL);
                return;
        }
        memcpy(&cr, mp->b_rptr, sizeof(cr));
        if (tc == NULL || tc->tc_state != TCPS_CLOSED) {
                tcp_errack(q, mp, T_CONN_REQ, TOUTSTATE, 0);
                return;
        }
        if (tcp_addr(mp, cr.DEST_length, cr.DEST_offset, &sin) != 0 ||
            sin.sin_port == 0 || sin.sin_addr.s_addr == INADDR_ANY ||
            sin.sin_addr.s_addr == INADDR_BROADCAST) {
                tcp_errack(q, mp, T_CONN_REQ, TBADADDR, 0);
                return;
        }
        for (struct tcb *o = tcp_tcbs; o != NULL; o = o->tc_next)
                if (o != tc && o->tc_state > TCPS_LISTEN &&
                    o->tc_lport == tc->tc_lport &&
                    o->tc_fport == sin.sin_port &&
                    o->tc_faddr == sin.sin_addr.s_addr &&
                    (o->tc_laddr == tc->tc_laddr ||
                    o->tc_laddr == INADDR_ANY || tc->tc_laddr == INADDR_ANY)) {
                        tcp_errack(q, mp, T_CONN_REQ, TADDRBUSY, 0);
                        return;
                }
        tcp_clear(tc);
        tc->tc_faddr = sin.sin_addr.s_addr;
        tc->tc_fport = sin.sin_port;
        tc->tc_mss = TCP_DEFMSS;
        tcp_begin(tc);
        tc->tc_state = TCPS_SYN_SENT;
        tcp_okack(q, mp, T_CONN_REQ);
        tcp_output(tc);
}

/* T_CONN_RES: a listener's child to the acceptor stream. */
static void
tcp_accept(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr, *acc;
        struct tcb *l = tu->tu_tcb, *tc;
        struct T_conn_res cr;

        if (MBLKL(mp) < sizeof(cr)) {
                tcp_errack(q, mp, T_CONN_RES, TSYSERR, EINVAL);
                return;
        }
        memcpy(&cr, mp->b_rptr, sizeof(cr));
        if (l == NULL || l->tc_state != TCPS_LISTEN) {
                tcp_errack(q, mp, T_CONN_RES, TOUTSTATE, 0);
                return;
        }
        for (tc = l->tc_children; tc != NULL; tc = tc->tc_sibling)
                if (tc->tc_indicated && tc->tc_seqno == cr.SEQ_number)
                        break;
        if (tc == NULL) {
                tcp_errack(q, mp, T_CONN_RES, TBADSEQ, 0);
                return;
        }
        acc = tcp_up_byid(cr.ACCEPTOR_id);
        if (acc == NULL || acc == tu || acc->tu_tcb != NULL) {
                tcp_errack(q, mp, T_CONN_RES, TBADF, 0);
                return;
        }
        tcp_unchild(tc);
        tc->tc_up = acc;
        acc->tu_tcb = tc;
        acc->tu_opt = tc->tc_opt;       /* the listener's */
        tcp_okack(q, mp, T_CONN_RES);
        qenable(acc->tu_rq);            /* what arrived before, up */
}

static void
tcp_discon(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr;
        struct tcb *tc = tu->tu_tcb;
        struct T_discon_req dr;

        if (MBLKL(mp) < sizeof(dr)) {
                tcp_errack(q, mp, T_DISCON_REQ, TSYSERR, EINVAL);
                return;
        }
        memcpy(&dr, mp->b_rptr, sizeof(dr));
        if (tc != NULL && tc->tc_state == TCPS_LISTEN) {
                struct tcb *c;

                for (c = tc->tc_children; c != NULL; c = c->tc_sibling)
                        if (c->tc_indicated && c->tc_seqno == dr.SEQ_number)
                                break;
                if (c == NULL) {
                        tcp_errack(q, mp, T_DISCON_REQ, TBADSEQ, 0);
                        return;
                }
                c->tc_indicated = 0;    /* no T_DISCON_IND for it */
                tcp_drop(c, ECONNABORTED, 1);
                tcp_okack(q, mp, T_DISCON_REQ);
                return;
        }
        if (tc == NULL || tc->tc_state == TCPS_CLOSED) {
                tcp_errack(q, mp, T_DISCON_REQ, TOUTSTATE, 0);
                return;
        }
        /* An abort: a reset to the peer, and nothing kept. */
        if (tc->tc_state != TCPS_SYN_SENT && tc->tc_state != TCPS_TIME_WAIT)
                tcp_xmit(tc->tc_laddr, tc->tc_faddr, tc->tc_lport,
                    tc->tc_fport, tc->tc_snd_nxt, tc->tc_rcv_nxt,
                    TH_RST | TH_ACK, 0, NULL, 0, NULL);
        tcp_clear(tc);
        tc->tc_state = TCPS_CLOSED;
        flushq(WR(q), FLUSHDATA);
        tcp_okack(q, mp, T_DISCON_REQ);
}

static void
tcp_addrack(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr;
        struct tcb *tc = tu->tu_tcb;
        struct T_addr_ack aa;
        struct sockaddr_in loc, rem;
        int conn = tc != NULL && tc->tc_state > TCPS_LISTEN;

        memset(&aa, 0, sizeof(aa));
        aa.PRIM_type = T_ADDR_ACK;
        if (tc != NULL) {
                aa.LOCADDR_length = sizeof(loc);
                aa.LOCADDR_offset = sizeof(aa);
                tcp_sin(&loc, tc->tc_laddr, tc->tc_lport);
        }
        if (conn) {
                aa.REMADDR_length = sizeof(rem);
                aa.REMADDR_offset = sizeof(aa) + sizeof(loc);
                tcp_sin(&rem, tc->tc_faddr, tc->tc_fport);
        }
        if ((mp = tcp_reply(mp, sizeof(aa) + 2 * sizeof(loc), M_PCPROTO)) ==
            NULL)
                return;
        mp->b_wptr = mp->b_rptr + sizeof(aa) + (tc != NULL ? sizeof(loc) :
            0) + (conn ? sizeof(rem) : 0);
        memcpy(mp->b_rptr, &aa, sizeof(aa));
        if (tc != NULL)
                memcpy(mp->b_rptr + sizeof(aa), &loc, sizeof(loc));
        if (conn)
                memcpy(mp->b_rptr + sizeof(aa) + sizeof(loc), &rem,
                    sizeof(rem));
        qreply(q, mp);
}

/* One option of a stream's (tpi_optfn). */
static int
tcp_optfn(void *arg, int mgmt, int level, int name, const void *val,
    size_t len, void *out, size_t *outlen)
{
        struct tcp_up *tu = arg;
        struct tcp_opt *o = &tu->tu_opt;
        int set = mgmt == T_NEGOTIATE, v = 0;

        if (level == SOL_SOCKET && name == SO_LINGER) {
                struct linger l;

                if (set) {
                        if (len < sizeof(l))
                                return T_FAILURE;
                        memcpy(&l, val, sizeof(l));
                        if (l.l_linger < 0)
                                return T_FAILURE;
                        o->to_linger = l.l_onoff != 0;
                        o->to_lingertime = l.l_linger;
                }
                l.l_onoff = o->to_linger;
                l.l_linger = o->to_lingertime;
                memcpy(out, &l, sizeof(l));
                *outlen = sizeof(l);
                goto done;
        }
        if (set && tpi_optint(val, len, &v) != 0)
                return T_FAILURE;
        if (level == SOL_SOCKET) {
                switch (name) {
                case SO_REUSEADDR:
                        if (set)
                                o->to_reuseaddr = v != 0;
                        v = o->to_reuseaddr;
                        break;
                case SO_KEEPALIVE:
                        if (set)
                                o->to_keepalive = v != 0;
                        v = o->to_keepalive;
                        break;
                case SO_SNDBUF:
                        if (set)
                                o->to_sndbuf = v < TCP_MINBUF ? TCP_MINBUF :
                                    v > TCP_MAXSNDBUF ? TCP_MAXSNDBUF :
                                    (uint32_t)v;
                        v = (int)o->to_sndbuf;
                        break;
                case SO_RCVBUF:
                        if (set)
                                o->to_rcvbuf = v < TCP_MINBUF ? TCP_MINBUF :
                                    v > TCP_RCVBUF ? TCP_RCVBUF : (uint32_t)v;
                        v = (int)o->to_rcvbuf;
                        break;
                default:
                        return T_NOTSUPPORT;
                }
        } else if (level == IPPROTO_TCP) {
                switch (name) {
                case TCP_NODELAY:
                        if (set)
                                o->to_nodelay = v != 0;
                        v = o->to_nodelay;
                        break;
                case TCP_MAXSEG:
                        /* The peer's, once it has said; read only. */
                        if (set)
                                return T_READONLY;
                        v = tu->tu_tcb != NULL &&
                            TCPS_SYNCED(tu->tu_tcb->tc_state) ?
                            (int)tu->tu_tcb->tc_mss : TCP_DEFMSS;
                        break;
                case TCP_KEEPIDLE:
                case TCP_KEEPINTVL:
                case TCP_KEEPCNT: {
                        int *p = name == TCP_KEEPIDLE ? &o->to_keepidle :
                            name == TCP_KEEPINTVL ? &o->to_keepintvl :
                            &o->to_keepcnt;

                        if (set) {
                                if (v < 1 || v > (name == TCP_KEEPCNT ? 127 :
                                    TCP_KEEPMAX))
                                        return T_FAILURE;
                                *p = v;
                        }
                        v = *p;
                        break;
                }
                default:
                        return T_NOTSUPPORT;
                }
        } else {
                return T_NOTSUPPORT;
        }
        tpi_optout(v, out, outlen);
done:
        if (set && tu->tu_tcb != NULL) {
                tu->tu_tcb->tc_opt = *o;
                if (WR(tu->tu_rq)->q_first != NULL)
                        qenable(WR(tu->tu_rq)); /* a bigger SO_SNDBUF */
        }
        return T_SUCCESS;
}

static void
tcp_tpi(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr;
        t_scalar_t prim;

        memcpy(&prim, mp->b_rptr, sizeof(prim));
        switch (prim) {
        case T_INFO_REQ: {
                struct T_info_ack ia;

                tcp_infoack(tu, &ia);
                if ((mp = tcp_reply(mp, sizeof(ia), M_PCPROTO)) != NULL) {
                        memcpy(mp->b_rptr, &ia, sizeof(ia));
                        qreply(q, mp);
                }
                return;
        }
        case T_CAPABILITY_REQ: {
                struct T_capability_req cq;
                struct T_capability_ack ca;

                memset(&cq, 0, sizeof(cq));
                memcpy(&cq, mp->b_rptr, MBLKL(mp) < sizeof(cq) ?
                    MBLKL(mp) : sizeof(cq));
                memset(&ca, 0, sizeof(ca));
                ca.PRIM_type = T_CAPABILITY_ACK;
                ca.CAP_bits1 = cq.CAP_bits1 & (TC1_INFO | TC1_ACCEPTOR_ID);
                if ((ca.CAP_bits1 & TC1_INFO) != 0)
                        tcp_infoack(tu, &ca.INFO_ack);
                if ((ca.CAP_bits1 & TC1_ACCEPTOR_ID) != 0)
                        ca.ACCEPTOR_id = tu->tu_id;
                if ((mp = tcp_reply(mp, sizeof(ca), M_PCPROTO)) != NULL) {
                        memcpy(mp->b_rptr, &ca, sizeof(ca));
                        qreply(q, mp);
                }
                return;
        }
        case T_ADDR_REQ:
                tcp_addrack(q, mp);
                return;
        case T_BIND_REQ:
                tcp_bind(q, mp);
                return;
        case T_UNBIND_REQ: {
                struct tcb *tc = tu->tu_tcb;

                if (tc == NULL || (tc->tc_state != TCPS_CLOSED &&
                    tc->tc_state != TCPS_LISTEN)) {
                        tcp_errack(q, mp, prim, TOUTSTATE, 0);
                        return;
                }
                if (tc->tc_state == TCPS_LISTEN)
                        tcp_unlisten(tc);
                tcp_free(tc);
                tcp_okack(q, mp, prim);
                return;
        }
        case T_CONN_REQ:
                tcp_connect(q, mp);
                return;
        case T_CONN_RES:
                tcp_accept(q, mp);
                return;
        case T_DISCON_REQ:
                tcp_discon(q, mp);
                return;
        case T_OPTMGMT_REQ:
                tpi_optmgmt(q, mp, tcp_optfn, tu);
                return;
        default:
                tcp_errack(q, mp, prim, TNOTSUPPORT, 0);
                return;
        }
}

/* Data for the connection (freed if it cannot take any). */
static void
tcp_senddata(struct tcb *tc, mblk_t *mp)
{
        size_t n = msgdsize(mp);

        if (tc == NULL || tc->tc_fin || n == 0 ||
            (tc->tc_state != TCPS_SYN_SENT && tc->tc_state != TCPS_SYN_RCVD &&
            tc->tc_state != TCPS_ESTABLISHED &&
            tc->tc_state != TCPS_CLOSE_WAIT)) {
                freemsg(mp);
                return;
        }
        for (mblk_t *bp = mp; bp != NULL; bp = bp->b_cont)
                bp->b_datap->db_type = M_DATA;
        if (tc->tc_sndq == NULL)
                tc->tc_sndq = mp;
        else
                linkb(tc->tc_sndq, mp);
        tc->tc_sndcc += n;
        tcp_output(tc);
}

/* T_ORDREL_REQ: our FIN, after the data. */
static void
tcp_ordrel(struct tcb *tc)
{
        if (tc == NULL || tc->tc_fin)
                return;
        switch (tc->tc_state) {
        case TCPS_SYN_SENT:
        case TCPS_SYN_RCVD:
                tc->tc_fin = 1;         /* sent once established */
                return;
        case TCPS_ESTABLISHED:
                tc->tc_state = TCPS_FIN_WAIT_1;
                break;
        case TCPS_CLOSE_WAIT:
                tc->tc_state = TCPS_LAST_ACK;
                break;
        default:
                return;
        }
        tc->tc_fin = 1;
        tcp_output(tc);
}

static int
tcp_isdata(const mblk_t *mp)
{
        t_scalar_t prim;

        if (DB_TYPE(mp) == M_DATA)
                return 1;
        if (DB_TYPE(mp) != M_PROTO || MBLKL(mp) < sizeof(prim))
                return 0;
        memcpy(&prim, mp->b_rptr, sizeof(prim));
        return prim == T_DATA_REQ;
}

static int
tcp_isordrel(const mblk_t *mp)
{
        t_scalar_t prim;

        if (DB_TYPE(mp) != M_PROTO || MBLKL(mp) < sizeof(prim))
                return 0;
        memcpy(&prim, mp->b_rptr, sizeof(prim));
        return prim == T_ORDREL_REQ;
}

/* Data or T_ORDREL_REQ, in order, from the write queue or its put. */
static void
tcp_wdo(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr;

        if (tcp_isordrel(mp)) {
                freemsg(mp);
                tcp_ordrel(tu->tu_tcb);
                return;
        }
        if (DB_TYPE(mp) != M_DATA) {    /* T_DATA_REQ: its data */
                mblk_t *dp = mp->b_cont;

                mp->b_cont = NULL;
                freeb(mp);
                if ((mp = dp) == NULL)
                        return;
        }
        tcp_senddata(tu->tu_tcb, mp);
}

static int
tcp_room(const struct tcp_up *tu)
{
        return tu->tu_tcb == NULL ||
            tu->tu_tcb->tc_sndcc < tu->tu_tcb->tc_opt.to_sndbuf;
}

/* --- The streams. --- */

static int
tcp_open(queue_t *q, dev_t *devp __unused, int flag __unused, int sflag,
    struct cred *cr)
{
        struct tcp_up *tu;

        if (sflag != CLONEOPEN)
                return ENXIO;
        tu = kmem_zalloc(sizeof(*tu), KM_SLEEP);
        tu->tu_rq = q;
        tu->tu_priv = CRED_ISROOT(cr);
        tu->tu_opt = tcp_defopt;
        tu->tu_id = tcp_nextid++;
        if (tcp_nextid == 0)
                tcp_nextid = 1;
        tu->tu_next = tcp_ups;
        tcp_ups = tu;
        q->q_ptr = WR(q)->q_ptr = tu;
        return 0;
}

/*
 * Whether data the reader never took is waiting at the stream head
 * above rq: at the last close the head is still linked and its syncq
 * is claimed with ours, so its queue holds still while we look.
 */
static int
tcp_unread(queue_t *rq)
{
        for (mblk_t *mp = rq->q_next != NULL ? rq->q_next->q_first : NULL;
            mp != NULL; mp = mp->b_next)
                if (DB_TYPE(mp) == M_DATA)
                        return 1;
        return 0;
}

/*
 * The stream's last close. What it wrote goes on; a connection ends
 * as a close of a socket ends it, with a FIN, or a reset if data it
 * never read was waiting (or SO_LINGER says to linger 0 seconds), and
 * finishes as an orphan. A longer linger is not waited for.
 */
static int
tcp_close(queue_t *q, int flag __unused, struct cred *cr __unused)
{
        struct tcp_up *tu = q->q_ptr, **link;
        struct tcb *tc;
        mblk_t *mp;

        while ((mp = getq(WR(q))) != NULL)
                tcp_wdo(WR(q), mp);
        for (link = &tcp_ups; *link != NULL; link = &(*link)->tu_next)
                if (*link == tu) {
                        *link = tu->tu_next;
                        break;
                }
        if ((tc = tu->tu_tcb) != NULL) {
                tc->tc_up = NULL;
                tu->tu_tcb = NULL;
                /* SO_LINGER with no time to linger: an abort, a reset. */
                if (tc->tc_opt.to_linger && tc->tc_opt.to_lingertime == 0 &&
                    tc->tc_state > TCPS_LISTEN) {
                        tcp_drop(tc, ECONNRESET, 1);    /* and freed */
                        tc = NULL;
                }
        }
        if (tc != NULL) {
                switch (tc->tc_state) {
                case TCPS_LISTEN:
                        tcp_unlisten(tc);
                        tcp_free(tc);
                        break;
                case TCPS_CLOSED:
                case TCPS_SYN_SENT:
                        tcp_free(tc);
                        break;
                case TCPS_SYN_RCVD:
                case TCPS_ESTABLISHED:
                case TCPS_CLOSE_WAIT:
                        if (tc->tc_rcvcc > 0 || tcp_unread(q)) {
                                tcp_drop(tc, ECONNRESET, 1);
                                break;
                        }
                        tcp_flushq(&tc->tc_rcvq);
                        tc->tc_rcvtail = NULL;
                        tcp_ordrel(tc);
                        break;
                default:
                        tcp_flushq(&tc->tc_rcvq);
                        tc->tc_rcvtail = NULL;
                        tc->tc_rcvcc = 0;
                        if (tc->tc_state == TCPS_FIN_WAIT_2)
                                tc->tc_timer2 = TCP_FW2ORPHAN;
                        break;
                }
        }
        kmem_free(tu, sizeof(*tu));
        q->q_ptr = WR(q)->q_ptr = NULL;
        return 0;
}

/* The IP stream has gone: every connection with it. */
static void
tcp_netdown(void)
{
        struct tcb *tc, *next;

        if (tcp_timer != 0)
                quntimeout(RD(tcp_lower), tcp_timer);
        tcp_timer = 0;
        tcp_lower = NULL;
        /* Each drop frees no tcb but its own (a child, or an orphan). */
        for (tc = tcp_tcbs; tc != NULL; tc = next) {
                next = tc->tc_next;
                if (tc->tc_state > TCPS_LISTEN)
                        tcp_drop(tc, ENETDOWN, 0);
        }
}

static void
tcp_bindproto(void)
{
        struct iocblk *ioc;
        int32_t proto = IPPROTO_TCP;
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
        (void)putnext(tcp_lower, mp);
}

static void
tcp_ioctl(queue_t *q, mblk_t *mp)
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
                else if (tcp_lower != NULL)
                        error = EBUSY;          /* one IP stream */
                if (error != 0)
                        break;
                tcp_lower = lb->l_qbot;
                tcp_muxid = lb->l_index;
                tcp_bindproto();
                tcp_timer = qtimeout(RD(tcp_lower), tcp_tick, NULL, TCP_TICK);
                break;
        case I_UNLINK:
        case I_PUNLINK:
                if (lb == NULL)
                        error = EINVAL;
                else if (tcp_lower != NULL && lb->l_index == tcp_muxid)
                        tcp_netdown();
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
tcp_uwput(queue_t *q, mblk_t *mp)
{
        struct tcp_up *tu = q->q_ptr;

        switch (DB_TYPE(mp)) {
        case M_DATA:
        case M_PROTO:
        case M_PCPROTO:
                if (DB_TYPE(mp) != M_DATA && MBLKL(mp) < sizeof(t_scalar_t)) {
                        freemsg(mp);
                        return 0;
                }
                if (tcp_isdata(mp) || tcp_isordrel(mp)) {
                        /* In order, and as the send queue has room. */
                        if (q->q_first != NULL || !tcp_room(tu))
                                (void)putq(q, mp);
                        else
                                tcp_wdo(q, mp);
                        return 0;
                }
                tcp_tpi(q, mp);
                return 0;
        case M_IOCTL:
                tcp_ioctl(q, mp);
                return 0;
        case M_FLUSH:
                if ((*mp->b_rptr & FLUSHW) != 0)
                        flushq(q, FLUSHDATA);
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

static int
tcp_uwsrv(queue_t *q)
{
        struct tcp_up *tu = q->q_ptr;
        mblk_t *mp;

        while ((mp = getq(q)) != NULL) {
                if (tcp_isdata(mp) && !tcp_room(tu)) {
                        (void)putbq(q, mp);
                        break;
                }
                tcp_wdo(q, mp);
        }
        return 0;
}

/* The stream has room again: what waited goes up. */
static int
tcp_ursrv(queue_t *q)
{
        struct tcp_up *tu = q->q_ptr;

        if (tu != NULL && tu->tu_tcb != NULL)
                tcp_push(tu->tu_tcb);
        return 0;
}

static int
tcp_lrput(queue_t *q, mblk_t *mp)
{
        switch (DB_TYPE(mp)) {
        case M_DATA:
                if (WR(q) == tcp_lower)
                        tcp_input(mp);
                else
                        freemsg(mp);
                return 0;
        case M_IOCNAK:
                printf("tcp: IP refused protocol %d\n", IPPROTO_TCP);
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
                freemsg(mp);
                return 0;
        }
}

static int
tcp_lwput(queue_t *q, mblk_t *mp)
{
        (void)putnext(q, mp);
        return 0;
}
