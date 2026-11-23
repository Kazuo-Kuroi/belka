/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * Reassembly of the fragments of datagrams for us (RFC 791 3.2, RFC 815;
 * doc/design/decisions.md, "IP reassembly"). A datagram's fragments wait
 * in a queue of their own, keyed by source, destination, protocol and
 * identification, kept in order of offset. A fragment that overlaps
 * another, unless it is the same one again, is taken as an attack and
 * the whole datagram is dropped; so it is for a fragment past the end
 * the last one set, one too long for IP, or one but the last not a
 * multiple of eight bytes. A queue lives IP_REASSLIFE seconds, aged by
 * the tick of the interface its first fragment came by (arp.c); one that
 * dies with its first fragment held answers its sender with time
 * exceeded (RFC 792, code 1). The queues, their fragments, and the bytes
 * they hold are all bounded, the oldest queue making way for new ones.
 * Everything here is inside IP's perimeter, as the rest of IP is.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/stream.h>
#include <sys/systm.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>

#define IP_REASSLIFE    30              /* seconds */
#define IP_REASSMAXQ    32              /* datagrams being reassembled */
#define IP_REASSMAXFRAG 64              /* fragments of one datagram */
#define IP_REASSMAXMEM  (256 * 1024)    /* bytes held, in all */
#define IP_TIMXCEED_REASS 1
#define IP_MAXPACKET    65535

struct ip_reass {
        struct ip_reass *rq_next;
        struct ip_if    *rq_if;         /* its first fragment came by */
        in_addr_t       rq_src;
        in_addr_t       rq_dst;
        uint16_t        rq_id;
        uint8_t         rq_proto;
        uint8_t         rq_bcast;       /* to a broadcast address */
        int             rq_ttl;         /* seconds left */
        size_t          rq_end;         /* the data's length; 0 unknown */
        size_t          rq_have;        /* data bytes held */
        size_t          rq_mem;         /* bytes held, headers too */
        int             rq_nfrag;
        mblk_t          *rq_frags;      /* by b_next, in order of offset */
};

static struct ip_reass *ip_reassq;
static int ip_reass_nq;
static size_t ip_reass_mem;

/* A fragment's offset and data length, in bytes. */
static size_t
ip_fragoff(const mblk_t *mp)
{
        const struct ip *ip = (const struct ip *)mp->b_rptr;

        return (size_t)(ntohs(ip->ip_off) & IP_OFFMASK) * 8;
}

static size_t
ip_fraglen(const mblk_t *mp)
{
        const struct ip *ip = (const struct ip *)mp->b_rptr;

        return ntohs(ip->ip_len) - IP_HLEN(ip);
}

static void
ip_reass_free(struct ip_reass *rq)
{
        mblk_t *mp;

        while ((mp = rq->rq_frags) != NULL) {
                rq->rq_frags = mp->b_next;
                mp->b_next = NULL;
                freemsg(mp);
        }
        ip_reass_mem -= rq->rq_mem;
        ip_reass_nq--;
        kmem_free(rq, sizeof(*rq));
}

/* Take rq off the list and free it, counting a drop on its interface. */
static void
ip_reass_drop(struct ip_reass *rq)
{
        for (struct ip_reass **link = &ip_reassq; *link != NULL;
            link = &(*link)->rq_next)
                if (*link == rq) {
                        *link = rq->rq_next;
                        break;
                }
        rq->rq_if->if_drops += (uint64_t)rq->rq_nfrag;
        ip_reass_free(rq);
}

/* Make way: drop the queue with the least life left. */
static void
ip_reass_evict(void)
{
        struct ip_reass *old = ip_reassq;

        for (struct ip_reass *rq = ip_reassq; rq != NULL; rq = rq->rq_next)
                if (rq->rq_ttl < old->rq_ttl)
                        old = rq;
        if (old != NULL)
                ip_reass_drop(old);
}

/*
 * The datagram, whole, from its queue's fragments: the first one's
 * header, with the length and offset of the whole datagram. Frees rq.
 */
static mblk_t *
ip_reass_join(struct ip_reass *rq)
{
        mblk_t *mp = rq->rq_frags, *fp, *next;
        struct ip *ip;
        size_t hlen;

        rq->rq_frags = NULL;
        for (fp = mp->b_next; fp != NULL; fp = next) {
                next = fp->b_next;
                fp->b_next = NULL;
                (void)adjmsg(fp, (ssize_t)IP_HLEN((struct ip *)fp->b_rptr));
                linkb(mp, fp);
        }
        mp->b_next = NULL;
        ip = (struct ip *)mp->b_rptr;
        hlen = IP_HLEN(ip);
        ip->ip_len = htons((uint16_t)(hlen + rq->rq_end));
        ip->ip_off &= htons(IP_DF);
        ip->ip_sum = 0;
        ip->ip_sum = htons(in_cksum(mp, hlen));
        rq->rq_nfrag = 0;
        ip_reass_free(rq);
        return mp;
}

/*
 * A fragment for us that came by ifp (its header contiguous and
 * checked, the message trimmed to its length). The datagram, whole, if
 * this fragment completes it, and *bcast whether it was to a broadcast
 * address; otherwise NULL, the fragment held or dropped.
 */
mblk_t *
ip_reass(struct ip_if *ifp, mblk_t *mp, int *bcast)
{
        struct ip *ip = (struct ip *)mp->b_rptr;
        size_t hlen = IP_HLEN(ip), off = ip_fragoff(mp),
            len = ip_fraglen(mp), mem = msgdsize(mp);
        int more = (ntohs(ip->ip_off) & IP_MF) != 0;
        struct ip_reass *rq;
        mblk_t **link, *prev = NULL;

        for (rq = ip_reassq; rq != NULL; rq = rq->rq_next)
                if (rq->rq_src == ip->ip_src && rq->rq_dst == ip->ip_dst &&
                    rq->rq_id == ip->ip_id && rq->rq_proto == ip->ip_p)
                        break;
        if ((more && (len == 0 || len % 8 != 0)) ||
            hlen + off + len > IP_MAXPACKET) {
                ifp->if_drops++;
                freemsg(mp);
                if (rq != NULL)
                        ip_reass_drop(rq);      /* and the rest of it */
                return NULL;
        }
        if (rq == NULL) {
                while (ip_reass_nq >= IP_REASSMAXQ ||
                    (ip_reassq != NULL &&
                    ip_reass_mem + mem > IP_REASSMAXMEM))
                        ip_reass_evict();
                if (mem > IP_REASSMAXMEM ||
                    (rq = kmem_zalloc(sizeof(*rq), KM_NOSLEEP)) == NULL) {
                        ifp->if_drops++;
                        freemsg(mp);
                        return NULL;
                }
                rq->rq_if = ifp;
                rq->rq_src = ip->ip_src;
                rq->rq_dst = ip->ip_dst;
                rq->rq_id = ip->ip_id;
                rq->rq_proto = ip->ip_p;
                rq->rq_bcast = (uint8_t)*bcast;
                rq->rq_ttl = IP_REASSLIFE;
                rq->rq_next = ip_reassq;
                ip_reassq = rq;
                ip_reass_nq++;
        }

        /* Where it goes, and whether it fits there. */
        for (link = &rq->rq_frags; *link != NULL; link = &(*link)->b_next) {
                if (ip_fragoff(*link) >= off)
                        break;
                prev = *link;
        }
        if (*link != NULL && ip_fragoff(*link) == off &&
            ip_fraglen(*link) == len &&
            (more || rq->rq_end == off + len)) {
                freemsg(mp);            /* the same one again */
                return NULL;
        }
        if ((prev != NULL && ip_fragoff(prev) + ip_fraglen(prev) > off) ||
            (*link != NULL && off + len > ip_fragoff(*link)) ||
            (rq->rq_end != 0 && off + len > rq->rq_end) ||
            (!more && rq->rq_end != 0 && rq->rq_end != off + len) ||
            (!more && *link != NULL) ||
            rq->rq_nfrag >= IP_REASSMAXFRAG) {
                ifp->if_drops++;
                freemsg(mp);
                ip_reass_drop(rq);
                return NULL;
        }
        while (ip_reass_mem + mem > IP_REASSMAXMEM && ip_reassq != NULL) {
                struct ip_reass *old = ip_reassq;

                for (struct ip_reass *r = ip_reassq; r != NULL;
                    r = r->rq_next)
                        if (r != rq && (old == rq || r->rq_ttl < old->rq_ttl))
                                old = r;
                if (old == rq) {
                        ifp->if_drops++;        /* nothing else to drop */
                        freemsg(mp);
                        ip_reass_drop(rq);
                        return NULL;
                }
                ip_reass_drop(old);
        }
        mp->b_next = *link;
        *link = mp;
        rq->rq_nfrag++;
        rq->rq_have += len;
        rq->rq_mem += mem;
        ip_reass_mem += mem;
        if (!more)
                rq->rq_end = off + len;
        if (rq->rq_end == 0 || rq->rq_have != rq->rq_end)
                return NULL;

        /* Whole: off the list, and joined up. */
        for (struct ip_reass **rl = &ip_reassq; *rl != NULL;
            rl = &(*rl)->rq_next)
                if (*rl == rq) {
                        *rl = rq->rq_next;
                        break;
                }
        *bcast = rq->rq_bcast;
        return ip_reass_join(rq);
}

/*
 * A second gone for the queues whose first fragment came by ifp: those
 * whose time is up are dropped, with time exceeded to the sender if the
 * first fragment had come.
 */
void
ip_reass_tick(struct ip_if *ifp)
{
        struct ip_reass **link = &ip_reassq, *rq;

        while ((rq = *link) != NULL) {
                if (rq->rq_if != ifp || --rq->rq_ttl > 0) {
                        link = &rq->rq_next;
                        continue;
                }
                *link = rq->rq_next;
                if (rq->rq_frags != NULL && ip_fragoff(rq->rq_frags) == 0 &&
                    !rq->rq_bcast) {
                        mblk_t *cp = copymsg(rq->rq_frags);

                        if (cp != NULL)
                                icmp_error(cp, ICMP_TIMXCEED,
                                    IP_TIMXCEED_REASS, 0);
                }
                ifp->if_drops += (uint64_t)rq->rq_nfrag;
                ip_reass_free(rq);
        }
}

/* Drop the queues whose first fragment came by ifp, which is going. */
void
ip_reass_flush(struct ip_if *ifp)
{
        struct ip_reass **link = &ip_reassq, *rq;

        while ((rq = *link) != NULL) {
                if (rq->rq_if != ifp) {
                        link = &rq->rq_next;
                        continue;
                }
                *link = rq->rq_next;
                ip_reass_free(rq);
        }
}
