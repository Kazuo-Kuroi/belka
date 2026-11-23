/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * UDP, from user mode: link an IP stream under /dev/udp, speak TPI to
 * a /dev/udp stream directly, and then use AF_INET datagram sockets
 * over the loopback: binding, ephemeral ports, sendto and recvfrom,
 * datagram boundaries, truncation, MSG_PEEK, connected sockets and
 * their filtering, ICMP port unreachable (watched on a raw ICMP stream,
 * which also shows IP's UDP checksum is right, since UDP drops bad
 * ones), and the unlinking. Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <sys/ipconf.h>
#include <sys/socket.h>
#include <sys/tihdr.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("udptest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

static struct sockaddr_in
sin_of(uint32_t host, uint16_t port)
{
        struct sockaddr_in sin;

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        sin.sin_addr.s_addr = htonl(host);
        return sin;
}

#define LOOPBACK        0x7f000001u

static int
udp_socket(void)
{
        return socket(AF_INET, SOCK_DGRAM, 0);
}

static int
bind_to(int fd, uint32_t host, uint16_t port)
{
        struct sockaddr_in sin = sin_of(host, port);

        return bind(fd, (struct sockaddr *)&sin, sizeof(sin));
}

/* The port (host order) fd is bound to, or -1. */
static int
port_of(int fd)
{
        struct sockaddr_in sin;
        socklen_t len = sizeof(sin);

        if (getsockname(fd, (struct sockaddr *)&sin, &len) != 0 ||
            len != sizeof(sin) || sin.sin_family != AF_INET)
                return -1;
        return ntohs(sin.sin_port);
}

static ssize_t
send_to(int fd, const void *buf, size_t len, uint32_t host, uint16_t port)
{
        struct sockaddr_in sin = sin_of(host, port);

        return sendto(fd, buf, len, 0, (struct sockaddr *)&sin, sizeof(sin));
}

/* Wait up to ms for fd to be readable. */
static int
readable(int fd, int ms)
{
        struct pollfd p = { fd, POLLIN, 0 };

        return poll(&p, 1, ms) == 1 && (p.revents & POLLIN) != 0;
}

/* TPI on a /dev/udp stream, directly. */
static void
test_tpi(void)
{
        struct T_info_req ir = { T_INFO_REQ };
        struct T_unbind_req ur = { T_UNBIND_REQ };
        union T_primitives pr;
        struct strbuf ctl;
        int fd = open("/dev/udp", O_RDWR), flags;

        CHECK(fd >= 0);
        if (fd < 0)
                return;
        ctl.len = sizeof(ir);
        ctl.buf = (char *)&ir;
        CHECK(putmsg(fd, &ctl, NULL, 0) == 0);
        memset(&pr, 0, sizeof(pr));
        ctl.maxlen = sizeof(pr);
        ctl.buf = (char *)&pr;
        flags = 0;
        CHECK(getmsg(fd, &ctl, NULL, &flags) == 0 && flags == RS_HIPRI);
        CHECK(ctl.len == (int)sizeof(pr.info_ack) &&
            pr.type == T_INFO_ACK && pr.info_ack.SERV_type == T_CLTS &&
            pr.info_ack.CURRENT_state == TS_UNBND &&
            pr.info_ack.ADDR_size == (int)sizeof(struct sockaddr_in) &&
            pr.info_ack.TSDU_size == 65507);

        ctl.len = sizeof(ur);
        ctl.buf = (char *)&ur;
        CHECK(putmsg(fd, &ctl, NULL, 0) == 0);
        ctl.maxlen = sizeof(pr);
        ctl.buf = (char *)&pr;
        flags = 0;
        CHECK(getmsg(fd, &ctl, NULL, &flags) == 0 &&
            pr.type == T_ERROR_ACK && pr.error_ack.ERROR_prim == T_UNBIND_REQ &&
            pr.error_ack.TLI_error == TOUTSTATE);
        close(fd);
}

/* Wait on a raw ICMP stream for port unreachable about a datagram to port. */
static int
await_unreach(int raw, uint16_t port)
{
        uint8_t buf[256];

        for (int i = 0; i < 8 && readable(raw, 2000); i++) {
                ssize_t n = read(raw, buf, sizeof(buf));
                size_t hl;

                if (n < 20)
                        continue;
                hl = (size_t)(buf[0] & 0x0f) * 4;
                /* ICMP 3/3, quoting an IP header and the UDP header. */
                if ((size_t)n < hl + 8 + 20 + 8 || buf[hl] != 3 ||
                    buf[hl + 1] != 3 || buf[hl + 8 + 9] != IPPROTO_UDP)
                        continue;
                {
                        const uint8_t *uh = buf + hl + 8 +
                            (size_t)(buf[hl + 8] & 0x0f) * 4;

                        if ((uint16_t)(uh[2] << 8 | uh[3]) == port)
                                return 1;
                }
        }
        return 0;
}

int
main(void)
{
        struct sockaddr_in from;
        socklen_t len;
        char buf[16384], big[8000];
        int ctl, ipfd, raw, a, b, c, muxid, bport, proto, n;

        /* Plumbing: an IP stream under UDP, once. */
        ctl = open("/dev/udp", O_RDWR);
        ipfd = open("/dev/ip", O_RDWR);
        CHECK(ctl >= 0 && ipfd >= 0);
        if (ctl < 0 || ipfd < 0) {
                printf("udptest: %d failures\n", failures);
                return failures;
        }
        muxid = ioctl(ctl, I_PLINK, ipfd);
        CHECK(muxid > 0);
        close(ipfd);
        ipfd = open("/dev/ip", O_RDWR);
        CHECK(ioctl(ctl, I_PLINK, ipfd) == -1 && errno == EBUSY);
        close(ipfd);

        test_tpi();

        /* Which sockets there are. */
        CHECK(socket(AF_INET, SOCK_STREAM, 0) == -1 &&
            errno == EPROTONOSUPPORT);
        CHECK(socket(AF_INET, SOCK_DGRAM, IPPROTO_TCP) == -1 &&
            errno == EPROTONOSUPPORT);
        CHECK(socket(99, SOCK_DGRAM, 0) == -1 && errno == EAFNOSUPPORT);

        a = udp_socket();
        b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(a >= 0 && b >= 0);
        CHECK(port_of(a) == 0);
        CHECK(bind_to(a, LOOPBACK, 5000) == 0);
        CHECK(port_of(a) == 5000);
        CHECK(bind_to(a, LOOPBACK, 5001) == -1 && errno == EINVAL);
        CHECK(bind_to(b, 0, 5000) == -1 && errno == EADDRINUSE);
        CHECK(bind_to(b, 0, 0) == 0);
        bport = port_of(b);
        CHECK(bport >= 49152);
        CHECK(listen(a, 1) == -1 && errno == EOPNOTSUPP);
        len = sizeof(from);
        CHECK(getpeername(a, (struct sockaddr *)&from, &len) == -1 &&
            errno == ENOTCONN);

        /* There and back. */
        CHECK(send_to(b, "hello", 5, LOOPBACK, 5000) == 5);
        CHECK(readable(a, 2000));
        len = sizeof(from);
        memset(&from, 0, sizeof(from));
        n = (int)recvfrom(a, buf, sizeof(buf), 0, (struct sockaddr *)&from,
            &len);
        CHECK(n == 5 && memcmp(buf, "hello", 5) == 0);
        CHECK(len == sizeof(from) && from.sin_family == AF_INET &&
            ntohs(from.sin_port) == bport &&
            from.sin_addr.s_addr == htonl(LOOPBACK));
        CHECK(sendto(a, "world!", 6, 0, (struct sockaddr *)&from, len) == 6);
        n = (int)recv(b, buf, sizeof(buf), 0);
        CHECK(n == 6 && memcmp(buf, "world!", 6) == 0);

        /* Boundaries, truncation, peeking, and nothing to read. */
        CHECK(send_to(b, "one", 3, LOOPBACK, 5000) == 3);
        CHECK(send_to(b, "two2", 4, LOOPBACK, 5000) == 4);
        CHECK(send_to(b, "0123456789", 10, LOOPBACK, 5000) == 10);
        CHECK(send_to(b, "0123456789", 10, LOOPBACK, 5000) == 10);
        CHECK(send_to(b, "", 0, LOOPBACK, 5000) == 0);
        CHECK(recv(a, buf, sizeof(buf), MSG_PEEK) == 3 &&
            memcmp(buf, "one", 3) == 0);
        CHECK(recv(a, buf, sizeof(buf), 0) == 3);
        CHECK(recv(a, buf, sizeof(buf), 0) == 4 && memcmp(buf, "two2", 4) == 0);
        CHECK(recv(a, buf, 4, 0) == 4 && memcmp(buf, "0123", 4) == 0);
        CHECK(recv(a, buf, 4, MSG_TRUNC) == 10);
        len = sizeof(from);
        CHECK(recvfrom(a, buf, sizeof(buf), 0, (struct sockaddr *)&from,
            &len) == 0 && ntohs(from.sin_port) == bport);
        CHECK(recv(a, buf, sizeof(buf), MSG_DONTWAIT) == -1 &&
            errno == EAGAIN);

        /* A big one; and one too big for UDP. */
        for (size_t i = 0; i < sizeof(big); i++)
                big[i] = (char)(i * 7);
        CHECK(send_to(b, big, sizeof(big), LOOPBACK, 5000) ==
            (ssize_t)sizeof(big));
        CHECK(recv(a, buf, sizeof(buf), 0) == (ssize_t)sizeof(big) &&
            memcmp(buf, big, sizeof(big)) == 0);
        {
                static char huge[70000];

                CHECK(send_to(b, huge, sizeof(huge), LOOPBACK, 5000) == -1 &&
                    errno == EMSGSIZE);
        }

        /* Connected: write and read, and only the peer is heard. */
        from = sin_of(LOOPBACK, 5000);
        CHECK(send(b, "x", 1, 0) == -1 && errno == EDESTADDRREQ);
        CHECK(connect(b, (struct sockaddr *)&from, sizeof(from)) == 0);
        len = sizeof(from);
        CHECK(getpeername(b, (struct sockaddr *)&from, &len) == 0 &&
            len == sizeof(from) && ntohs(from.sin_port) == 5000);
        CHECK(write(b, "via write", 9) == 9);
        CHECK(read(a, buf, sizeof(buf)) == 9 && memcmp(buf, "via write",
            9) == 0);
        c = udp_socket();
        CHECK(c >= 0 && send_to(c, "stranger", 8, LOOPBACK, bport) == 8);
        CHECK(send_to(a, "friend", 6, LOOPBACK, bport) == 6);
        CHECK(read(b, buf, sizeof(buf)) == 6 && memcmp(buf, "friend", 6) == 0);
        CHECK(recv(b, buf, sizeof(buf), MSG_DONTWAIT) == -1 &&
            errno == EAGAIN);
        from.sin_family = AF_UNSPEC;
        CHECK(connect(b, (struct sockaddr *)&from, sizeof(from)) == 0);
        len = sizeof(from);
        CHECK(getpeername(b, (struct sockaddr *)&from, &len) == -1 &&
            errno == ENOTCONN);
        CHECK(send_to(c, "stranger", 8, LOOPBACK, bport) == 8);
        CHECK(recv(b, buf, sizeof(buf), 0) == 8);

        /* Nobody at the port: ICMP port unreachable comes back. */
        raw = open("/dev/ip", O_RDWR);
        proto = IPPROTO_ICMP;
        {
                struct strioctl sio = { IP_BINDPROTO, 0, sizeof(proto),
                    (char *)&proto };

                CHECK(raw >= 0 && ioctl(raw, I_STR, &sio) == 0);
        }
        CHECK(send_to(c, "anyone?", 7, LOOPBACK, 4999) == 7);
        CHECK(await_unreach(raw, 4999));
        close(raw);

        /* Unlinked: datagrams go nowhere. */
        CHECK(ioctl(ctl, I_PUNLINK, MUXID_ALL) == 0);
        CHECK(send_to(b, "gone", 4, LOOPBACK, 5000) == 4);
        CHECK(!readable(a, 300));
        close(a);
        close(b);
        close(c);
        close(ctl);
        printf("udptest: %d failures\n", failures);
        return failures;
}
