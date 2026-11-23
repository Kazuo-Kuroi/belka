/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * IPv4 and ARP, from user mode: link /dev/vionet0 (twice, for IPv4 and
 * ARP) under /dev/ip, attach and configure the interface as QEMU's
 * user-mode network expects (10.0.2.15/24, the gateway 10.0.2.2 as the
 * default route), and ping through a raw ICMP stream: the gateway (by
 * way of ARP), our own address and 127.0.0.1. Then a protocol nobody
 * has bound, which must come back unreachable, and the teardown.
 * Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <sys/ipconf.h>
#include <unistd.h>

static const uint8_t station[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("iptest: %d: %s\n", __LINE__, #expr);    \
                        failures++;                                     \
                }                                                       \
        } while (0)

/* a.b.c.d, in network order. */
static uint32_t
inet(int a, int b, int c, int d)
{
        uint8_t q[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
        uint32_t v;

        memcpy(&v, q, 4);
        return v;
}

static int
istr(int fd, int cmd, void *data, int len)
{
        struct strioctl sio = { cmd, 0, len, data };

        return ioctl(fd, I_STR, &sio);
}

static uint16_t
cksum(const uint8_t *p, size_t len)
{
        uint32_t sum = 0;

        for (; len > 1; p += 2, len -= 2)
                sum += (uint32_t)p[0] << 8 | p[1];
        if (len > 0)
                sum += (uint32_t)p[0] << 8;
        while ((sum >> 16) != 0)
                sum = (sum & 0xffff) + (sum >> 16);
        return (uint16_t)~sum;
}

static void
put16(uint8_t *p, uint16_t v)
{
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
}

static uint16_t
get16(const uint8_t *p)
{
        return (uint16_t)(p[0] << 8 | p[1]);
}

/* An IPv4 datagram to dst of protocol proto, with an ICMP echo inside. */
static size_t
dgram(uint8_t *p, uint32_t dst, int proto, uint16_t id, uint16_t seq)
{
        size_t len = 20 + 8 + 16;

        memset(p, 0, len);
        p[0] = 0x45;
        p[9] = (uint8_t)proto;          /* the rest IP fills in */
        memcpy(p + 16, &dst, 4);
        p[20] = 8;                      /* echo request */
        put16(p + 24, id);
        put16(p + 26, seq);
        memcpy(p + 28, "belka iptest....", 16);
        put16(p + 22, cksum(p + 20, len - 20));
        return len;
}

static uint8_t rbuf[2048];

/*
 * Wait for the ICMP message of type from src, with id and seq for an
 * echo reply, passing over the rest (our own requests, for one).
 */
static int
await(int fd, uint32_t src, int type, uint16_t id, uint16_t seq, int ms)
{
        for (int i = 0; i < 16; i++) {
                struct pollfd p = { fd, POLLIN, 0 };
                ssize_t n;
                size_t hl;

                if (poll(&p, 1, ms) != 1)
                        return 0;
                if ((n = read(fd, rbuf, sizeof(rbuf))) < 28)
                        continue;
                hl = (size_t)(rbuf[0] & 0x0f) * 4;
                if ((rbuf[0] >> 4) != 4 || hl < 20 || (size_t)n < hl + 8 ||
                    get16(rbuf + 2) != (size_t)n || cksum(rbuf, hl) != 0 ||
                    rbuf[9] != 1 || memcmp(rbuf + 12, &src, 4) != 0 ||
                    cksum(rbuf + hl, (size_t)n - hl) != 0 ||
                    rbuf[hl] != type)
                        continue;
                if (type == 0 && (get16(rbuf + hl + 4) != id ||
                    get16(rbuf + hl + 6) != seq))
                        continue;
                return 1;
        }
        return 0;
}

static int
ping(int fd, uint32_t dst, uint16_t seq, int ms)
{
        uint8_t p[64];
        size_t len = dgram(p, dst, 1, 0x4242, seq);

        if (write(fd, p, len) != (ssize_t)len)
                return 0;
        return await(fd, dst, 0, 0x4242, seq, ms);
}

int
main(void)
{
        struct ip_ifinfo ifs[4];
        struct ip_route routes[8];
        struct ip_arpinfo arps[8];
        struct ip_ifattach ia;
        struct ip_ifaddr ifa;
        struct ip_route rt;
        uint8_t p[64];
        int ipfd, d1, d2, m1, m2, raw, any, n, proto, found;

        ipfd = open("/dev/ip", O_RDWR);
        d1 = open("/dev/vionet0", O_RDWR);
        d2 = open("/dev/vionet0", O_RDWR);
        CHECK(ipfd >= 0 && d1 >= 0 && d2 >= 0);
        if (ipfd < 0 || d1 < 0 || d2 < 0) {
                printf("iptest: %d failures\n", failures);
                return failures;
        }

        /* lo0 is there from the first. */
        n = istr(ipfd, IP_IFLIST, ifs, sizeof(ifs));
        CHECK(n == 1 && strcmp(ifs[0].ii_name, "lo0") == 0 &&
            ifs[0].ii_addr == inet(127, 0, 0, 1) &&
            (ifs[0].ii_flags & IPIF_LOOPBACK) != 0);

        /* The interface: two DLPI streams, then its address. */
        m1 = ioctl(ipfd, I_PLINK, d1);
        m2 = ioctl(ipfd, I_PLINK, d2);
        CHECK(m1 > 0 && m2 > 0);
        close(d1);
        close(d2);
        memset(&ia, 0, sizeof(ia));
        strcpy(ia.ia_name, "vionet0");
        ia.ia_ipmux = m1;
        ia.ia_arpmux = m1;
        CHECK(istr(ipfd, IP_IFATTACH, &ia, sizeof(ia)) == -1 &&
            errno == EINVAL);
        ia.ia_arpmux = m2;
        CHECK(istr(ipfd, IP_IFATTACH, &ia, sizeof(ia)) == 0);
        CHECK(istr(ipfd, IP_IFATTACH, &ia, sizeof(ia)) == -1 &&
            errno == EEXIST);
        memset(&ifa, 0, sizeof(ifa));
        strcpy(ifa.ifa_name, "vionet0");
        ifa.ifa_addr = inet(10, 0, 2, 15);
        ifa.ifa_mask = inet(255, 0, 255, 0);
        CHECK(istr(ipfd, IP_IFADDR, &ifa, sizeof(ifa)) == -1 &&
            errno == EINVAL);
        ifa.ifa_mask = inet(255, 255, 255, 0);
        CHECK(istr(ipfd, IP_IFADDR, &ifa, sizeof(ifa)) == 0);
        strcpy(ifa.ifa_name, "nosuch0");
        CHECK(istr(ipfd, IP_IFADDR, &ifa, sizeof(ifa)) == -1 &&
            errno == ENXIO);

        memset(&rt, 0, sizeof(rt));
        rt.rt_gateway = inet(10, 0, 2, 2);
        CHECK(istr(ipfd, IP_ROUTEADD, &rt, sizeof(rt)) == 0);
        CHECK(istr(ipfd, IP_ROUTEADD, &rt, sizeof(rt)) == -1 &&
            errno == EEXIST);
        rt.rt_dst = inet(192, 168, 7, 0);
        rt.rt_mask = inet(255, 255, 255, 0);
        rt.rt_gateway = inet(172, 16, 0, 1);
        CHECK(istr(ipfd, IP_ROUTEADD, &rt, sizeof(rt)) == -1 &&
            errno == ENETUNREACH);

        n = istr(ipfd, IP_IFLIST, ifs, sizeof(ifs));
        found = 0;
        for (int i = 0; i < n && i < 4; i++)
                if (strcmp(ifs[i].ii_name, "vionet0") == 0)
                        found = ifs[i].ii_addr == inet(10, 0, 2, 15) &&
                            ifs[i].ii_mask == inet(255, 255, 255, 0) &&
                            ifs[i].ii_mtu == 1500 && ifs[i].ii_hwlen == 6 &&
                            memcmp(ifs[i].ii_hwaddr, station, 6) == 0 &&
                            (ifs[i].ii_flags & (IPIF_UP | IPIF_BROADCAST |
                            IPIF_RUNNING)) == (IPIF_UP | IPIF_BROADCAST |
                            IPIF_RUNNING);
        CHECK(n == 2 && found);
        n = istr(ipfd, IP_ROUTELIST, routes, sizeof(routes));
        CHECK(n == 3);                  /* loopback, subnet, default */
        CHECK(istr(ipfd, IP_ROUTELIST, NULL, 0) == 3);

        /* A raw ICMP stream. */
        raw = open("/dev/ip", O_RDWR);
        CHECK(raw >= 0);
        proto = 0;
        CHECK(istr(raw, IP_BINDPROTO, &proto, sizeof(proto)) == -1 &&
            errno == EINVAL);
        proto = 1;
        CHECK(istr(raw, IP_BINDPROTO, &proto, sizeof(proto)) == 0);

        CHECK(ping(raw, inet(10, 0, 2, 2), 1, 3000));
        n = istr(ipfd, IP_ARPLIST, arps, sizeof(arps));
        found = 0;
        for (int i = 0; i < n && i < 8; i++)
                if (arps[i].ai_addr == inet(10, 0, 2, 2))
                        found = (arps[i].ai_flags & IPARP_COMPLETE) != 0 &&
                            strcmp(arps[i].ai_ifname, "vionet0") == 0 &&
                            memcmp(arps[i].ai_hwaddr, "\0\0\0\0\0\0", 6) != 0;
        CHECK(found);
        CHECK(ping(raw, inet(10, 0, 2, 2), 2, 3000));   /* from the cache */
        CHECK(ping(raw, inet(10, 0, 2, 15), 3, 2000));
        CHECK(ping(raw, inet(127, 0, 0, 1), 4, 2000));

        /* A protocol no stream has: ICMP protocol unreachable. */
        any = open("/dev/ip", O_RDWR);
        proto = 255;
        CHECK(any >= 0 && istr(any, IP_BINDPROTO, &proto, sizeof(proto)) == 0);
        if (any >= 0) {
                size_t len = dgram(p, inet(10, 0, 2, 15), 200, 1, 1);

                CHECK(write(any, p, len) == (ssize_t)len);
                CHECK(await(raw, inet(10, 0, 2, 15), 3, 0, 0, 2000) &&
                    rbuf[20 + 1] == 2);
                close(any);
        }

        /* Taking the interface apart. */
        CHECK(ioctl(ipfd, I_PUNLINK, MUXID_ALL) == 0);
        n = istr(ipfd, IP_IFLIST, ifs, sizeof(ifs));
        CHECK(n == 1 && strcmp(ifs[0].ii_name, "lo0") == 0);
        CHECK(istr(ipfd, IP_ROUTELIST, routes, sizeof(routes)) == 1);
        CHECK(!ping(raw, inet(10, 0, 2, 2), 5, 500));
        CHECK(ping(raw, inet(127, 0, 0, 1), 6, 2000));
        close(raw);
        close(ipfd);
        printf("iptest: %d failures\n", failures);
        return failures;
}
