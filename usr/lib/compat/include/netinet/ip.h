/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/* <netinet/ip.h>: the BSDs' limits, after mlibc's header. */

#ifndef _COMPAT_NETINET_IP_H_
#define _COMPAT_NETINET_IP_H_

#include_next <netinet/ip.h>

#ifndef IP_MAXPACKET
#define IP_MAXPACKET    65535           /* the largest datagram */
#endif
#ifndef MAXTTL
#define MAXTTL          255
#endif
#ifndef IPDEFTTL
#define IPDEFTTL        64
#endif

#endif /* _COMPAT_NETINET_IP_H_ */
