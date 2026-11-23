/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/* The IPv4 header (RFC 791) and ICMP's (RFC 792), as on the wire. */

#ifndef _NETINET_IP_H_
#define _NETINET_IP_H_

#include <stdint.h>

#include <netinet/in.h>

struct ip {
        uint8_t         ip_vhl;         /* version 4, header length in words */
        uint8_t         ip_tos;
        uint16_t        ip_len;         /* network order, as all below */
        uint16_t        ip_id;
        uint16_t        ip_off;
        uint8_t         ip_ttl;
        uint8_t         ip_p;
        uint16_t        ip_sum;
        in_addr_t       ip_src;
        in_addr_t       ip_dst;
};

#define IP_VERSION(ip)  ((ip)->ip_vhl >> 4)
#define IP_HLEN(ip)     (((ip)->ip_vhl & 0x0f) * 4)
#define IP_DF           0x4000
#define IP_MF           0x2000
#define IP_OFFMASK      0x1fff
#define IP_DEFTTL       64
#define IP_MINHLEN      20

struct icmp {
        uint8_t         icmp_type;
        uint8_t         icmp_code;
        uint16_t        icmp_cksum;
        uint16_t        icmp_id;        /* echo; otherwise unused */
        uint16_t        icmp_seq;
};

#define ICMP_MINLEN             8
#define ICMP_ECHOREPLY          0
#define ICMP_UNREACH            3
#define ICMP_UNREACH_PROTOCOL   2
#define ICMP_ECHO               8

#endif /* _NETINET_IP_H_ */
