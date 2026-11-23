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
 * it may not be fragmented. Fragments for us are reassembled, in any
 * order and once only; overlapping ones and ones past IP's limit are
 * dropped, and a datagram never finished brings time exceeded. ICMP
 * port unreachable reaches a connected UDP socket, and a TCP connection
 * being made, as ECONNREFUSED, but not when it quotes what was never
 * sent; and a flood draws only so many errors. Exits with the number
 * of failed checks.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <sys/socket.h>
#include <sys/dlpi.h>
#include <sys/ipconf.h>
#include <netinet/in.h>
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

/* A fragment of a protocol-253 datagram from the sender to us. */
static void
frag_send(uint16_t id, size_t off, int more, size_t len)
{
        static uint8_t d[20 + 1480];

        memset(d, 0, 20);
        d[0] = 0x45;
        put32(d, 0x45u << 24 | (uint32_t)(20 + len));
        d[4] = (uint8_t)(id >> 8);
        d[5] = (uint8_t)id;
        d[6] = (uint8_t)(((off / 8) >> 8) | (more ? 0x20 : 0));
        d[7] = (uint8_t)(off / 8);
        d[8] = 64;
        d[9] = 253;
        put32(d + 12, SENDER);
        put32(d + 16, OURS);
        for (size_t i = 0; i < len; i++)
                d[20 + i] = (uint8_t)((off + i) * 7 + id);
        {
                uint16_t sum = cksum(d, 20);

                d[10] = (uint8_t)(sum >> 8);
                d[11] = (uint8_t)sum;
        }
        CHECK(dlsendto(hip, ifmac, ETHERTYPE_IP, d, 20 + len) == 0);
}

/*
 * The datagram the raw stream rfd reads within ms, reassembled: its
 * length, or -1 if none came; whether it is id's, len bytes long.
 */
static int
frag_got(int rfd, int ms, uint16_t id, size_t len)
{
        static uint8_t d[20 + 4000];
        struct pollfd p = { rfd, POLLIN, 0 };
        ssize_t n;
        int ok;

        if (poll(&p, 1, ms) <= 0 || (n = read(rfd, d, sizeof(d))) < 20)
                return -1;
        ok = n == (ssize_t)(20 + len) && get16(d + 2) == 20 + len &&
            get16(d + 4) == id && (get16(d + 6) & 0x3fff) == 0 &&
            cksum(d, 20) == 0;
        for (size_t i = 0; ok && i < len; i++)
                ok = d[20 + i] == (uint8_t)(i * 7 + id);
        return ok;
}

/*
 * An ICMP error of type and code from the sender to us, quoting the
 * first qlen bytes of q (a datagram of ours).
 */
static void
icmp_send(uint8_t type, uint8_t code, const uint8_t *q, size_t qlen)
{
        uint8_t d[20 + 8 + 68];
        uint16_t sum;

        if (qlen > 68)
                qlen = 68;
        memset(d, 0, sizeof(d));
        put32(d, 0x45u << 24 | (uint32_t)(28 + qlen));
        d[8] = 64;
        d[9] = 1;
        put32(d + 12, SENDER);
        put32(d + 16, OURS);
        sum = cksum(d, 20);
        d[10] = (uint8_t)(sum >> 8);
        d[11] = (uint8_t)sum;
        d[20] = type;
        d[21] = code;
        memcpy(d + 28, q, qlen);
        sum = cksum(d + 20, 8 + qlen);
        d[22] = (uint8_t)(sum >> 8);
        d[23] = (uint8_t)sum;
        CHECK(dlsendto(hip, ifmac, ETHERTYPE_IP, d, 28 + qlen) == 0);
}

/* The next datagram of ours of proto to port, within ms, into q. */
static size_t
catch_ours(int proto, uint16_t port, uint8_t *q, int ms)
{
        while (next_ip(ms) == 0) {
                size_t hl = (size_t)(pkt[0] & 0x0f) * 4;

                if (pkt[9] == proto && get32(pkt + 16) == SENDER &&
                    pktlen >= (int)hl + 8 && get16(pkt + hl + 2) == port) {
                        size_t n = (size_t)pktlen < 68 ? (size_t)pktlen : 68;

                        memcpy(q, pkt, n);
                        return n;
                }
        }
        return 0;
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

                /*
                 * Fragments for us, reassembled: in order; out of order,
                 * one twice; one overlapping another, and one past what
                 * IP can hold, never.
                 */
                while (frag_got(raw, 0, 0, 0) != -1)
                        continue;
                frag_send(0x101, 0, 1, 1480);
                frag_send(0x101, 1480, 1, 1480);
                frag_send(0x101, 2960, 0, 40);
                CHECK(frag_got(raw, 1000, 0x101, 3000) == 1);
                frag_send(0x102, 2960, 0, 40);
                frag_send(0x102, 0, 1, 1480);
                frag_send(0x102, 0, 1, 1480);
                frag_send(0x102, 1480, 1, 1480);
                CHECK(frag_got(raw, 1000, 0x102, 3000) == 1);
                CHECK(frag_got(raw, 300, 0, 0) == -1);
                frag_send(0x103, 0, 1, 1480);
                frag_send(0x103, 1472, 1, 1480);
                frag_send(0x103, 2952, 0, 48);
                frag_send(0x103, 1480, 1, 1472);
                CHECK(frag_got(raw, 500, 0, 0) == -1);
                frag_send(0x104, 0, 1, 64);
                frag_send(0x104, 65480, 0, 48);
                CHECK(frag_got(raw, 300, 0, 0) == -1);

                /* Never finished: time exceeded, once its time is up. */
                frag_send(0x105, 0, 1, 1480);
                n = 0;
                while (n == 0 && next_ip(40000) == 0) {
                        const uint8_t *ic;

                        hl = (size_t)(pkt[0] & 0x0f) * 4;
                        ic = pkt + hl;
                        if (pkt[9] != 1 || pktlen < (int)hl + 8 + 28 ||
                            ic[0] != 11)
                                continue;
                        CHECK(ic[1] == 1 && get32(pkt + 16) == SENDER &&
                            get16(ic + 8 + 4) == 0x105 &&
                            cksum(ic, get16(pkt + 2) - hl) == 0);
                        n++;
                }
                CHECK(n == 1);
        }
        close(raw);

        /*
         * ICMP errors to the sockets: port unreachable to a connected UDP
         * socket is ECONNREFUSED, once; to a TCP connection being made,
         * ECONNREFUSED too, but only quoting a segment it has in flight.
         */
        {
                struct sockaddr_in sin;
                uint8_t q[68], qx[68];
                size_t qlen;
                socklen_t sl = sizeof(int);
                char b;
                int u, t, err = 0, uctl, tctl, lfd;
                struct pollfd p;

                /* UDP and TCP over IP, as long as this takes. */
                uctl = open("/dev/udp", O_RDWR);
                tctl = open("/dev/tcp", O_RDWR);
                CHECK(uctl >= 0 && tctl >= 0);
                lfd = open("/dev/ip", O_RDWR);
                CHECK(lfd >= 0 && ioctl(uctl, I_PLINK, lfd) > 0);
                close(lfd);
                lfd = open("/dev/ip", O_RDWR);
                CHECK(lfd >= 0 && ioctl(tctl, I_PLINK, lfd) > 0);
                close(lfd);

                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
                sin.sin_addr.s_addr = htonl(SENDER);
                sin.sin_port = htons(5000);
                u = socket(AF_INET, SOCK_DGRAM, 0);
                CHECK(u >= 0 && connect(u, (struct sockaddr *)&sin,
                    sizeof(sin)) == 0 && send(u, "x", 1, 0) == 1);
                qlen = catch_ours(17, 5000, q, 1000);
                CHECK(qlen >= 28);
                memcpy(qx, q, qlen);
                qx[20] ^= 1;            /* another source port */
                icmp_send(3, 3, qx, qlen);
                (void)poll(NULL, 0, 300);
                CHECK(recv(u, &b, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
                icmp_send(3, 3, q, qlen);
                (void)poll(NULL, 0, 300);
                CHECK(recv(u, &b, 1, MSG_DONTWAIT) == -1 &&
                    errno == ECONNREFUSED);
                CHECK(recv(u, &b, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
                close(u);

                sin.sin_port = htons(5001);
                t = socket(AF_INET, SOCK_STREAM, 0);
                CHECK(t >= 0 && fcntl(t, F_SETFL, O_NONBLOCK) == 0 &&
                    connect(t, (struct sockaddr *)&sin, sizeof(sin)) == -1 &&
                    errno == EINPROGRESS);
                qlen = catch_ours(6, 5001, q, 1000);
                CHECK(qlen >= 28);
                memcpy(qx, q, qlen);
                qx[24] ^= 0x80;         /* a sequence number not sent */
                icmp_send(3, 3, qx, qlen);
                p.fd = t;
                p.events = POLLOUT;
                p.revents = 0;
                CHECK(poll(&p, 1, 300) == 0);
                icmp_send(3, 3, q, qlen);
                CHECK(poll(&p, 1, 1000) == 1 &&
                    getsockopt(t, SOL_SOCKET, SO_ERROR, &err, &sl) == 0 &&
                    err == ECONNREFUSED);
                close(t);
                CHECK(ioctl(uctl, I_PUNLINK, MUXID_ALL) == 0 &&
                    ioctl(tctl, I_PUNLINK, MUXID_ALL) == 0);
                close(uctl);
                close(tctl);
        }

        /* A flood of TTL 1: time exceeded for only so many (100 at once). */
        {
                int n11 = 0;

                (void)poll(NULL, 0, 1100);      /* a full bucket */
                for (int i = 0; i < 250; i++)
                        host_send(ifmac, SENDER, FAR, 1);
                watch(1500, FAR, &fwd, 11, &n11);
                CHECK(fwd == 0 && n11 >= 50 && n11 <= 130);
        }

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
