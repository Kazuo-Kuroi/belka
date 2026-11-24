/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * A link-level address, as Linux's struct sockaddr_ll: how dhcpcd's
 * interface list carries a station address. Only dhcpcd uses it; no
 * Belka socket takes one.
 */

#ifndef _DHCPCD_NETPACKET_PACKET_H_
#define _DHCPCD_NETPACKET_PACKET_H_

#include <sys/socket.h>
#include <stdint.h>

struct sockaddr_ll {
        sa_family_t     sll_family;     /* AF_PACKET */
        uint16_t        sll_protocol;
        int             sll_ifindex;
        uint16_t        sll_hatype;     /* ARPHRD_* */
        uint8_t         sll_pkttype;
        uint8_t         sll_halen;
        uint8_t         sll_addr[8];
};

#endif /* _DHCPCD_NETPACKET_PACKET_H_ */
