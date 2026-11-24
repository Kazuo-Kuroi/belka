/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * <netinet/ip_icmp.h>: the BSDs' names for ICMP's types and codes (RFC
 * 792 and later), after mlibc's header, which has Linux's names and the
 * BSDs' struct icmp.
 */

#ifndef _COMPAT_NETINET_IP_ICMP_H_
#define _COMPAT_NETINET_IP_ICMP_H_

#include_next <netinet/ip_icmp.h>

#define ICMP_UNREACH            3
#define ICMP_UNREACH_NET        0
#define ICMP_UNREACH_HOST       1
#define ICMP_UNREACH_PROTOCOL   2
#define ICMP_UNREACH_PORT       3
#define ICMP_UNREACH_NEEDFRAG   4
#define ICMP_UNREACH_SRCFAIL    5
#define ICMP_UNREACH_NET_UNKNOWN 6
#define ICMP_UNREACH_HOST_UNKNOWN 7
#define ICMP_UNREACH_ISOLATED   8
#define ICMP_UNREACH_NET_PROHIB 9
#define ICMP_UNREACH_HOST_PROHIB 10
#define ICMP_UNREACH_TOSNET     11
#define ICMP_UNREACH_TOSHOST    12
#define ICMP_UNREACH_FILTER_PROHIB 13
#define ICMP_UNREACH_HOST_PRECEDENCE 14
#define ICMP_UNREACH_PRECEDENCE_CUTOFF 15
#define ICMP_SOURCEQUENCH       4
#define ICMP_REDIRECT_NET       0
#define ICMP_REDIRECT_HOST      1
#define ICMP_REDIRECT_TOSNET    2
#define ICMP_REDIRECT_TOSHOST   3
#define ICMP_ALTHOSTADDR        6
#define ICMP_ROUTERADVERT       9
#define ICMP_ROUTERSOLICIT      10
#define ICMP_TIMXCEED           11
#define ICMP_TIMXCEED_INTRANS   0
#define ICMP_TIMXCEED_REASS     1
#define ICMP_PARAMPROB          12
#define ICMP_PARAMPROB_ERRATPTR 0
#define ICMP_PARAMPROB_OPTABSENT 1
#define ICMP_PARAMPROB_LENGTH   2
#define ICMP_TSTAMP             13
#define ICMP_TSTAMPREPLY        14
#define ICMP_IREQ               15
#define ICMP_IREQREPLY          16
#define ICMP_MASKREQ            17
#define ICMP_MASKREPLY          18
#define ICMP_MAXTYPE            18

#ifndef ICMP_TSLEN
#define ICMP_TSLEN      (8 + 3 * sizeof(uint32_t))      /* timestamps */
#endif
#ifndef ICMP_MASKLEN
#define ICMP_MASKLEN    12                              /* address mask */
#endif

#endif /* _COMPAT_NETINET_IP_ICMP_H_ */
