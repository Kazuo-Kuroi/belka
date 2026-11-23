/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * Internet addresses and protocol numbers, for the kernel. Addresses
 * are kept in network byte order, as on the wire; htonl and the rest
 * convert, in plain C.
 */

#ifndef _NETINET_IN_H_
#define _NETINET_IN_H_

#include <stdint.h>

#include <machine/endian.h>

typedef uint32_t        in_addr_t;
typedef uint16_t        in_port_t;

struct in_addr {
        in_addr_t       s_addr;
};

/* An IPv4 socket address; the port and address in network order. */
struct sockaddr_in {
        uint16_t        sin_family;     /* AF_INET */
        in_port_t       sin_port;
        struct in_addr  sin_addr;
        uint8_t         sin_zero[8];
};

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_RAW     255

static inline uint16_t
in_bswap16(uint16_t x)
{
        return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t
in_bswap32(uint32_t x)
{
        return (x << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) |
            (x >> 24);
}

#if _BYTE_ORDER == _LITTLE_ENDIAN
#define htons(x)        in_bswap16((uint16_t)(x))
#define ntohs(x)        in_bswap16((uint16_t)(x))
#define htonl(x)        in_bswap32((uint32_t)(x))
#define ntohl(x)        in_bswap32((uint32_t)(x))
#else
#define htons(x)        ((uint16_t)(x))
#define ntohs(x)        ((uint16_t)(x))
#define htonl(x)        ((uint32_t)(x))
#define ntohl(x)        ((uint32_t)(x))
#endif

#define INADDR_ANY              ((in_addr_t)0)
#define INADDR_BROADCAST        ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK         ((in_addr_t)0x7f000001)  /* host order */

#endif /* _NETINET_IN_H_ */
