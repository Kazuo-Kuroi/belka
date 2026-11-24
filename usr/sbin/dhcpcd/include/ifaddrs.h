/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * getifaddrs(3), as the BSDs have it, for dhcpcd alone: Belka's C
 * library has no such call, so dhcpcd's interface code
 * (usr/sbin/dhcpcd/if-belka.c) makes the list from IP_IFLIST.
 */

#ifndef _DHCPCD_IFADDRS_H_
#define _DHCPCD_IFADDRS_H_

struct sockaddr;

struct ifaddrs {
        struct ifaddrs  *ifa_next;
        char            *ifa_name;
        unsigned int    ifa_flags;
        struct sockaddr *ifa_addr;
        struct sockaddr *ifa_netmask;
        struct sockaddr *ifa_broadaddr;
        void            *ifa_data;
};

#define ifa_dstaddr     ifa_broadaddr

int     getifaddrs(struct ifaddrs **);
void    freeifaddrs(struct ifaddrs *);

#endif /* _DHCPCD_IFADDRS_H_ */
