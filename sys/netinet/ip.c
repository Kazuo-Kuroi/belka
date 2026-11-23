/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * IPv4 (RFC 791), with ICMP's echo and protocol-unreachable (RFC 792),
 * as a multiplexing STREAMS driver, /dev/ip (doc/design/decisions.md,
 * "ARP and IPv4"). Lower streams are DLPI streams, two per interface,
 * one for IPv4 and one for ARP (arp.c); upper streams bind a protocol
 * and send and receive whole datagrams. Every stream and timer of IP's
 * is in one syncq (SQLVL_MODULE), which is what guards the tables.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dlpi.h>
#include <sys/errno.h>
#include <sys/ipconf.h>
#include <sys/kmem.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/systm.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>

#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806
#define IP_ETHERMTU     1500
#define IP_LOMTU        65535
#define IP_MAXPACKET    65535

struct ip_if *ip_ifs;
static struct ip_lower *ip_lowers;
static struct ip_rt *ip_routes;
static struct ip_upper *ip_uppers;
static struct ip_if ip_lo;
static int ip_ready;
static uint16_t ip_nextid = 1;

static const uint8_t ip_bcast_hw[IP_HWADDRLEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static int ip_open(queue_t *, dev_t *, int, int, struct cred *);
static int ip_close(queue_t *, int, struct cred *);
static int ip_uwput(queue_t *, mblk_t *);
static int ip_lrput(queue_t *, mblk_t *);
static int ip_lwput(queue_t *, mblk_t *);
static void ip_send(mblk_t *);
static void ip_local(mblk_t *, int);

static struct module_info ip_minfo = {
        0x4950, "ip", 0, INFPSZ, 65536, 8192
};
static struct qinit ip_urinit = {
        NULL, NULL, ip_open, ip_close, NULL, &ip_minfo, NULL
};
static struct qinit ip_uwinit = {
        ip_uwput, NULL, NULL, NULL, NULL, &ip_minfo, NULL
};
static struct qinit ip_lrinit = {
        ip_lrput, NULL, NULL, NULL, NULL, &ip_minfo, NULL
};
static struct qinit ip_lwinit = {
        ip_lwput, NULL, NULL, NULL, NULL, &ip_minfo, NULL
};
struct streamtab ipinfo = { &ip_urinit, &ip_uwinit, &ip_lrinit, &ip_lwinit };

/* --- Checksums and tables. --- */

/* The Internet checksum of a buffer, in host order. */
uint16_t
in_cksum_buf(const uint8_t *p, size_t len)
{
        uint32_t sum = 0;

        for (; len > 1; p += 2, len -= 2)
                sum += (uint32_t)p[0] << 8 | p[1];
        if (len > 0)
                sum += (uint32_t)p[0] << 8;
        while ((sum >> 16) != 0)
                sum = (sum & 0xffff) + (sum >> 16);
        return (uint16_t)~sum;
}

/*
 * The ones' complement sum, unfolded, of len bytes of a message from
 * byte off (an even offset), added to sum.
 */
uint32_t
in_sum(const mblk_t *mp, size_t off, size_t len, uint32_t sum)
{
        int odd = 0;

        for (; mp != NULL && len > 0; mp = mp->b_cont) {
                const uint8_t *p = mp->b_rptr;
                size_t n = MBLKL(mp);

                if (off >= n) {
                        off -= n;
                        continue;
                }
                p += off;
                n -= off;
                off = 0;
                if (n > len)
                        n = len;
                len -= n;
                while (n-- > 0) {
                        sum += odd ? *p : (uint32_t)*p << 8;
                        p++;
                        odd = !odd;
                }
                while ((sum >> 16) != 0)
                        sum = (sum & 0xffff) + (sum >> 16);
        }
        return sum;
}

/* The sum of the pseudo-header UDP and TCP checksums cover. */
uint32_t
in_pseudo(in_addr_t src, in_addr_t dst, uint8_t proto, size_t len)
{
        uint32_t s = ntohl(src), d = ntohl(dst);

        return (s >> 16) + (s & 0xffff) + (d >> 16) + (d & 0xffff) +
            proto + (uint32_t)len;
}

/* A sum folded and complemented: the checksum, in host order. */
uint16_t
in_fold(uint32_t sum)
{
        while ((sum >> 16) != 0)
                sum = (sum & 0xffff) + (sum >> 16);
        return (uint16_t)~sum;
}

/* The Internet checksum of len bytes of a message, in host order. */
uint16_t
in_cksum(const mblk_t *mp, size_t len)
{
        return in_fold(in_sum(mp, 0, len, 0));
}

static int
ip_masklen(in_addr_t mask)
{
        uint32_t m = ntohl(mask);
        int n = 0;

        while ((m & 0x80000000u) != 0) {
                n++;
                m <<= 1;
        }
        return n;
}

/* A mask is ones, then zeros. */
static int
ip_maskok(in_addr_t mask)
{
        uint32_t inv = ~ntohl(mask);

        return (inv & (inv + 1)) == 0;
}

/* Whether a name field holds its terminating nul. */
static int
strnlen_ok(const char *s, size_t size)
{
        for (size_t i = 0; i < size; i++)
                if (s[i] == '\0')
                        return 1;
        return 0;
}

static struct ip_if *
ip_if_byname(const char *name)
{
        for (struct ip_if *ifp = ip_ifs; ifp != NULL; ifp = ifp->if_next)
                if (strcmp(ifp->if_name, name) == 0)
                        return ifp;
        return NULL;
}

/* The route to dst with the longest prefix, on an interface that is up. */
static struct ip_rt *
ip_route(in_addr_t dst)
{
        struct ip_rt *best = NULL;
        int bestlen = -1;

        for (struct ip_rt *rt = ip_routes; rt != NULL; rt = rt->rt_next) {
                int len = ip_masklen(rt->rt_mask);

                if ((dst & rt->rt_mask) != rt->rt_dst ||
                    (rt->rt_if->if_flags & IPIF_UP) == 0 || len <= bestlen)
                        continue;
                best = rt;
                bestlen = len;
        }
        return best;
}

static int
ip_route_add(in_addr_t dst, in_addr_t mask, in_addr_t gw, struct ip_if *ifp)
{
        struct ip_rt *rt;

        for (rt = ip_routes; rt != NULL; rt = rt->rt_next)
                if (rt->rt_dst == (dst & mask) && rt->rt_mask == mask)
                        return EEXIST;
        if ((rt = kmem_zalloc(sizeof(*rt), KM_NOSLEEP)) == NULL)
                return ENOMEM;
        rt->rt_dst = dst & mask;
        rt->rt_mask = mask;
        rt->rt_gateway = gw;
        rt->rt_if = ifp;
        rt->rt_next = ip_routes;
        ip_routes = rt;
        return 0;
}

/* Remove the routes through ifp (only its subnet's, if direct). */
static void
ip_route_purge(struct ip_if *ifp, int direct)
{
        struct ip_rt **link = &ip_routes, *rt;

        while ((rt = *link) != NULL) {
                if (rt->rt_if == ifp && (!direct || rt->rt_gateway == 0)) {
                        *link = rt->rt_next;
                        kmem_free(rt, sizeof(*rt));
                } else {
                        link = &rt->rt_next;
                }
        }
}

/* 1 if dst is one of ours, 2 if a broadcast we take, else 0. */
static int
ip_ours(in_addr_t dst)
{
        if (dst == INADDR_BROADCAST)
                return 2;
        if ((ntohl(dst) >> 24) == 127)
                return 1;
        for (struct ip_if *ifp = ip_ifs; ifp != NULL; ifp = ifp->if_next) {
                if ((ifp->if_flags & IPIF_UP) == 0 || ifp->if_addr == 0)
                        continue;
                if (dst == ifp->if_addr)
                        return 1;
                if ((ifp->if_flags & IPIF_BROADCAST) != 0 &&
                    dst == (ifp->if_addr | ~ifp->if_mask))
                        return 2;
        }
        return 0;
}

static void
ip_init(void)
{
        memcpy(ip_lo.if_name, "lo0", 4);
        ip_lo.if_addr = htonl(0x7f000001);
        ip_lo.if_mask = htonl(0xff000000);
        ip_lo.if_flags = IPIF_UP | IPIF_LOOPBACK | IPIF_RUNNING;
        ip_lo.if_mtu = IP_LOMTU;
        ip_ifs = &ip_lo;
        (void)ip_route_add(ip_lo.if_addr, ip_lo.if_mask, 0, &ip_lo);
        ip_ready = 1;
}

/* --- Output. --- */

/* Send a datagram on ifp to the station hw, by its IPv4 lower stream. */
void
ip_xmit(struct ip_if *ifp, const uint8_t *hw, mblk_t *mp)
{
        dl_unitdata_req_t req;
        uint16_t sap = ETHERTYPE_IP;
        mblk_t *cp;
        queue_t *wq = ifp->if_ip != NULL ? ifp->if_ip->il_wq : NULL;

        if (wq == NULL || !canputnext(wq) ||
            (cp = allocb(sizeof(req) + IP_HWADDRLEN + 2, BPRI_MED)) == NULL) {
                ifp->if_drops++;
                freemsg(mp);
                return;
        }
        memset(&req, 0, sizeof(req));
        req.dl_primitive = DL_UNITDATA_REQ;
        req.dl_dest_addr_length = IP_HWADDRLEN + 2;
        req.dl_dest_addr_offset = sizeof(req);
        cp->b_datap->db_type = M_PROTO;
        memcpy(cp->b_wptr, &req, sizeof(req));
        memcpy(cp->b_wptr + sizeof(req), hw, IP_HWADDRLEN);
        memcpy(cp->b_wptr + sizeof(req) + IP_HWADDRLEN, &sap, 2);
        cp->b_wptr += sizeof(req) + IP_HWADDRLEN + 2;
        cp->b_cont = mp;
        ifp->if_opackets++;
        (void)putnext(wq, cp);
}

/* A finished datagram (header contiguous, checksummed): out, or to us. */
void
ip_output(mblk_t *mp)
{
        struct ip *ip = (struct ip *)mp->b_rptr;
        in_addr_t dst = ip->ip_dst;
        struct ip_if *ifp;
        struct ip_rt *rt;
        int ours = ip_ours(dst);

        if (ours == 1) {
                ip_lo.if_opackets++;
                ip_lo.if_ipackets++;
                ip_local(mp, 0);
                return;
        }
        if (dst == INADDR_BROADCAST) {
                /* The first interface that can broadcast; and to us. */
                for (ifp = ip_ifs; ifp != NULL; ifp = ifp->if_next)
                        if ((ifp->if_flags & (IPIF_UP | IPIF_BROADCAST)) ==
                            (IPIF_UP | IPIF_BROADCAST))
                                break;
                if (ifp == NULL) {
                        freemsg(mp);
                        return;
                }
                ip_xmit(ifp, ip_bcast_hw, mp);
                return;
        }
        if ((rt = ip_route(dst)) == NULL) {
                freemsg(mp);            /* no route to the host */
                return;
        }
        ifp = rt->rt_if;
        if ((ifp->if_flags & IPIF_LOOPBACK) != 0) {
                ip_local(mp, 0);
                return;
        }
        if (msgdsize(mp) > ifp->if_mtu) {
                ifp->if_drops++;        /* no fragmentation yet */
                freemsg(mp);
                return;
        }
        if (ours == 2) {
                ip_xmit(ifp, ip_bcast_hw, mp);
                return;
        }
        arp_resolve(ifp, rt->rt_gateway != 0 ? rt->rt_gateway : dst, mp);
}

/*
 * A datagram of ours (an upper stream's, or ICMP's): fill in what IP
 * owns, choose its source if it has none, and send it.
 */
static void
ip_send(mblk_t *mp)
{
        struct ip *ip = (struct ip *)mp->b_rptr;
        size_t hlen = IP_HLEN(ip), len = msgdsize(mp);

        if (ip->ip_src == INADDR_ANY) {
                struct ip_rt *rt;

                if (ip_ours(ip->ip_dst) == 1)
                        ip->ip_src = (ntohl(ip->ip_dst) >> 24) == 127 ?
                            ip_lo.if_addr : ip->ip_dst;
                else if ((rt = ip_route(ip->ip_dst)) != NULL)
                        ip->ip_src = rt->rt_if->if_addr;
                else if (ip->ip_dst == INADDR_BROADCAST && ip_ifs != NULL)
                        ip->ip_src = INADDR_ANY;
        }
        ip->ip_vhl = (uint8_t)(0x40 | (hlen / 4));
        ip->ip_len = htons((uint16_t)len);
        ip->ip_id = htons(ip_nextid++);
        if (ip->ip_ttl == 0)
                ip->ip_ttl = IP_DEFTTL;
        ip->ip_sum = 0;
        ip->ip_sum = htons(in_cksum(mp, hlen));
        /*
         * UDP's and TCP's checksums cover the source, so they are
         * finished here: at offset 6 of UDP's 8-byte header, 16 of
         * TCP's 20.
         */
        if ((ip->ip_p == IPPROTO_UDP || ip->ip_p == IPPROTO_TCP)) {
                size_t off = ip->ip_p == IPPROTO_UDP ? 6 : 16;
                uint8_t proto = ip->ip_p, *th;
                uint16_t sum;

                if (len >= hlen + off + 2 &&
                    pullupmsg(mp, (ssize_t)(hlen + off + 2))) {
                        ip = (struct ip *)mp->b_rptr;
                        th = mp->b_rptr + hlen;
                        th[off] = th[off + 1] = 0;
                        sum = in_fold(in_sum(mp, hlen, len - hlen,
                            in_pseudo(ip->ip_src, ip->ip_dst, proto,
                            len - hlen)));
                        if (sum == 0 && proto == IPPROTO_UDP)
                                sum = 0xffff;   /* 0: no checksum at all */
                        th[off] = (uint8_t)(sum >> 8);
                        th[off + 1] = (uint8_t)sum;
                }
        }
        ip_output(mp);
}

/* --- ICMP. --- */

/* A new datagram of len bytes after a 20-byte header to dst, from src. */
static mblk_t *
ip_newdgram(in_addr_t src, in_addr_t dst, uint8_t proto, size_t len)
{
        mblk_t *mp = allocb(IP_MINHLEN + len, BPRI_MED);
        struct ip *ip;

        if (mp == NULL)
                return NULL;
        memset(mp->b_wptr, 0, IP_MINHLEN + len);
        ip = (struct ip *)mp->b_wptr;
        ip->ip_vhl = 0x45;
        ip->ip_p = proto;
        ip->ip_src = src;
        ip->ip_dst = dst;
        mp->b_wptr += IP_MINHLEN + len;
        return mp;
}

/* An ICMP error about mp (a datagram for us) to its sender; frees mp. */
static void
icmp_error(mblk_t *mp, uint8_t type, uint8_t code)
{
        struct ip *ip = (struct ip *)mp->b_rptr;
        size_t hlen = IP_HLEN(ip), quote = hlen + 8, len = msgdsize(mp);
        struct icmp *ic;
        mblk_t *ep;

        if (len < quote)
                quote = len;
        if (ip->ip_p == IPPROTO_ICMP || ip_ours(ip->ip_src) == 2 ||
            ip->ip_src == INADDR_ANY ||
            (ep = ip_newdgram(ip->ip_dst, ip->ip_src, IPPROTO_ICMP,
            ICMP_MINLEN + quote)) == NULL) {
                freemsg(mp);
                return;
        }
        ic = (struct icmp *)(ep->b_rptr + IP_MINHLEN);
        ic->icmp_type = type;
        ic->icmp_code = code;
        {
                uint8_t *to = (uint8_t *)ic + ICMP_MINLEN;
                size_t done = 0;

                for (mblk_t *bp = mp; bp != NULL && done < quote;
                    bp = bp->b_cont) {
                        size_t n = MBLKL(bp);

                        if (n > quote - done)
                                n = quote - done;
                        memcpy(to + done, bp->b_rptr, n);
                        done += n;
                }
        }
        freemsg(mp);
        ic->icmp_cksum = htons(in_cksum_buf((uint8_t *)ic,
            ICMP_MINLEN + quote));
        ip_send(ep);
}

/* ICMP for us: answer an echo request. Frees mp. */
static void
icmp_input(mblk_t *mp, int bcast)
{
        mblk_t *nmp = msgpullup(mp, -1);
        struct ip *ip;
        struct icmp *ic;
        size_t hlen, len;

        freemsg(mp);
        if (nmp == NULL)
                return;
        ip = (struct ip *)nmp->b_rptr;
        hlen = IP_HLEN(ip);
        len = MBLKL(nmp) - hlen;
        ic = (struct icmp *)(nmp->b_rptr + hlen);
        if (len < ICMP_MINLEN || in_cksum_buf((uint8_t *)ic, len) != 0 ||
            ic->icmp_type != ICMP_ECHO || bcast) {
                freemsg(nmp);
                return;
        }
        /* The reply: the same message back, from where it was sent to. */
        {
                in_addr_t src = ip->ip_dst;

                ip->ip_dst = ip->ip_src;
                ip->ip_src = src;
                ip->ip_ttl = 0;
                ip->ip_off = 0;
                ic->icmp_type = ICMP_ECHOREPLY;
                ic->icmp_cksum = 0;
                ic->icmp_cksum = htons(in_cksum_buf((uint8_t *)ic, len));
        }
        ip_send(nmp);
}

/* --- Input. --- */

/* A datagram for us (header checked): to the upper streams, and ICMP. */
static void
ip_local(mblk_t *mp, int bcast)
{
        struct ip *ip = (struct ip *)mp->b_rptr;
        int proto = ip->ip_p, taken = 0;

        for (struct ip_upper *iu = ip_uppers; iu != NULL; iu = iu->iu_next) {
                mblk_t *dp;

                if (iu->iu_proto != proto && iu->iu_proto != IPPROTO_RAW)
                        continue;
                if (iu->iu_proto == proto)
                        taken = 1;      /* raw (255) streams only listen */
                if (!canputnext(iu->iu_rq) || (dp = dupmsg(mp)) == NULL)
                        continue;       /* full: dropped */
                (void)putnext(iu->iu_rq, dp);
        }
        if (proto == IPPROTO_ICMP)
                icmp_input(mp, bcast);
        else if (!taken && !bcast)
                icmp_error(mp, ICMP_UNREACH, ICMP_UNREACH_PROTOCOL);
        else
                freemsg(mp);
}

/* A datagram from ifp's IPv4 lower stream. */
static void
ip_input(struct ip_if *ifp, mblk_t *mp)
{
        struct ip *ip;
        size_t hlen, len, have = msgdsize(mp);
        int ours;

        ifp->if_ipackets++;
        if (!pullupmsg(mp, IP_MINHLEN))
                goto bad;
        ip = (struct ip *)mp->b_rptr;
        hlen = IP_HLEN(ip);
        if (IP_VERSION(ip) != 4 || hlen < IP_MINHLEN || !pullupmsg(mp,
            (ssize_t)hlen))
                goto bad;
        ip = (struct ip *)mp->b_rptr;
        len = ntohs(ip->ip_len);
        if (in_cksum(mp, hlen) != 0 || len < hlen || len > have)
                goto bad;
        if (len < have)
                (void)adjmsg(mp, -(ssize_t)(have - len));
        if ((ntohs(ip->ip_off) & (IP_MF | IP_OFFMASK)) != 0) {
                ifp->if_drops++;        /* no reassembly yet */
                freemsg(mp);
                return;
        }
        ours = ip_ours(ip->ip_dst);
        if (ours == 0 && (ifp->if_flags & IPIF_BROADCAST) != 0 &&
            ip->ip_dst == (ifp->if_addr | ~ifp->if_mask))
                ours = 2;
        if (ours == 0) {
                freemsg(mp);            /* not a router */
                return;
        }
        ip_local(mp, ours == 2);
        return;
bad:
        ifp->if_ierrors++;
        freemsg(mp);
}

/* --- Interfaces. --- */

static void
ip_dlpi_send(struct ip_lower *il, const void *prim, size_t len)
{
        mblk_t *mp = allocb(len, BPRI_HI);

        if (mp == NULL)
                return;
        mp->b_datap->db_type = M_PROTO;
        memcpy(mp->b_wptr, prim, len);
        mp->b_wptr += len;
        (void)putnext(il->il_wq, mp);
}

static void
ip_unbind(struct ip_lower *il)
{
        dl_unbind_req_t req = { DL_UNBIND_REQ };

        ip_dlpi_send(il, &req, sizeof(req));
}

/*
 * Take the interface away: its routes, its ARP cache, and its lowers,
 * which are unbound unless one is gone (gone). An IP_IFATTACH still
 * waiting is refused.
 */
static void
ip_if_detach(struct ip_if *ifp, struct ip_lower *gone)
{
        struct ip_if **link;

        if (ifp->if_attaching == 0)
                arp_stop(ifp);
        ip_route_purge(ifp, 0);
        if (ifp->if_ioc != NULL) {
                miocnak(ifp->if_iocq, ifp->if_ioc, 0, EIO);
                ifp->if_ioc = NULL;
        }
        for (int i = 0; i < 2; i++) {
                struct ip_lower *il = i == 0 ? ifp->if_ip : ifp->if_arp;

                if (il == NULL)
                        continue;
                il->il_if = NULL;
                il->il_kind = 0;
                if (il != gone)
                        ip_unbind(il);
        }
        for (link = &ip_ifs; *link != NULL; link = &(*link)->if_next)
                if (*link == ifp) {
                        *link = ifp->if_next;
                        break;
                }
        kmem_free(ifp, sizeof(*ifp));
}

/* One acknowledgement more of the three IP_IFATTACH waits for. */
static void
ip_attach_step(struct ip_if *ifp)
{
        if (ifp->if_attaching == 0 || --ifp->if_attaching != 0)
                return;
        ifp->if_flags |= IPIF_BROADCAST | IPIF_RUNNING;
        arp_start(ifp);
        if (ifp->if_ioc != NULL) {
                miocack(ifp->if_iocq, ifp->if_ioc, 0, 0);
                ifp->if_ioc = NULL;
        }
}

/* A DLPI primitive from a lower stream. Frees mp. */
void
ip_dlpi(struct ip_lower *il, mblk_t *mp)
{
        struct ip_if *ifp = il->il_if;
        t_uscalar_t prim;

        if (MBLKL(mp) < sizeof(prim)) {
                freemsg(mp);
                return;
        }
        memcpy(&prim, mp->b_rptr, sizeof(prim));
        switch (prim) {
        case DL_UNITDATA_IND:
                if (ifp != NULL && ifp->if_attaching == 0 &&
                    mp->b_cont != NULL) {
                        mblk_t *dp = mp->b_cont;

                        mp->b_cont = NULL;
                        freeb(mp);
                        if (il->il_kind == IL_IP)
                                ip_input(ifp, dp);
                        else
                                arp_input(ifp, dp);
                        return;
                }
                break;
        case DL_PHYS_ADDR_ACK:
                if (ifp != NULL && MBLKL(mp) >= sizeof(dl_phys_addr_ack_t)) {
                        dl_phys_addr_ack_t ack;

                        memcpy(&ack, mp->b_rptr, sizeof(ack));
                        if (ack.dl_addr_length == IP_HWADDRLEN &&
                            ack.dl_addr_offset + IP_HWADDRLEN <= MBLKL(mp))
                                memcpy(ifp->if_hwaddr, mp->b_rptr +
                                    ack.dl_addr_offset, IP_HWADDRLEN);
                        ip_attach_step(ifp);
                }
                break;
        case DL_BIND_ACK:
                if (ifp != NULL)
                        ip_attach_step(ifp);
                break;
        case DL_ERROR_ACK:
                if (ifp != NULL && ifp->if_attaching != 0)
                        ip_if_detach(ifp, NULL);
                break;
        case DL_UDERROR_IND:
                if (ifp != NULL)
                        ifp->if_drops++;
                break;
        default:
                break;                  /* DL_OK_ACK and the rest */
        }
        freemsg(mp);
}

static struct ip_lower *
ip_lower_bymux(int muxid)
{
        for (struct ip_lower *il = ip_lowers; il != NULL; il = il->il_next)
                if (il->il_muxid == muxid)
                        return il;
        return NULL;
}

static int
ip_ifattach(queue_t *q, mblk_t *mp, const struct ip_ifattach *ia)
{
        struct ip_lower *ipl = ip_lower_bymux(ia->ia_ipmux);
        struct ip_lower *arpl = ip_lower_bymux(ia->ia_arpmux);
        dl_phys_addr_req_t phys = { DL_PHYS_ADDR_REQ, DL_CURR_PHYS_ADDR };
        dl_bind_req_t bind;
        struct ip_if *ifp;

        if (ia->ia_name[0] == '\0' ||
            !strnlen_ok(ia->ia_name, sizeof(ia->ia_name)))
                return EINVAL;
        if (ip_if_byname(ia->ia_name) != NULL)
                return EEXIST;
        if (ipl == NULL || arpl == NULL || ipl == arpl ||
            ipl->il_if != NULL || arpl->il_if != NULL)
                return EINVAL;
        if ((ifp = kmem_zalloc(sizeof(*ifp), KM_NOSLEEP)) == NULL)
                return ENOMEM;
        memcpy(ifp->if_name, ia->ia_name, sizeof(ifp->if_name));
        ifp->if_mtu = IP_ETHERMTU;
        ifp->if_ip = ipl;
        ifp->if_arp = arpl;
        ipl->il_if = arpl->il_if = ifp;
        ipl->il_kind = IL_IP;
        arpl->il_kind = IL_ARP;
        ifp->if_attaching = 3;          /* the address and two binds */
        ifp->if_ioc = mp;
        ifp->if_iocq = q;
        ifp->if_next = ip_ifs;
        ip_ifs = ifp;

        ip_dlpi_send(ipl, &phys, sizeof(phys));
        memset(&bind, 0, sizeof(bind));
        bind.dl_primitive = DL_BIND_REQ;
        bind.dl_service_mode = DL_CLDLS;
        bind.dl_sap = ETHERTYPE_IP;
        ip_dlpi_send(ipl, &bind, sizeof(bind));
        bind.dl_sap = ETHERTYPE_ARP;
        ip_dlpi_send(arpl, &bind, sizeof(bind));
        return 0;
}

static int
ip_ifaddr(const struct ip_ifaddr *ifa)
{
        struct ip_if *ifp = ip_if_byname(ifa->ifa_name);
        int error;

        if (ifp == NULL)
                return ENXIO;
        if (ifp->if_attaching != 0)
                return EBUSY;
        if ((ifp->if_flags & IPIF_LOOPBACK) != 0)
                return EPERM;
        if (ifa->ifa_addr == INADDR_ANY || !ip_maskok(ifa->ifa_mask) ||
            !strnlen_ok(ifa->ifa_name, sizeof(ifa->ifa_name)))
                return EINVAL;
        ip_route_purge(ifp, 1);
        arp_stop(ifp);
        ifp->if_addr = ifa->ifa_addr;
        ifp->if_mask = ifa->ifa_mask;
        arp_start(ifp);
        if ((error = ip_route_add(ifp->if_addr, ifp->if_mask, 0, ifp)) != 0)
                return error;
        ifp->if_flags |= IPIF_UP;
        return 0;
}

static int
ip_routeadd(const struct ip_route *r)
{
        struct ip_if *ifp = NULL;

        if ((r->rt_dst & ~r->rt_mask) != 0)
                return EINVAL;
        if (r->rt_ifname[0] != '\0') {
                if (!strnlen_ok(r->rt_ifname, sizeof(r->rt_ifname)) ||
                    (ifp = ip_if_byname(r->rt_ifname)) == NULL)
                        return ENXIO;
        } else if (r->rt_gateway != 0) {
                struct ip_rt *via = ip_route(r->rt_gateway);

                if (via == NULL || via->rt_gateway != 0)
                        return ENETUNREACH;
                ifp = via->rt_if;
        } else {
                return EINVAL;
        }
        return ip_route_add(r->rt_dst, r->rt_mask, r->rt_gateway, ifp);
}

static int
ip_routedel(const struct ip_route *r)
{
        for (struct ip_rt **link = &ip_routes; *link != NULL;
            link = &(*link)->rt_next) {
                struct ip_rt *rt = *link;

                if (rt->rt_dst != r->rt_dst || rt->rt_mask != r->rt_mask ||
                    (r->rt_gateway != 0 && rt->rt_gateway != r->rt_gateway))
                        continue;
                if (rt->rt_if == &ip_lo)
                        return EPERM;
                *link = rt->rt_next;
                kmem_free(rt, sizeof(*rt));
                return 0;
        }
        return ESRCH;
}

/* The lists: up to room records into buf; the number there are. */
static int
ip_iflist(struct ip_ifinfo *buf, int room)
{
        int n = 0;

        for (struct ip_if *ifp = ip_ifs; ifp != NULL; ifp = ifp->if_next, n++) {
                struct ip_ifinfo *ii = &buf[n];

                if (n >= room)
                        continue;
                memset(ii, 0, sizeof(*ii));
                memcpy(ii->ii_name, ifp->if_name, sizeof(ii->ii_name));
                ii->ii_addr = ifp->if_addr;
                ii->ii_mask = ifp->if_mask;
                ii->ii_flags = ifp->if_flags;
                ii->ii_mtu = ifp->if_mtu;
                if ((ifp->if_flags & IPIF_LOOPBACK) == 0) {
                        memcpy(ii->ii_hwaddr, ifp->if_hwaddr, IP_HWADDRLEN);
                        ii->ii_hwlen = IP_HWADDRLEN;
                }
                ii->ii_ipackets = ifp->if_ipackets;
                ii->ii_opackets = ifp->if_opackets;
                ii->ii_ierrors = ifp->if_ierrors;
                ii->ii_drops = ifp->if_drops;
        }
        return n;
}

static int
ip_routelist(struct ip_route *buf, int room)
{
        int n = 0;

        for (struct ip_rt *rt = ip_routes; rt != NULL; rt = rt->rt_next, n++) {
                if (n >= room)
                        continue;
                memset(&buf[n], 0, sizeof(buf[n]));
                buf[n].rt_dst = rt->rt_dst;
                buf[n].rt_mask = rt->rt_mask;
                buf[n].rt_gateway = rt->rt_gateway;
                memcpy(buf[n].rt_ifname, rt->rt_if->if_name,
                    sizeof(buf[n].rt_ifname));
        }
        return n;
}

/* --- The streams. --- */

static int
ip_open(queue_t *q, dev_t *devp __unused, int flag __unused, int sflag,
    struct cred *cr __unused)
{
        struct ip_upper *iu;
        struct stroptions *opts;
        mblk_t *so;

        if (sflag != CLONEOPEN)
                return ENXIO;
        if (!ip_ready)
                ip_init();
        iu = kmem_zalloc(sizeof(*iu), KM_SLEEP);
        iu->iu_rq = q;
        iu->iu_proto = -1;
        iu->iu_next = ip_uppers;
        ip_uppers = iu;
        q->q_ptr = WR(q)->q_ptr = iu;
        /* Datagrams: one per read, the rest of a long one discarded. */
        if ((so = allocb(sizeof(*opts), BPRI_MED)) != NULL) {
                so->b_datap->db_type = M_SETOPTS;
                opts = (struct stroptions *)so->b_wptr;
                memset(opts, 0, sizeof(*opts));
                opts->so_flags = SO_READOPT;
                opts->so_readopt = RMSGD;
                so->b_wptr += sizeof(*opts);
                (void)putnext(q, so);
        }
        return 0;
}

static int
ip_close(queue_t *q, int flag __unused, struct cred *cr __unused)
{
        struct ip_upper *iu = q->q_ptr, **link;

        for (link = &ip_uppers; *link != NULL; link = &(*link)->iu_next)
                if (*link == iu) {
                        *link = iu->iu_next;
                        break;
                }
        /* An IP_IFATTACH of this stream's goes on, unanswered. */
        for (struct ip_if *ifp = ip_ifs; ifp != NULL; ifp = ifp->if_next)
                if (ifp->if_iocq == WR(q)) {
                        if (ifp->if_ioc != NULL)
                                freemsg(ifp->if_ioc);
                        ifp->if_ioc = NULL;
                        ifp->if_iocq = NULL;
                }
        kmem_free(iu, sizeof(*iu));
        q->q_ptr = WR(q)->q_ptr = NULL;
        return 0;
}

/* An ioctl's data, in one block of at least need bytes; NULL if not. */
static void *
ip_iocdata(mblk_t *mp, size_t need)
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;

        if (ioc->ioc_count < need || mp->b_cont == NULL ||
            !pullupmsg(mp->b_cont, -1) || MBLKL(mp->b_cont) < need)
                return NULL;
        return mp->b_cont->b_rptr;
}

/* Answer a list ioctl: records of size into a block of the room asked. */
static void
ip_listioc(queue_t *q, mblk_t *mp, size_t size,
    int (*fill)(void *, int))
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
        int room = (int)(ioc->ioc_count / size), n;
        mblk_t *dp = room > 0 ? allocb((size_t)room * size, BPRI_MED) : NULL;

        if (room > 0 && dp == NULL) {
                miocnak(q, mp, 0, ENOSR);
                return;
        }
        n = fill(dp != NULL ? dp->b_wptr : NULL, room);
        if (dp != NULL)
                dp->b_wptr += (size_t)(n < room ? n : room) * size;
        if (mp->b_cont != NULL)
                freemsg(mp->b_cont);
        mp->b_cont = dp;
        miocack(q, mp, dp != NULL ? MBLKL(dp) : 0, n);
}

static int
fill_ifs(void *buf, int room)
{
        return ip_iflist(buf, room);
}

static int
fill_routes(void *buf, int room)
{
        return ip_routelist(buf, room);
}

static int
fill_arp(void *buf, int room)
{
        return arp_list(buf, room);
}

static void
ip_ioctl(queue_t *q, mblk_t *mp)
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
        struct ip_upper *iu = q->q_ptr;
        int root = ioc->ioc_cr != NULL && CRED_ISROOT(ioc->ioc_cr);
        int error = 0;
        void *data;

        if (ioc->ioc_count == TRANSPARENT) {
                miocnak(q, mp, 0, EINVAL);
                return;
        }
        switch (ioc->ioc_cmd) {
        case I_LINK:
        case I_PLINK: {
                struct linkblk *lb = ip_iocdata(mp, sizeof(*lb));
                struct ip_lower *il;

                if (lb == NULL || (il = kmem_zalloc(sizeof(*il),
                    KM_NOSLEEP)) == NULL) {
                        error = lb == NULL ? EINVAL : ENOMEM;
                        break;
                }
                il->il_wq = lb->l_qbot;
                il->il_muxid = lb->l_index;
                il->il_next = ip_lowers;
                ip_lowers = il;
                break;
        }
        case I_UNLINK:
        case I_PUNLINK: {
                struct linkblk *lb = ip_iocdata(mp, sizeof(*lb));
                struct ip_lower **link, *il = NULL;

                if (lb == NULL) {
                        error = EINVAL;
                        break;
                }
                for (link = &ip_lowers; *link != NULL;
                    link = &(*link)->il_next)
                        if ((*link)->il_muxid == lb->l_index) {
                                il = *link;
                                *link = il->il_next;
                                break;
                        }
                if (il == NULL) {
                        error = EINVAL;
                        break;
                }
                if (il->il_if != NULL)
                        ip_if_detach(il->il_if, il);
                kmem_free(il, sizeof(*il));
                break;
        }
        case IP_IFATTACH:
                if (!root) {
                        error = EPERM;
                } else if ((data = ip_iocdata(mp,
                    sizeof(struct ip_ifattach))) == NULL) {
                        error = EINVAL;
                } else if ((error = ip_ifattach(q, mp, data)) == 0) {
                        return;         /* answered when the binds are */
                }
                break;
        case IP_IFADDR:
                if (!root)
                        error = EPERM;
                else if ((data = ip_iocdata(mp,
                    sizeof(struct ip_ifaddr))) == NULL)
                        error = EINVAL;
                else
                        error = ip_ifaddr(data);
                break;
        case IP_ROUTEADD:
        case IP_ROUTEDEL:
                if (!root)
                        error = EPERM;
                else if ((data = ip_iocdata(mp,
                    sizeof(struct ip_route))) == NULL)
                        error = EINVAL;
                else if (ioc->ioc_cmd == IP_ROUTEADD)
                        error = ip_routeadd(data);
                else
                        error = ip_routedel(data);
                break;
        case IP_IFLIST:
                ip_listioc(q, mp, sizeof(struct ip_ifinfo), fill_ifs);
                return;
        case IP_ROUTELIST:
                ip_listioc(q, mp, sizeof(struct ip_route), fill_routes);
                return;
        case IP_ARPLIST:
                ip_listioc(q, mp, sizeof(struct ip_arpinfo), fill_arp);
                return;
        case IP_BINDPROTO: {
                int32_t proto;

                if (!root) {
                        error = EPERM;
                        break;
                }
                if ((data = ip_iocdata(mp, sizeof(proto))) == NULL) {
                        error = EINVAL;
                        break;
                }
                memcpy(&proto, data, sizeof(proto));
                if (proto <= 0 || proto > 255)
                        error = EINVAL;
                else
                        iu->iu_proto = proto;
                break;
        }
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

/* A datagram from an upper stream. */
static void
ip_usend(queue_t *q, mblk_t *mp)
{
        struct ip_upper *iu = q->q_ptr;
        struct ip *ip;
        size_t hlen, len = msgdsize(mp);

        if (iu->iu_proto < 0 || len < IP_MINHLEN || len > IP_MAXPACKET ||
            !pullupmsg(mp, IP_MINHLEN)) {
                freemsg(mp);
                return;
        }
        ip = (struct ip *)mp->b_rptr;
        hlen = IP_HLEN(ip);
        if (IP_VERSION(ip) != 4 || hlen < IP_MINHLEN || hlen > len ||
            !pullupmsg(mp, (ssize_t)hlen)) {
                freemsg(mp);
                return;
        }
        ip = (struct ip *)mp->b_rptr;
        if (iu->iu_proto != IPPROTO_RAW && ip->ip_p != iu->iu_proto) {
                freemsg(mp);
                return;
        }
        ip_send(mp);
}

/* An M_CTL from a transport above (struct ip_ctl). */
static void
ip_uctl(mblk_t *mp)
{
        struct ip_ctl ic;
        mblk_t *dp = mp->b_cont;

        if (MBLKL(mp) < sizeof(ic) || dp == NULL) {
                freemsg(mp);
                return;
        }
        memcpy(&ic, mp->b_rptr, sizeof(ic));
        mp->b_cont = NULL;
        freeb(mp);
        if (ic.ic_cmd == IPCTL_UNREACH && pullupmsg(dp, IP_MINHLEN) &&
            IP_HLEN((struct ip *)dp->b_rptr) >= IP_MINHLEN &&
            pullupmsg(dp, (ssize_t)IP_HLEN((struct ip *)dp->b_rptr)) &&
            ip_ours(((struct ip *)dp->b_rptr)->ip_dst) == 1)
                icmp_error(dp, ICMP_UNREACH, (uint8_t)ic.ic_arg);
        else
                freemsg(dp);
}

static int
ip_uwput(queue_t *q, mblk_t *mp)
{
        switch (DB_TYPE(mp)) {
        case M_DATA:
                ip_usend(q, mp);
                return 0;
        case M_CTL:
                ip_uctl(mp);
                return 0;
        case M_IOCTL:
                ip_ioctl(q, mp);
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

/* Up from a lower (DLPI) stream. */
static int
ip_lrput(queue_t *q, mblk_t *mp)
{
        struct ip_lower *il;

        switch (DB_TYPE(mp)) {
        case M_PROTO:
        case M_PCPROTO:
                for (il = ip_lowers; il != NULL; il = il->il_next)
                        if (il->il_wq == WR(q))
                                break;
                if (il == NULL)
                        freemsg(mp);
                else
                        ip_dlpi(il, mp);
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
ip_lwput(queue_t *q, mblk_t *mp)
{
        (void)putnext(q, mp);
        return 0;
}
