/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * IP forwarding, from user mode, on the two more virtio-net devices the
 * self-test boot puts on one QEMU hub. vionet1 is attached under /dev/ip
 * as 10.1.0.1/24, with a route to 10.9.0.0/16 through 10.1.0.3; the test
 * plays the other hosts on that wire through raw DLPI streams on
 * vionet2: 10.1.0.2, a sender, and 10.1.0.3, the gateway, answering
 * IP's ARP requests for both. With forwarding off a datagram for
 * elsewhere goes nowhere; on, it comes back out to the gateway with its
 * TTL one less, and the sender gets a redirect; a TTL of 1 brings time
 * exceeded, no route host unreachable; a link broadcast and a martian
 * source go nowhere, and a sender off the subnet gets no redirect. And
 * a datagram of ours too big for the link goes out in fragments, unless
 * it may not be fragmented. Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <sys/dlpi.h>
#include <sys/ipconf.h>
#include <unistd.h>

#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IP    0x0800

/* The self-test boot's station addresses (the Makefile's). */
static const uint8_t ifmac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x57 };
static const uint8_t hmac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x58 };
static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("fwdtest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

/* Addresses, as host-order numbers. */
#define A(a, b, c, d)   ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | \
                            (uint32_t)(c) << 8 | (uint32_t)(d))
#define OURS            A(10, 1, 0, 1)
#define SENDER          A(10, 1, 0, 2)
#define GATEWAY         A(10, 1, 0, 3)
#define FAR             A(10, 9, 1, 1)

static union {
        t_uscalar_t     prim;
        dl_bind_ack_t   bind;
        dl_unitdata_ind_t ind;
        char            buf[256];
} ctl;
static uint8_t pkt[2048];
static int pktlen;

static void
put32(uint8_t *p, uint32_t v)
{
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
}

static uint32_t
get32(const uint8_t *p)
{
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
            (uint32_t)p[2] << 8 | p[3];
}

static uint16_t
get16(const uint8_t *p)
{
        return (uint16_t)(p[0] << 8 | p[1]);
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

/* --- DLPI, for the hosts on vionet2. --- */

static int
dlsend(int fd, const void *req, size_t len, const void *d, size_t dlen)
{
        struct strbuf c = { 0, (int)len, (char *)req };
        struct strbuf dd = { 0, (int)dlen, (char *)d };

        return putmsg(fd, &c, d != NULL ? &dd : NULL, 0);
}

/* The next message on fd, into ctl and pkt; its primitive, or -1. */
static int
dlget(int fd)
{
        struct strbuf c = { sizeof(ctl.buf), 0, ctl.buf };
        struct strbuf d = { sizeof(pkt), 0, (char *)pkt };
        int flags = 0;

        if (getmsg(fd, &c, &d, &flags) < 0 || c.len < (int)sizeof(t_uscalar_t))
                return -1;
        pktlen = d.len;
        return (int)ctl.prim;
}

static int
dlbind(int fd, t_uscalar_t sap)
{
        dl_bind_req_t req;
        struct pollfd p = { fd, POLLIN | POLLPRI, 0 };

        memset(&req, 0, sizeof(req));
        req.dl_primitive = DL_BIND_REQ;
        req.dl_sap = sap;
        req.dl_service_mode = DL_CLDLS;
        if (dlsend(fd, &req, sizeof(req), NULL, 0) != 0 ||
            poll(&p, 1, 2000) != 1)
                return -1;
        return dlget(fd);
}

/* A frame from fd to the station to, of type sap. */
static int
dlsendto(int fd, const uint8_t *to, uint16_t sap, const void *d, size_t len)
{
        struct {
                dl_unitdata_req_t req;
                uint8_t addr[8];
        } m;

        memset(&m, 0, sizeof(m));
        m.req.dl_primitive = DL_UNITDATA_REQ;
        m.req.dl_dest_addr_length = 8;
        m.req.dl_dest_addr_offset = sizeof(m.req);
        memcpy(m.addr, to, 6);
        memcpy(m.addr + 6, &sap, 2);
        return dlsend(fd, &m, sizeof(m), d, len);
}

static int hip = -1, harp = -1;         /* the hosts' streams */

/* An ARP request for one of the hosts: answer it, from vionet2. */
static void
arp_answer(void)
{
        uint8_t reply[28];
        uint32_t want;

        if (pktlen < 28 || get16(pkt + 6) != 1)
                return;
        want = get32(pkt + 24);
        if (want != SENDER && want != GATEWAY)
                return;
        memcpy(reply, pkt, 6);
        reply[6] = 0;
        reply[7] = 2;
        memcpy(reply + 8, hmac, 6);
        put32(reply + 14, want);
        memcpy(reply + 18, pkt + 8, 6);
        memcpy(reply + 24, pkt + 14, 4);
        (void)dlsendto(harp, pkt + 8, ETHERTYPE_ARP, reply, sizeof(reply));
}

/*
 * The next IP datagram to reach the hosts within ms, into pkt (pktlen),
 * answering ARP meanwhile. 0, or -1 if none came.
 */
static int
next_ip(int ms)
{
        for (;;) {
                struct pollfd p[2] = { { hip, POLLIN, 0 },
                    { harp, POLLIN, 0 } };

                if (poll(p, 2, ms) <= 0)
                        return -1;
                if ((p[1].revents & POLLIN) != 0 &&
                    dlget(harp) == DL_UNITDATA_IND)
                        arp_answer();
                if ((p[0].revents & POLLIN) != 0 &&
                    dlget(hip) == DL_UNITDATA_IND && pktlen >= 20)
                        return 0;
        }
}

/* A UDP datagram from src to dst with ttl, sent from vionet2 to to. */
static void
host_send(const uint8_t *to, uint32_t src, uint32_t dst, uint8_t ttl)
{
        uint8_t d[20 + 8 + 16];

        memset(d, 0, sizeof(d));
        d[0] = 0x45;
        d[3] = sizeof(d);
        d[4] = 0x12;
        d[5] = ttl;                     /* the id: tells them apart */
        d[8] = ttl;
        d[9] = 17;
        put32(d + 12, src);
        put32(d + 16, dst);
        put32(d + 20, 4000u << 16 | 4001u);
        d[25] = 8 + 16;
        memcpy(d + 28, "forward this one", 16);
        {
                uint16_t sum = cksum(d, 20);

                d[10] = (uint8_t)(sum >> 8);
                d[11] = (uint8_t)sum;
        }
        CHECK(dlsendto(hip, to, ETHERTYPE_IP, d, sizeof(d)) == 0);
}

/* What came within ms: forwarded datagrams to dst, and ICMP of type. */
static void
watch(int ms, uint32_t dst, int *fwd, int icmptype, int *icmp)
{
        *fwd = 0;
        if (icmp != NULL)
                *icmp = 0;
        while (next_ip(ms) == 0) {
                size_t hl = (size_t)(pkt[0] & 0x0f) * 4;

                if (get32(pkt + 16) == dst && pkt[9] == 17) {
                        ++*fwd;
                        continue;
                }
                if (icmp != NULL && pkt[9] == 1 && pktlen >= (int)hl + 8 &&
                    pkt[hl] == icmptype)
                        ++*icmp;
        }
}

static int
istr(int fd, int cmd, void *data, int len)
{
        struct strioctl sio = { cmd, 0, len, data };

        return ioctl(fd, I_STR, &sio);
}

int
main(void)
{
        int ipfd, d1, d2, mux1, mux2, raw, on, fwd, icmp, n;
        struct ip_ifattach ia;
        struct ip_ifaddr ifa;
        struct ip_route rt;
        size_t hl;

        ipfd = open("/dev/ip", O_RDWR);
        d1 = open("/dev/vionet1", O_RDWR);
        d2 = open("/dev/vionet1", O_RDWR);
        hip = open("/dev/vionet2", O_RDWR);
        harp = open("/dev/vionet2", O_RDWR);
        CHECK(ipfd >= 0 && d1 >= 0 && d2 >= 0 && hip >= 0 && harp >= 0);
        if (ipfd < 0 || d1 < 0 || d2 < 0 || hip < 0 || harp < 0) {
                printf("fwdtest: %d failures\n", failures);
                return failures;
        }
        CHECK(dlbind(hip, ETHERTYPE_IP) == DL_BIND_ACK);
        CHECK(dlbind(harp, ETHERTYPE_ARP) == DL_BIND_ACK);

        /* vionet1 under IP, 10.1.0.1/24; 10.9.0.0/16 by way of .3. */
        mux1 = ioctl(ipfd, I_PLINK, d1);
        mux2 = ioctl(ipfd, I_PLINK, d2);
        CHECK(mux1 > 0 && mux2 > 0);
        close(d1);
        close(d2);
        memset(&ia, 0, sizeof(ia));
        strcpy(ia.ia_name, "vionet1");
        ia.ia_ipmux = mux1;
        ia.ia_arpmux = mux2;
        CHECK(istr(ipfd, IP_IFATTACH, &ia, sizeof(ia)) == 0);
        memset(&ifa, 0, sizeof(ifa));
        strcpy(ifa.ifa_name, "vionet1");
        put32((uint8_t *)&ifa.ifa_addr, OURS);
        put32((uint8_t *)&ifa.ifa_mask, A(255, 255, 255, 0));
        CHECK(istr(ipfd, IP_IFADDR, &ifa, sizeof(ifa)) == 0);
        memset(&rt, 0, sizeof(rt));
        put32((uint8_t *)&rt.rt_dst, A(10, 9, 0, 0));
        put32((uint8_t *)&rt.rt_mask, A(255, 255, 0, 0));
        put32((uint8_t *)&rt.rt_gateway, GATEWAY);
        CHECK(istr(ipfd, IP_ROUTEADD, &rt, sizeof(rt)) == 0);

        /* Off, as it starts: a datagram for elsewhere goes nowhere. */
        CHECK(istr(ipfd, IP_GETFORWARD, NULL, 0) == 0);
        on = 2;
        CHECK(istr(ipfd, IP_SETFORWARD, &on, sizeof(on)) == -1 &&
            errno == EINVAL);
        host_send(ifmac, SENDER, FAR, 5);
        watch(500, FAR, &fwd, 5, &icmp);
        CHECK(fwd == 0 && icmp == 0);

        /* On: to the gateway, its TTL one less; a redirect to it. */
        on = 1;
        CHECK(istr(ipfd, IP_SETFORWARD, &on, sizeof(on)) == 0);
        CHECK(istr(ipfd, IP_GETFORWARD, NULL, 0) == 1);
        host_send(ifmac, SENDER, FAR, 5);
        fwd = icmp = 0;
        for (int i = 0; i < 8 && (fwd == 0 || icmp == 0); i++) {
                if (next_ip(1000) != 0)
                        break;
                hl = (size_t)(pkt[0] & 0x0f) * 4;
                if (get32(pkt + 16) == FAR) {
                        const uint8_t *to = (const uint8_t *)ctl.buf +
                            ctl.ind.dl_dest_addr_offset;

                        CHECK(pkt[8] == 4 && pkt[9] == 17 &&
                            get32(pkt + 12) == SENDER &&
                            cksum(pkt, hl) == 0 && get16(pkt + 2) == 44 &&
                            memcmp(pkt + 28, "forward this one", 16) == 0 &&
                            memcmp(to, hmac, 6) == 0);
                        fwd++;
                } else if (pkt[9] == 1 && pktlen >= (int)hl + 8 + 20) {
                        const uint8_t *ic = pkt + hl;

                        CHECK(ic[0] == 5 && ic[1] == 1 &&
                            get32(ic + 4) == GATEWAY &&
                            get32(pkt + 12) == OURS &&
                            get32(pkt + 16) == SENDER &&
                            get32(ic + 8 + 16) == FAR &&
                            cksum(ic, get16(pkt + 2) - hl) == 0);
                        icmp++;
                }
        }
        CHECK(fwd == 1 && icmp == 1);

        /* A TTL of 1: time exceeded, and not forwarded. */
        host_send(ifmac, SENDER, FAR, 1);
        watch(1000, FAR, &fwd, 11, &icmp);
        CHECK(fwd == 0 && icmp == 1);

        /* No route: host unreachable. */
        host_send(ifmac, SENDER, A(10, 200, 0, 1), 5);
        watch(1000, A(10, 200, 0, 1), &fwd, 3, &icmp);
        CHECK(fwd == 0 && icmp == 1);

        /* Not as a link broadcast, nor from a martian. */
        host_send(bcast, SENDER, FAR, 5);
        watch(500, FAR, &fwd, 5, &icmp);
        CHECK(fwd == 0 && icmp == 0);
        host_send(ifmac, A(127, 0, 0, 5), FAR, 5);
        watch(500, FAR, &fwd, 5, &icmp);
        CHECK(fwd == 0 && icmp == 0);

        /* From off the subnet: forwarded, with no redirect. */
        host_send(ifmac, A(10, 5, 5, 5), FAR, 5);
        watch(1000, FAR, &fwd, 5, &icmp);
        CHECK(fwd == 1 && icmp == 0);

        /* Ours, too big for the link: fragments of 1480 bytes and less. */
        raw = open("/dev/ip", O_RDWR);
        on = 253;
        CHECK(raw >= 0 && istr(raw, IP_BINDPROTO, &on, sizeof(on)) == 0);
        {
                static uint8_t big[20 + 3000], got[3000];
                int frags = 0, last = 0, ok = 1;
                uint16_t id = 0;

                memset(big, 0, sizeof(big));
                big[0] = 0x45;
                big[9] = 253;
                put32(big + 16, SENDER);
                for (size_t i = 0; i < 3000; i++)
                        big[20 + i] = (uint8_t)(i * 13 + 1);
                memset(got, 0, sizeof(got));
                CHECK(write(raw, big, sizeof(big)) == (ssize_t)sizeof(big));
                while (!last && next_ip(1000) == 0) {
                        size_t off, plen;

                        if (pkt[9] != 253)
                                continue;
                        hl = (size_t)(pkt[0] & 0x0f) * 4;
                        off = (size_t)(get16(pkt + 6) & 0x1fff) * 8;
                        plen = get16(pkt + 2) - hl;
                        if (frags == 0)
                                id = get16(pkt + 4);
                        ok &= get16(pkt + 4) == id && cksum(pkt, hl) == 0 &&
                            get16(pkt + 2) <= 1500 && off + plen <= 3000 &&
                            (int)(hl + plen) <= pktlen;
                        if (ok)
                                memcpy(got + off, pkt + hl, plen);
                        last = (get16(pkt + 6) & 0x2000) == 0;
                        frags++;
                }
                CHECK(ok && last && frags == 3 &&
                    memcmp(got, big + 20, 3000) == 0);

                /* Not to be fragmented: it goes nowhere. */
                big[6] = 0x40;
                CHECK(write(raw, big, sizeof(big)) == (ssize_t)sizeof(big));
                n = 0;
                while (next_ip(500) == 0)
                        n += pkt[9] == 253;
                CHECK(n == 0);
        }
        close(raw);

        /* Off again, and vionet1 gone. */
        on = 0;
        CHECK(istr(ipfd, IP_SETFORWARD, &on, sizeof(on)) == 0);
        CHECK(istr(ipfd, IP_GETFORWARD, NULL, 0) == 0);
        CHECK(ioctl(ipfd, I_PUNLINK, mux1) == 0);
        CHECK(ioctl(ipfd, I_PUNLINK, mux2) == 0);
        close(ipfd);
        close(hip);
        close(harp);
        printf("fwdtest: %d failures\n", failures);
        return failures;
}
