/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * The virtio-net DLPI provider, from user mode: /dev/vionet0's
 * DL_INFO_ACK and station address, binding, the errors of the state
 * machine, and a real exchange on QEMU's user-mode network: an ARP
 * request for the gateway, 10.0.2.2, whose reply must come back to the
 * stream bound to the ARP type and to a promiscuous one, and not to one
 * bound to IPv4. The self-test boot gives the device the station
 * address below. Exits with the number of failed checks.
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
#include <unistd.h>

#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IP    0x0800

static const uint8_t station[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t myip[4] = { 10, 0, 2, 15 };
static const uint8_t gwip[4] = { 10, 0, 2, 2 };

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("nettest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

static union {
        t_uscalar_t     prim;
        dl_info_ack_t   info;
        dl_bind_ack_t   bind;
        dl_ok_ack_t     ok;
        dl_error_ack_t  error;
        dl_unitdata_ind_t ind;
        dl_uderror_ind_t uderror;
        dl_phys_addr_ack_t phys;
        char            buf[256];
} ctl;
static unsigned char data[2048];
static int ctllen, datalen;

/* Send a primitive (ctl, len bytes), with data if any. */
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
        ctllen = c.len;
        datalen = d.len;
        return (int)ctl.prim;
}

static int
simple(int fd, t_uscalar_t prim)
{
        return dlsend(fd, &prim, sizeof(prim), NULL, 0) == 0 ?
            dlrecv(fd, 2000) : -1;
}

/* An error ack for prim, with errno err. */
static int
is_error(int got, t_uscalar_t prim, t_uscalar_t err)
{
        return got == DL_ERROR_ACK && ctl.error.dl_error_primitive == prim &&
            ctl.error.dl_errno == err;
}

static int
dlbind(int fd, t_uscalar_t sap)
{
        dl_bind_req_t req;

        memset(&req, 0, sizeof(req));
        req.dl_primitive = DL_BIND_REQ;
        req.dl_sap = sap;
        req.dl_service_mode = DL_CLDLS;
        return dlsend(fd, &req, sizeof(req), NULL, 0) == 0 ?
            dlrecv(fd, 2000) : -1;
}

static int
promisc(int fd, t_uscalar_t prim, t_uscalar_t level)
{
        dl_promiscon_req_t req = { prim, level };

        return dlsend(fd, &req, sizeof(req), NULL, 0) == 0 ?
            dlrecv(fd, 2000) : -1;
}

/* DL_UNITDATA_REQ to dest (alen bytes: station, then SAP if 8). */
static int
unitdata(int fd, const void *dest, size_t alen, const void *d, size_t dlen)
{
        struct {
                dl_unitdata_req_t req;
                uint8_t addr[8];
        } m;

        memset(&m, 0, sizeof(m));
        m.req.dl_primitive = DL_UNITDATA_REQ;
        m.req.dl_dest_addr_length = (t_uscalar_t)alen;
        m.req.dl_dest_addr_offset = sizeof(m.req);
        memcpy(m.addr, dest, alen);
        return dlsend(fd, &m, sizeof(m.req) + alen, d, dlen);
}

static size_t
arp_request(uint8_t *p)
{
        static const uint8_t head[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };

        memcpy(p, head, 8);
        memcpy(p + 8, station, 6);
        memcpy(p + 14, myip, 4);
        memset(p + 18, 0, 6);
        memcpy(p + 24, gwip, 4);
        return 28;
}

/* The last message received is the gateway's ARP reply to us. */
static int
is_arp_reply(void)
{
        const uint8_t *a;
        uint16_t sap;

        if (ctl.prim != DL_UNITDATA_IND || ctl.ind.dl_dest_addr_length != 8 ||
            ctl.ind.dl_src_addr_length != 8 || datalen < 28)
                return 0;
        a = (const uint8_t *)ctl.buf + ctl.ind.dl_dest_addr_offset;
        memcpy(&sap, a + 6, 2);
        if (memcmp(a, station, 6) != 0 || sap != ETHERTYPE_ARP ||
            ctl.ind.dl_group_address != 0)
                return 0;
        return data[6] == 0 && data[7] == 2 &&          /* a reply */
            memcmp(data + 14, gwip, 4) == 0 &&
            memcmp(data + 18, station, 6) == 0 &&
            memcmp(data + 24, myip, 4) == 0 &&
            memcmp(data + 8, broadcast, 6) != 0;
}

/* Wait for the ARP reply, passing over anything else that comes. */
static int
await_arp(int fd)
{
        for (int i = 0; i < 8; i++) {
                if (dlrecv(fd, 3000) < 0)
                        return 0;
                if (is_arp_reply())
                        return 1;
        }
        return 0;
}

int
main(void)
{
        uint8_t arp[28], dest[8], big[1501];
        uint16_t sap = ETHERTYPE_ARP;
        int fd, ip, pr, n, got;

        fd = open("/dev/vionet0", O_RDWR);
        CHECK(fd >= 0);
        if (fd < 0) {
                printf("nettest: %d failures\n", failures);
                return failures;
        }

        /* What the provider says it is. */
        got = simple(fd, DL_INFO_REQ);
        CHECK(got == DL_INFO_ACK && ctl.info.dl_max_sdu == 1500 &&
            ctl.info.dl_addr_length == 8 && ctl.info.dl_mac_type == DL_ETHER &&
            ctl.info.dl_current_state == DL_UNBOUND &&
            ctl.info.dl_sap_length == -2 &&
            ctl.info.dl_service_mode == DL_CLDLS &&
            ctl.info.dl_provider_style == DL_STYLE1 &&
            ctl.info.dl_version == DL_VERSION_2 &&
            ctl.info.dl_brdcst_addr_length == 6 &&
            memcmp(ctl.buf + ctl.info.dl_brdcst_addr_offset, broadcast, 6) == 0);
        {
                dl_phys_addr_req_t req = { DL_PHYS_ADDR_REQ, DL_CURR_PHYS_ADDR };

                CHECK(dlsend(fd, &req, sizeof(req), NULL, 0) == 0);
                CHECK(dlrecv(fd, 2000) == DL_PHYS_ADDR_ACK &&
                    ctl.phys.dl_addr_length == 6 &&
                    memcmp(ctl.buf + ctl.phys.dl_addr_offset, station, 6) == 0);
        }

        /* The state machine's errors. */
        {
                dl_attach_req_t req = { DL_ATTACH_REQ, 0 };

                CHECK(dlsend(fd, &req, sizeof(req), NULL, 0) == 0);
                CHECK(is_error(dlrecv(fd, 2000), DL_ATTACH_REQ, DL_OUTSTATE));
        }
        CHECK(is_error(simple(fd, 0x77), 0x77, DL_BADPRIM));
        CHECK(is_error(simple(fd, DL_UNBIND_REQ), DL_UNBIND_REQ, DL_OUTSTATE));
        CHECK(unitdata(fd, broadcast, 6, "x", 1) == 0);
        CHECK(is_error(dlrecv(fd, 2000), DL_UNITDATA_REQ, DL_OUTSTATE));
        CHECK(is_error(promisc(fd, DL_PROMISCOFF_REQ, DL_PROMISC_SAP),
            DL_PROMISCOFF_REQ, DL_NOTENAB));
        CHECK(is_error(dlbind(fd, 0x10), DL_BIND_REQ, DL_BADSAP));

        /* Binding to ARP. */
        got = dlbind(fd, ETHERTYPE_ARP);
        CHECK(got == DL_BIND_ACK && ctl.bind.dl_sap == ETHERTYPE_ARP &&
            ctl.bind.dl_addr_length == 8 &&
            memcmp(ctl.buf + ctl.bind.dl_addr_offset, station, 6) == 0 &&
            memcmp(ctl.buf + ctl.bind.dl_addr_offset + 6, &sap, 2) == 0);
        CHECK(is_error(dlbind(fd, ETHERTYPE_ARP), DL_BIND_REQ, DL_OUTSTATE));
        got = simple(fd, DL_INFO_REQ);
        CHECK(got == DL_INFO_ACK && ctl.info.dl_current_state == DL_IDLE);

        /* Two more streams: one for IPv4, one promiscuous for any type. */
        ip = open("/dev/vionet0", O_RDWR | O_NONBLOCK);
        pr = open("/dev/vionet0", O_RDWR);
        CHECK(ip >= 0 && pr >= 0);
        if (ip >= 0)
                CHECK(dlbind(ip, ETHERTYPE_IP) == DL_BIND_ACK);
        if (pr >= 0)
                CHECK(promisc(pr, DL_PROMISCON_REQ, DL_PROMISC_SAP) ==
                    DL_OK_ACK);

        /* Who has 10.0.2.2? */
        memcpy(dest, broadcast, 6);
        memcpy(dest + 6, &sap, 2);
        CHECK(unitdata(fd, dest, 8, arp, arp_request(arp)) == 0);
        CHECK(await_arp(fd));
        if (pr >= 0)
                CHECK(await_arp(pr));
        if (ip >= 0)
                CHECK(ioctl(ip, I_NREAD, &n) == 0);

        /* Again, with the stream's own type implied. */
        CHECK(unitdata(fd, broadcast, 6, arp, sizeof(arp)) == 0);
        CHECK(await_arp(fd));

        /* What cannot be sent. */
        CHECK(unitdata(fd, broadcast, 3, arp, sizeof(arp)) == 0);
        CHECK(dlrecv(fd, 2000) == DL_UDERROR_IND &&
            ctl.uderror.dl_errno == DL_BADADDR);
        memset(big, 0, sizeof(big));
        CHECK(unitdata(fd, dest, 8, big, sizeof(big)) == 0);
        CHECK(dlrecv(fd, 2000) == DL_UDERROR_IND &&
            ctl.uderror.dl_errno == DL_BADDATA);

        /* Unbinding. */
        CHECK(simple(fd, DL_UNBIND_REQ) == DL_OK_ACK &&
            ctl.ok.dl_correct_primitive == DL_UNBIND_REQ);
        got = simple(fd, DL_INFO_REQ);
        CHECK(got == DL_INFO_ACK && ctl.info.dl_current_state == DL_UNBOUND);
        if (pr >= 0) {
                CHECK(promisc(pr, DL_PROMISCOFF_REQ, DL_PROMISC_SAP) ==
                    DL_OK_ACK);
                close(pr);
        }
        if (ip >= 0)
                close(ip);
        close(fd);
        printf("nettest: %d failures\n", failures);
        return failures;
}
