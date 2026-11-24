/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * The IPv4 driver's own state: interfaces, lower streams, routes, the
 * ARP cache, and upper streams. Everything here is touched only inside
 * IP's perimeter (its one syncq), so it has no locks of its own.
 */

#ifndef _NETINET_IP_VAR_H_
#define _NETINET_IP_VAR_H_

#include <stdint.h>

#include <sys/ipconf.h>
#include <sys/stream.h>

#include <netinet/in.h>

#define IP_HWADDRLEN    6
#define IP_ARPHOLD      3       /* datagrams held for an unresolved hop */
#define IP_ARPTRIES     3
#define IP_ARPLIFE      (20 * 60)       /* seconds */

struct ip_if;

/* A lower (DLPI) stream: l_qbot, and what it is for. */
struct ip_lower {
        struct ip_lower *il_next;
        queue_t         *il_wq;
        int             il_muxid;
        struct ip_if    *il_if;         /* NULL: not yet an interface's */
        int             il_kind;        /* IL_IP, IL_ARP */
};

#define IL_IP   1
#define IL_ARP  2

struct arp_entry {
        struct arp_entry *ae_next;
        in_addr_t       ae_addr;
        uint8_t         ae_hwaddr[IP_HWADDRLEN];
        int             ae_complete;
        int             ae_tries;       /* requests sent, while incomplete */
        int             ae_ttl;         /* seconds left, when complete */
        mblk_t          *ae_hold;       /* datagrams waiting, by b_next */
        int             ae_nhold;
};

struct ip_if {
        struct ip_if    *if_next;
        char            if_name[IPCONF_NAMELEN];
        in_addr_t       if_addr;
        in_addr_t       if_mask;
        uint32_t        if_flags;       /* IPIF_* */
        uint32_t        if_mtu;
        uint8_t         if_hwaddr[IP_HWADDRLEN];
        struct ip_lower *if_ip;
        struct ip_lower *if_arp;
        int             if_attaching;   /* acknowledgements still due */
        mblk_t          *if_ioc;        /* the IP_IFATTACH that waits */
        queue_t         *if_iocq;       /* and where to answer it */
        struct arp_entry *if_arp_cache;
        int64_t         if_timer;       /* the ARP tick's qtimeout */
        uint64_t        if_ipackets;
        uint64_t        if_opackets;
        uint64_t        if_ierrors;
        uint64_t        if_drops;
};

struct ip_rt {
        struct ip_rt    *rt_next;
        in_addr_t       rt_dst;
        in_addr_t       rt_mask;
        in_addr_t       rt_gateway;
        struct ip_if    *rt_if;
};

/*
 * M_CTL between IP and a transport linked above it: ic_cmd, and the
 * datagram it is about in b_cont. Down, IPCTL_UNREACH: answer that
 * datagram, one for us, with ICMP destination unreachable of code
 * ic_arg (unless it was a broadcast). Up, IPCTL_ICMPERR: an ICMP error
 * (destination unreachable, time exceeded, parameter problem) about a
 * datagram of ours, to the transport of its protocol: ic_arg is the
 * error's type << 8 | its code, ic_info its second word (network order;
 * a need-frag's next-hop MTU), and b_cont the datagram it quotes, its
 * header and at least the eight bytes after.
 */
struct ip_ctl {
        uint32_t        ic_cmd;
        uint32_t        ic_arg;
        uint32_t        ic_info;
};

#define IPCTL_UNREACH   1
#define IPCTL_ICMPERR   2

/* An upper stream. */
struct ip_upper {
        struct ip_upper *iu_next;
        queue_t         *iu_rq;
        int             iu_proto;       /* -1: none bound */
        int             iu_deaf;        /* IP_DEAF: nothing comes up */
};

extern struct ip_if *ip_ifs;

/* ip.c */
uint16_t in_cksum(const mblk_t *, size_t);
uint16_t in_cksum_buf(const uint8_t *, size_t);
uint32_t in_sum(const mblk_t *, size_t, size_t, uint32_t);
uint32_t in_pseudo(in_addr_t, in_addr_t, uint8_t, size_t);
uint16_t in_fold(uint32_t);
void    ip_output(mblk_t *);
void    ip_xmit(struct ip_if *, const uint8_t *, mblk_t *);
void    ip_dlpi(struct ip_lower *, mblk_t *);
void    icmp_error(mblk_t *, uint8_t, uint8_t, uint32_t);

/* ip_reass.c */
mblk_t  *ip_reass(struct ip_if *, mblk_t *, int *);
void    ip_reass_tick(struct ip_if *);
void    ip_reass_flush(struct ip_if *);

/* arp.c */
void    arp_input(struct ip_if *, mblk_t *);
void    arp_resolve(struct ip_if *, in_addr_t, mblk_t *);
void    arp_start(struct ip_if *);
void    arp_stop(struct ip_if *);
int     arp_list(struct ip_arpinfo *, int);

/*
 * T_OPTMGMT_REQ (netinet/tpi_opt.c): a transport's function for one
 * option, given the request's MGMT_flags, the level, the name and the
 * value (T_NEGOTIATE's), puts the option's value in out (TPI_OPTVAL
 * bytes at most) and *outlen, and returns its status: T_SUCCESS,
 * T_FAILURE (a bad value), T_READONLY or T_NOTSUPPORT.
 */
#define TPI_OPTVAL      16

typedef int tpi_optfn(void *, int, int, int, const void *, size_t, void *,
    size_t *);

void    tpi_optmgmt(queue_t *, mblk_t *, tpi_optfn *, void *);
int     tpi_optint(const void *, size_t, int *);
void    tpi_optout(int, void *, size_t *);

#endif /* _NETINET_IP_VAR_H_ */
