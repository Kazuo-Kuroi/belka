/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

#ifndef _NETINET_TCP_H_
#define _NETINET_TCP_H_

/*
 * TCP's options, at level IPPROTO_TCP (numbered as Linux numbers them).
 * The keep-alive times are in seconds.
 */
#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6

#endif /* _NETINET_TCP_H_ */
