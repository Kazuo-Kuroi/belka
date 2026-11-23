/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * ARP (RFC 826) for IPv4 over Ethernet, inside the IP driver: a cache
 * per interface, resolution of next hops (holding a few datagrams while
 * a request is out), answers to requests for our address, and a tick
 * per interface (qtimeout, in IP's perimeter) that resends requests and
 * ages entries. See doc/design/decisions.md, "ARP and IPv4".
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/dlpi.h>
#include <sys/kmem.h>
#include <sys/param.h>
#include <sys/stream.h>
#include <sys/systm.h>

#include <netinet/in.h>
#include <netinet/ip_var.h>

#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806
#define ARPHRD_ETHER    1
#define ARPOP_REQUEST   1
#define ARPOP_REPLY     2

struct arp_pkt {
        uint16_t        ar_hrd;
        uint16_t        ar_pro;
        uint8_t         ar_hln;
        uint8_t         ar_pln;
        uint16_t        ar_op;
        uint8_t         ar_sha[IP_HWADDRLEN];
        uint8_t         ar_spa[4];
        uint8_t         ar_tha[IP_HWADDRLEN];
        uint8_t         ar_tpa[4];
};

CTASSERT(sizeof(struct arp_pkt) == 28);

static const uint8_t arp_bcast[IP_HWADDRLEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
static const uint8_t arp_zero[IP_HWADDRLEN];

static void
arp_send(struct ip_if *ifp, uint16_t op, const uint8_t *to,
    const uint8_t *tha, in_addr_t tpa)
{
        dl_unitdata_req_t req;
        struct arp_pkt ap;
        uint16_t sap = ETHERTYPE_ARP;
        queue_t *wq = ifp->if_arp->il_wq;
        mblk_t *cp, *dp;

        if (!canputnext(wq))
                return;
        if ((cp = allocb(sizeof(req) + IP_HWADDRLEN + 2, BPRI_MED)) == NULL)
                return;
        if ((dp = allocb(sizeof(ap), BPRI_MED)) == NULL) {
                freeb(cp);
                return;
        }
        memset(&req, 0, sizeof(req));
        req.dl_primitive = DL_UNITDATA_REQ;
        req.dl_dest_addr_length = IP_HWADDRLEN + 2;
        req.dl_dest_addr_offset = sizeof(req);
        cp->b_datap->db_type = M_PROTO;
        memcpy(cp->b_wptr, &req, sizeof(req));
        memcpy(cp->b_wptr + sizeof(req), to, IP_HWADDRLEN);
        memcpy(cp->b_wptr + sizeof(req) + IP_HWADDRLEN, &sap, 2);
        cp->b_wptr += sizeof(req) + IP_HWADDRLEN + 2;

        ap.ar_hrd = htons(ARPHRD_ETHER);
        ap.ar_pro = htons(ETHERTYPE_IP);
        ap.ar_hln = IP_HWADDRLEN;
        ap.ar_pln = 4;
        ap.ar_op = htons(op);
        memcpy(ap.ar_sha, ifp->if_hwaddr, IP_HWADDRLEN);
        memcpy(ap.ar_spa, &ifp->if_addr, 4);
        memcpy(ap.ar_tha, tha, IP_HWADDRLEN);
        memcpy(ap.ar_tpa, &tpa, 4);
        memcpy(dp->b_wptr, &ap, sizeof(ap));
        dp->b_wptr += sizeof(ap);
        cp->b_cont = dp;
        (void)putnext(wq, cp);
}

static struct arp_entry *
arp_find(struct ip_if *ifp, in_addr_t addr)
{
        for (struct arp_entry *ae = ifp->if_arp_cache; ae != NULL;
            ae = ae->ae_next)
                if (ae->ae_addr == addr)
                        return ae;
        return NULL;
}

static void
arp_free(struct arp_entry *ae)
{
        mblk_t *mp;

        while ((mp = ae->ae_hold) != NULL) {
                ae->ae_hold = mp->b_next;
                mp->b_next = NULL;
                freemsg(mp);
        }
        kmem_free(ae, sizeof(*ae));
}

/* The station for ae is hw: send what waited for it. */
static void
arp_complete(struct ip_if *ifp, struct arp_entry *ae, const uint8_t *hw)
{
        mblk_t *mp;

        memcpy(ae->ae_hwaddr, hw, IP_HWADDRLEN);
        ae->ae_complete = 1;
        ae->ae_tries = 0;
        ae->ae_ttl = IP_ARPLIFE;
        while ((mp = ae->ae_hold) != NULL) {
                ae->ae_hold = mp->b_next;
                mp->b_next = NULL;
                ip_xmit(ifp, ae->ae_hwaddr, mp);
        }
        ae->ae_nhold = 0;
}

/* Send mp to the next hop nh on ifp, asking for its station if need be. */
void
arp_resolve(struct ip_if *ifp, in_addr_t nh, mblk_t *mp)
{
        struct arp_entry *ae = arp_find(ifp, nh);
        mblk_t **link;

        if (ae != NULL && ae->ae_complete) {
                ip_xmit(ifp, ae->ae_hwaddr, mp);
                return;
        }
        if (ae == NULL) {
                if (ifp->if_addr == INADDR_ANY ||
                    (ae = kmem_zalloc(sizeof(*ae), KM_NOSLEEP)) == NULL) {
                        ifp->if_drops++;
                        freemsg(mp);
                        return;
                }
                ae->ae_addr = nh;
                ae->ae_next = ifp->if_arp_cache;
                ifp->if_arp_cache = ae;
                arp_send(ifp, ARPOP_REQUEST, arp_bcast, arp_zero, nh);
                ae->ae_tries = 1;
        }
        if (ae->ae_nhold == IP_ARPHOLD) {
                mblk_t *old = ae->ae_hold;     /* the oldest goes */

                ae->ae_hold = old->b_next;
                old->b_next = NULL;
                freemsg(old);
                ae->ae_nhold--;
                ifp->if_drops++;
        }
        for (link = &ae->ae_hold; *link != NULL; link = &(*link)->b_next)
                continue;
        mp->b_next = NULL;
        *link = mp;
        ae->ae_nhold++;
}

/* An ARP packet from ifp's ARP lower stream. Frees mp. */
void
arp_input(struct ip_if *ifp, mblk_t *mp)
{
        struct arp_pkt ap;
        struct arp_entry *ae;
        in_addr_t spa, tpa;

        if (!pullupmsg(mp, sizeof(ap))) {
                ifp->if_ierrors++;
                freemsg(mp);
                return;
        }
        memcpy(&ap, mp->b_rptr, sizeof(ap));
        freemsg(mp);
        if (ntohs(ap.ar_hrd) != ARPHRD_ETHER ||
            ntohs(ap.ar_pro) != ETHERTYPE_IP || ap.ar_hln != IP_HWADDRLEN ||
            ap.ar_pln != 4 || (ap.ar_sha[0] & 1) != 0) {
                ifp->if_ierrors++;
                return;
        }
        memcpy(&spa, ap.ar_spa, 4);
        memcpy(&tpa, ap.ar_tpa, 4);
        if (ifp->if_addr == INADDR_ANY || spa == ifp->if_addr ||
            spa == INADDR_ANY)
                return;                 /* unconfigured, ours, or a probe */
        /* Merge what the sender says, then add it if it asks for us. */
        if ((ae = arp_find(ifp, spa)) != NULL) {
                arp_complete(ifp, ae, ap.ar_sha);
        } else if (tpa == ifp->if_addr &&
            (ae = kmem_zalloc(sizeof(*ae), KM_NOSLEEP)) != NULL) {
                ae->ae_addr = spa;
                ae->ae_next = ifp->if_arp_cache;
                ifp->if_arp_cache = ae;
                arp_complete(ifp, ae, ap.ar_sha);
        }
        if (tpa == ifp->if_addr && ntohs(ap.ar_op) == ARPOP_REQUEST)
                arp_send(ifp, ARPOP_REPLY, ap.ar_sha, ap.ar_sha, spa);
}

/* Once a second: resend requests, give up on hops, age entries. */
static void
arp_tick(void *arg)
{
        struct ip_if *ifp = arg;
        struct arp_entry **link = &ifp->if_arp_cache, *ae;

        while ((ae = *link) != NULL) {
                int gone;

                if (!ae->ae_complete) {
                        gone = ae->ae_tries >= IP_ARPTRIES;
                        if (!gone) {
                                arp_send(ifp, ARPOP_REQUEST, arp_bcast,
                                    arp_zero, ae->ae_addr);
                                ae->ae_tries++;
                        } else {
                                ifp->if_drops += (uint64_t)ae->ae_nhold;
                        }
                } else {
                        gone = --ae->ae_ttl <= 0;
                }
                if (gone) {
                        *link = ae->ae_next;
                        arp_free(ae);
                } else {
                        link = &ae->ae_next;
                }
        }
        ip_reass_tick(ifp);
        ifp->if_timer = qtimeout(RD(ifp->if_arp->il_wq), arp_tick, ifp, HZ);
}

void
arp_start(struct ip_if *ifp)
{
        if (ifp->if_arp != NULL && ifp->if_timer == 0)
                ifp->if_timer = qtimeout(RD(ifp->if_arp->il_wq), arp_tick,
                    ifp, HZ);
}

/* Stop the tick and empty the cache (its waiting datagrams dropped). */
void
arp_stop(struct ip_if *ifp)
{
        struct arp_entry *ae;

        if (ifp->if_arp != NULL && ifp->if_timer != 0)
                quntimeout(RD(ifp->if_arp->il_wq), ifp->if_timer);
        ifp->if_timer = 0;
        while ((ae = ifp->if_arp_cache) != NULL) {
                ifp->if_arp_cache = ae->ae_next;
                arp_free(ae);
        }
}

/* Up to room entries of every cache into buf; the number there are. */
int
arp_list(struct ip_arpinfo *buf, int room)
{
        int n = 0;

        for (struct ip_if *ifp = ip_ifs; ifp != NULL; ifp = ifp->if_next) {
                for (struct arp_entry *ae = ifp->if_arp_cache; ae != NULL;
                    ae = ae->ae_next, n++) {
                        struct ip_arpinfo *ai = &buf[n];

                        if (n >= room)
                                continue;
                        memset(ai, 0, sizeof(*ai));
                        memcpy(ai->ai_ifname, ifp->if_name,
                            sizeof(ai->ai_ifname));
                        ai->ai_addr = ae->ae_addr;
                        memcpy(ai->ai_hwaddr, ae->ae_hwaddr, IP_HWADDRLEN);
                        ai->ai_flags = ae->ae_complete ? IPARP_COMPLETE : 0;
                        ai->ai_expire = ae->ae_complete ?
                            (uint32_t)ae->ae_ttl : 0;
                }
        }
        return n;
}
