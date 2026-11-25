/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * The Intel driver, from user mode, on each device the self-test boot
 * gives it: /dev/e1000g0, QEMU's 82574 ("e1000e", by MSI-X), and
 * /dev/e1000g1, its 82540EM ("e1000", by INTx routed by the ACPI _PRT),
 * against vionet2 on the same QEMU hub. For each, the station address
 * in DL_INFO_ACK and DL_PHYS_ADDR_ACK; frames of every size each way, padded when short; a broadcast; a frame for
 * another station not delivered, unless the stream is promiscuous; a
 * burst of frames more than the device's rings hold, each way, all in
 * order. Then the device under /dev/ip as 10.4.0.1/24, with the test
 * playing 10.4.0.2 on vionet2: a UDP datagram out, after IP's ARP
 * request is answered, and one in, to a socket. Exits with the number
 * of failed checks.
 */

#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <sys/dlpi.h>
#include <sys/ipconf.h>
#include <sys/socket.h>
#include <unistd.h>

#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_TEST  0x88b5          /* IEEE's for local experiments */
#define BURST           300             /* more than a ring of 128 */

/* The devices: QEMU's 82574 ("e1000e") and 82540EM ("e1000"). */
static const struct {
        const char      *name;
        const char      *path;
        uint8_t         mac[6];
} devs[] = {
        { "e1000g0", "/dev/e1000g0", { 0x52, 0x54, 0x00, 0x12, 0x34, 0x59 } },
        { "e1000g1", "/dev/e1000g1", { 0x52, 0x54, 0x00, 0x12, 0x34, 0x5a } },
};
static const char *e1kname, *e1kpath;  /* the one under test */
static const uint8_t *e1kmac;
static const uint8_t viomac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x58 };
static const uint8_t other[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t ourip[4] = { 10, 4, 0, 1 };
static const uint8_t hostip[4] = { 10, 4, 0, 2 };

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("e1000test: %d: %s\n", __LINE__, #expr); \
                        failures++;                                     \
                }                                                       \
        } while (0)

static union {
        t_uscalar_t     prim;
        dl_info_ack_t   info;
        dl_bind_ack_t   bind;
        dl_ok_ack_t     ok;
        dl_unitdata_ind_t ind;
        dl_phys_addr_ack_t phys;
        char            buf[256];
} ctl;
static uint8_t data[2048];
static int datalen;

static int
dlsend(int fd, const void *req, size_t len, const void *d, size_t dlen)
{
        struct strbuf c = { 0, (int)len, (char *)req };
        struct strbuf dd = { 0, (int)dlen, (char *)d };

        return putmsg(fd, &c, d != NULL ? &dd : NULL, 0);
}

/* The next message, waiting up to ms; its primitive, or -1. */
static int
dlrecv(int fd, int ms)
{
        struct strbuf c = { sizeof(ctl.buf), 0, ctl.buf };
        struct strbuf d = { sizeof(data), 0, (char *)data };
        struct pollfd p = { fd, POLLIN | POLLPRI, 0 };
        int flags = 0;

        if (poll(&p, 1, ms) != 1)
                return -1;
        if (getmsg(fd, &c, &d, &flags) < 0 || c.len < (int)sizeof(t_uscalar_t))
                return -1;
        datalen = d.len < 0 ? 0 : d.len;
        return (int)ctl.prim;
}

static int
dlreq(int fd, const void *req, size_t len)
{
        return dlsend(fd, req, len, NULL, 0) == 0 ? dlrecv(fd, 2000) : -1;
}

static int
dlbind(int fd, t_uscalar_t sap)
{
        dl_bind_req_t req;

        memset(&req, 0, sizeof(req));
        req.dl_primitive = DL_BIND_REQ;
        req.dl_sap = sap;
        req.dl_service_mode = DL_CLDLS;
        return dlreq(fd, &req, sizeof(req));
}

/* A frame of type sap to dest, with d. */
static int
unitdata(int fd, const uint8_t *dest, uint16_t sap, const void *d,
    size_t dlen)
{
        struct {
                dl_unitdata_req_t req;
                uint8_t addr[8];
        } m;

        memset(&m, 0, sizeof(m));
        m.req.dl_primitive = DL_UNITDATA_REQ;
        m.req.dl_dest_addr_length = 8;
        m.req.dl_dest_addr_offset = sizeof(m.req);
        memcpy(m.addr, dest, 6);
        memcpy(m.addr + 6, &sap, 2);
        return dlsend(fd, &m, sizeof(m.req) + 8, d, dlen);
}

/* The last message was a frame from src; its type in *sap. */
static int
from(const uint8_t *src, uint16_t *sap)
{
        const uint8_t *a;

        if (ctl.prim != DL_UNITDATA_IND || ctl.ind.dl_src_addr_length != 8)
                return 0;
        a = (const uint8_t *)ctl.buf + ctl.ind.dl_src_addr_offset;
        memcpy(sap, a + 6, 2);
        return memcmp(a, src, 6) == 0;
}

/* Frame contents: seq, then a pattern from it. */
static void
fill(uint8_t *p, size_t len, unsigned int seq)
{
        for (size_t i = 0; i < len; i++)
                p[i] = (uint8_t)(seq * 7 + i);
}

static int
filled(const uint8_t *p, size_t len, unsigned int seq)
{
        for (size_t i = 0; i < len; i++)
                if (p[i] != (uint8_t)(seq * 7 + i))
                        return 0;
        return 1;
}

/* The next test frame on fd from src: its length, or -1. */
static int
next_test(int fd, const uint8_t *src, int ms)
{
        uint16_t sap;

        for (int i = 0; i < 16; i++) {
                if (dlrecv(fd, ms) != DL_UNITDATA_IND)
                        return -1;
                if (from(src, &sap) && sap == ETHERTYPE_TEST)
                        return datalen;
        }
        return -1;
}

/* Sizes one way: from tx (as src) to the station dst, read on rx. */
static void
sizes(int tx, int rx, const uint8_t *src, const uint8_t *dst)
{
        static const size_t lens[] = { 1, 45, 46, 47, 100, 1000, 1499, 1500 };
        uint8_t buf[1500];

        for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
                size_t len = lens[i];
                int n;

                fill(buf, len, (unsigned int)i);
                CHECK(unitdata(tx, dst, ETHERTYPE_TEST, buf, len) == 0);
                n = next_test(rx, src, 2000);
                /* Short frames come padded to 60 bytes: 46 of data. */
                CHECK(n == (int)(len < 46 ? 46 : len) &&
                    filled(data, len, (unsigned int)i));
                if (len < 46)
                        for (int j = (int)len; j < 46 && n == 46; j++)
                                CHECK(data[j] == 0);
        }
}

/* BURST frames from tx, read in order on rx as they come. */
static void
burst(int tx, int rx, const uint8_t *src, const uint8_t *dst)
{
        uint8_t buf[64];
        unsigned int got = 0, sent = 0, bad = 0;

        while (got < BURST) {
                /* Ahead of the reader by at most 32: the head's limit. */
                while (sent < BURST && sent - got < 32) {
                        fill(buf, sizeof(buf), sent);
                        buf[0] = (uint8_t)sent;
                        buf[1] = (uint8_t)(sent >> 8);
                        if (unitdata(tx, dst, ETHERTYPE_TEST, buf,
                            sizeof(buf)) != 0)
                                break;
                        sent++;
                }
                if (next_test(rx, src, 2000) != (int)sizeof(buf))
                        break;
                if ((unsigned int)(data[0] | data[1] << 8) != got)
                        bad++;
                for (size_t i = 2; i < sizeof(buf); i++)
                        if (data[i] != (uint8_t)(got * 7 + i)) {
                                bad++;
                                break;
                        }
                got++;
        }
        CHECK(got == BURST && bad == 0);
        if (got != BURST || bad != 0)
                printf("e1000test: burst: %u of %u, %u wrong\n", got,
                    BURST, bad);
}

static int
istr(int fd, int cmd, void *d, int len)
{
        struct strioctl sio = { cmd, 0, len, d };

        return ioctl(fd, I_STR, &sio);
}

static uint16_t
cksum(const uint8_t *p, size_t len)
{
        uint32_t sum = 0;

        for (; len > 1; p += 2, len -= 2)
                sum += (uint32_t)(p[0] << 8 | p[1]);
        if (len)
                sum += (uint32_t)p[0] << 8;
        while (sum >> 16)
                sum = (sum & 0xffff) + (sum >> 16);
        return (uint16_t)~sum;
}

/* An ARP reply from the host to IP's request (the last frame). */
static int
answer_arp(int arp)
{
        uint8_t r[28];
        uint16_t sap;

        if (!from(e1kmac, &sap) || sap != ETHERTYPE_ARP || datalen < 28 ||
            data[7] != 1 || memcmp(data + 24, hostip, 4) != 0)
                return 0;
        memcpy(r, data, 8);
        r[7] = 2;
        memcpy(r + 8, viomac, 6);
        memcpy(r + 14, hostip, 4);
        memcpy(r + 18, data + 8, 6);
        memcpy(r + 24, data + 14, 4);
        return unitdata(arp, e1kmac, ETHERTYPE_ARP, r, sizeof(r)) == 0;
}

/* The device under IP, and UDP each way with the host on vionet2. */
static void
udp_over_ip(int vip, int varp)
{
        struct ip_ifattach ia;
        struct ip_ifaddr ifa;
        struct sockaddr_in sin;
        struct pollfd p;
        uint8_t pkt[28 + 5];
        char buf[16];
        int ipfd, uctl, lfd, d1, d2, m1, m2, s, sent = 0, arped = 0;
        uint16_t c;

        /* UDP over IP: the self-test boot has no rc to link it. */
        ipfd = open("/dev/ip", O_RDWR);
        uctl = open("/dev/udp", O_RDWR);
        lfd = open("/dev/ip", O_RDWR);
        d1 = open(e1kpath, O_RDWR);
        d2 = open(e1kpath, O_RDWR);
        CHECK(ipfd >= 0 && uctl >= 0 && lfd >= 0 && d1 >= 0 && d2 >= 0);
        if (ipfd < 0 || uctl < 0 || lfd < 0 || d1 < 0 || d2 < 0)
                return;
        CHECK(ioctl(uctl, I_PLINK, lfd) > 0);
        close(lfd);
        m1 = ioctl(ipfd, I_PLINK, d1);
        m2 = ioctl(ipfd, I_PLINK, d2);
        CHECK(m1 > 0 && m2 > 0);
        close(d1);
        close(d2);
        memset(&ia, 0, sizeof(ia));
        strcpy(ia.ia_name, e1kname);
        ia.ia_ipmux = m1;
        ia.ia_arpmux = m2;
        CHECK(istr(ipfd, IP_IFATTACH, &ia, sizeof(ia)) == 0);
        memset(&ifa, 0, sizeof(ifa));
        strcpy(ifa.ifa_name, e1kname);
        memcpy(&ifa.ifa_addr, ourip, 4);
        ifa.ifa_mask = htonl(0xffffff00);
        CHECK(istr(ipfd, IP_IFADDR, &ifa, sizeof(ifa)) == 0);

        s = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(s >= 0);
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(4000);
        memcpy(&sin.sin_addr, ourip, 4);
        CHECK(bind(s, (struct sockaddr *)&sin, sizeof(sin)) == 0);

        /* Out: IP asks for the host's address, then sends. */
        memcpy(&sin.sin_addr, hostip, 4);
        sin.sin_port = htons(5000);
        CHECK(sendto(s, "hello", 5, 0, (struct sockaddr *)&sin,
            sizeof(sin)) == 5);
        for (int i = 0; i < 8 && !arped; i++)
                if (dlrecv(varp, 3000) == DL_UNITDATA_IND)
                        arped = answer_arp(varp);
        CHECK(arped);
        for (int i = 0; i < 8 && !sent; i++) {
                uint16_t sap;

                if (dlrecv(vip, 3000) != DL_UNITDATA_IND)
                        break;
                sent = from(e1kmac, &sap) && sap == ETHERTYPE_IP &&
                    datalen >= 33 && data[3] == 33 && data[9] == 17 &&
                    memcmp(data + 12, ourip, 4) == 0 &&
                    memcmp(data + 16, hostip, 4) == 0 &&
                    data[22] == 0x13 && data[23] == 0x88 &&
                    memcmp(data + 28, "hello", 5) == 0;
        }
        CHECK(sent);

        /* In: a datagram from the host, to the socket. */
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x45;
        pkt[3] = sizeof(pkt);
        pkt[8] = 64;
        pkt[9] = 17;
        memcpy(pkt + 12, hostip, 4);
        memcpy(pkt + 16, ourip, 4);
        c = cksum(pkt, 20);
        pkt[10] = (uint8_t)(c >> 8);
        pkt[11] = (uint8_t)c;
        pkt[20] = 0x13;                         /* from 5000 */
        pkt[21] = 0x88;
        pkt[22] = 0x0f;                         /* to 4000 */
        pkt[23] = 0xa0;
        pkt[25] = 13;                           /* no checksum */
        memcpy(pkt + 28, "there", 5);
        CHECK(unitdata(vip, e1kmac, ETHERTYPE_IP, pkt, sizeof(pkt)) == 0);
        p.fd = s;
        p.events = POLLIN;
        CHECK(poll(&p, 1, 3000) == 1 && recv(s, buf, sizeof(buf), 0) == 5 &&
            memcmp(buf, "there", 5) == 0);

        close(s);
        CHECK(ioctl(ipfd, I_PUNLINK, m1) == 0);
        CHECK(ioctl(ipfd, I_PUNLINK, m2) == 0);
        CHECK(ioctl(uctl, I_PUNLINK, MUXID_ALL) == 0);
        close(uctl);
        close(ipfd);
}

/* The whole test on the device e1kname, against vionet2. */
static void
test_device(void)
{
        dl_phys_addr_req_t phys = { DL_PHYS_ADDR_REQ, DL_CURR_PHYS_ADDR };
        dl_promiscon_req_t pon = { DL_PROMISCON_REQ, DL_PROMISC_PHYS };
        t_uscalar_t info = DL_INFO_REQ;
        uint8_t buf[64];
        int e1k, vio, vip, varp;

        printf("e1000test: %s\n", e1kname);
        e1k = open(e1kpath, O_RDWR);
        vio = open("/dev/vionet2", O_RDWR);
        vip = open("/dev/vionet2", O_RDWR);
        varp = open("/dev/vionet2", O_RDWR);
        CHECK(e1k >= 0 && vio >= 0 && vip >= 0 && varp >= 0);
        if (e1k < 0 || vio < 0 || vip < 0 || varp < 0)
                return;

        /* The station address. */
        CHECK(dlreq(e1k, &info, sizeof(info)) == DL_INFO_ACK &&
            ctl.info.dl_max_sdu == 1500 && ctl.info.dl_mac_type == DL_ETHER &&
            ctl.info.dl_addr_length == 8 &&
            memcmp(ctl.buf + ctl.info.dl_addr_offset, e1kmac, 6) == 0);
        CHECK(dlreq(e1k, &phys, sizeof(phys)) == DL_PHYS_ADDR_ACK &&
            ctl.phys.dl_addr_length == 6 &&
            memcmp(ctl.buf + ctl.phys.dl_addr_offset, e1kmac, 6) == 0);

        CHECK(dlbind(e1k, ETHERTYPE_TEST) == DL_BIND_ACK);
        CHECK(dlbind(vio, ETHERTYPE_TEST) == DL_BIND_ACK);
        CHECK(dlbind(vip, ETHERTYPE_IP) == DL_BIND_ACK);
        CHECK(dlbind(varp, ETHERTYPE_ARP) == DL_BIND_ACK);

        /* Every size, each way. */
        sizes(e1k, vio, e1kmac, viomac);
        sizes(vio, e1k, viomac, e1kmac);

        /* A broadcast comes; a frame for another station does not. */
        fill(buf, sizeof(buf), 1);
        CHECK(unitdata(vio, broadcast, ETHERTYPE_TEST, buf, sizeof(buf)) == 0);
        CHECK(next_test(e1k, viomac, 2000) == (int)sizeof(buf) &&
            ctl.ind.dl_group_address != 0);
        fill(buf, sizeof(buf), 2);
        CHECK(unitdata(vio, other, ETHERTYPE_TEST, buf, sizeof(buf)) == 0);
        fill(buf, sizeof(buf), 3);
        CHECK(unitdata(vio, e1kmac, ETHERTYPE_TEST, buf, sizeof(buf)) == 0);
        CHECK(next_test(e1k, viomac, 2000) == (int)sizeof(buf) &&
            filled(data, sizeof(buf), 3));
        /* Promiscuous, it does: the device takes every frame then. */
        CHECK(dlreq(e1k, &pon, sizeof(pon)) == DL_OK_ACK);
        fill(buf, sizeof(buf), 4);
        CHECK(unitdata(vio, other, ETHERTYPE_TEST, buf, sizeof(buf)) == 0);
        CHECK(next_test(e1k, viomac, 2000) == (int)sizeof(buf) &&
            filled(data, sizeof(buf), 4));
        pon.dl_primitive = DL_PROMISCOFF_REQ;
        CHECK(dlreq(e1k, &pon, sizeof(pon)) == DL_OK_ACK);

        /* More than the rings hold, each way. */
        burst(e1k, vio, e1kmac, viomac);
        burst(vio, e1k, viomac, e1kmac);

        close(e1k);
        close(vio);
        udp_over_ip(vip, varp);
        close(vip);
        close(varp);
}

int
main(void)
{
        for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
                e1kname = devs[i].name;
                e1kpath = devs[i].path;
                e1kmac = devs[i].mac;
                test_device();
        }
        printf("e1000test: %d failures\n", failures);
        return failures;
}
