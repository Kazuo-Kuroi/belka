/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * TCP, from user mode: link an IP stream under /dev/tcp, speak TPI to
 * a /dev/tcp stream directly, and then use AF_INET stream sockets over
 * the loopback: listening and binding conflicts, connect and accept and
 * their addresses, a mebibyte each way between two processes (flow
 * control and the windows), orderly release and EPIPE, a refused
 * connection, and a reset for data closed on unread. Exits with the
 * number of failed checks.
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
#include <sys/socket.h>
#include <sys/tihdr.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("tcptest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

#define LOOPBACK        0x7f000001u
#define PORT            6000
#define BULK            (1024 * 1024)

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

static int
tcp_socket(void)
{
        return socket(AF_INET, SOCK_STREAM, 0);
}

static int
bind_to(int fd, uint32_t host, uint16_t port)
{
        struct sockaddr_in sin = sin_of(host, port);

        return bind(fd, (struct sockaddr *)&sin, sizeof(sin));
}

static int
connect_to(int fd, uint32_t host, uint16_t port)
{
        struct sockaddr_in sin = sin_of(host, port);

        return connect(fd, (struct sockaddr *)&sin, sizeof(sin));
}

static int
name_of(int fd, int peer, struct sockaddr_in *sin)
{
        socklen_t len = sizeof(*sin);
        int r = peer ? getpeername(fd, (struct sockaddr *)sin, &len) :
            getsockname(fd, (struct sockaddr *)sin, &len);

        return r == 0 && len == sizeof(*sin) && sin->sin_family == AF_INET ?
            0 : -1;
}

static int
readable(int fd, int ms)
{
        struct pollfd p = { fd, POLLIN, 0 };

        return poll(&p, 1, ms) == 1 && (p.revents & POLLIN) != 0;
}

static unsigned char
pattern(size_t i)
{
        return (unsigned char)(i * 131 + (i >> 11));
}

/* All of len bytes, as a stream gives them; the count read. */
static size_t
read_all(int fd, unsigned char *buf, size_t len)
{
        size_t done = 0;

        while (done < len) {
                ssize_t n = read(fd, buf + done, len - done);

                if (n <= 0)
                        break;
                done += (size_t)n;
        }
        return done;
}

static int
write_all(int fd, const unsigned char *buf, size_t len)
{
        size_t done = 0;

        while (done < len) {
                ssize_t n = write(fd, buf + done, len - done);

                if (n <= 0)
                        return -1;
                done += (size_t)n;
        }
        return 0;
}

static unsigned char bulk[BULK], got[BULK];

/* TPI on a /dev/tcp stream, directly. */
static void
test_tpi(void)
{
        struct T_info_req ir = { T_INFO_REQ };
        struct T_capability_req cq = { T_CAPABILITY_REQ, TC1_ACCEPTOR_ID };
        union T_primitives pr;
        struct strbuf ctl;
        int fd = open("/dev/tcp", O_RDWR), flags;

        CHECK(fd >= 0);
        if (fd < 0)
                return;
        ctl.len = sizeof(ir);
        ctl.buf = (char *)&ir;
        CHECK(putmsg(fd, &ctl, NULL, 0) == 0);
        ctl.maxlen = sizeof(pr);
        ctl.buf = (char *)&pr;
        flags = 0;
        CHECK(getmsg(fd, &ctl, NULL, &flags) == 0 && flags == RS_HIPRI &&
            pr.type == T_INFO_ACK && pr.info_ack.SERV_type == T_COTS_ORD &&
            pr.info_ack.CURRENT_state == TS_UNBND);
        ctl.len = sizeof(cq);
        ctl.buf = (char *)&cq;
        CHECK(putmsg(fd, &ctl, NULL, 0) == 0);
        ctl.maxlen = sizeof(pr);
        ctl.buf = (char *)&pr;
        flags = 0;
        CHECK(getmsg(fd, &ctl, NULL, &flags) == 0 &&
            pr.type == T_CAPABILITY_ACK &&
            (pr.capability_ack.CAP_bits1 & TC1_ACCEPTOR_ID) != 0 &&
            pr.capability_ack.ACCEPTOR_id != 0);
        close(fd);
}

/* A mebibyte each way between two processes, then the orderly release. */
static void
test_bulk(int s)
{
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int c, a, status;
        pid_t pid;

        for (size_t i = 0; i < BULK; i++)
                bulk[i] = pattern(i);
        c = tcp_socket();
        CHECK(c >= 0 && connect_to(c, LOOPBACK, PORT) == 0);
        a = accept(s, (struct sockaddr *)&from, &len);
        CHECK(a >= 0);
        if ((pid = fork()) == 0) {
                /* The child: sends, says it is done, and checks the echo. */
                int bad = 0;

                close(a);
                close(s);
                bad |= write_all(c, bulk, BULK) != 0;
                bad |= shutdown(c, SHUT_WR) != 0;
                bad |= read_all(c, got, BULK) != BULK;
                bad |= memcmp(got, bulk, BULK) != 0;
                bad |= read(c, got, 1) != 0;
                _exit(bad);
        }
        CHECK(pid > 0);
        close(c);
        memset(got, 0, sizeof(got));
        CHECK(read_all(a, got, BULK) == BULK);
        CHECK(memcmp(got, bulk, BULK) == 0);
        CHECK(read(a, got, 1) == 0);    /* the child's FIN */
        CHECK(read(a, got, 1) == 0);
        CHECK(write_all(a, bulk, BULK) == 0);
        CHECK(shutdown(a, SHUT_WR) == 0);
        CHECK(send(a, "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0);
        close(a);
}

int
main(void)
{
        struct sockaddr_in sin, from;
        socklen_t len;
        char buf[256];
        int ctl, ipfd, s, s2, c, a, u, n, fl;

        /* Plumbing: an IP stream under TCP, once. */
        ctl = open("/dev/tcp", O_RDWR);
        ipfd = open("/dev/ip", O_RDWR);
        CHECK(ctl >= 0 && ipfd >= 0);
        if (ctl < 0 || ipfd < 0) {
                printf("tcptest: %d failures\n", failures);
                return failures;
        }
        CHECK(ioctl(ctl, I_PLINK, ipfd) > 0);
        close(ipfd);
        ipfd = open("/dev/ip", O_RDWR);
        CHECK(ioctl(ctl, I_PLINK, ipfd) == -1 && errno == EBUSY);
        close(ipfd);

        test_tpi();
        CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 &&
            errno == EPROTONOSUPPORT);

        /* A listener, and what may not share its port. */
        s = tcp_socket();
        CHECK(s >= 0 && bind_to(s, LOOPBACK, PORT) == 0);
        CHECK(listen(s, 4) == 0);
        CHECK(name_of(s, 0, &sin) == 0 && ntohs(sin.sin_port) == PORT &&
            sin.sin_addr.s_addr == htonl(LOOPBACK));
        s2 = tcp_socket();
        CHECK(bind_to(s2, 0, PORT) == -1 && errno == EADDRINUSE);
        close(s2);
        u = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(listen(u, 1) == -1 && errno == EOPNOTSUPP);
        close(u);
        CHECK(!readable(s, 0));
        fl = fcntl(s, F_GETFL);
        CHECK(fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0);
        CHECK(accept(s, NULL, NULL) == -1 && errno == EAGAIN);
        CHECK(fcntl(s, F_SETFL, fl) == 0);

        /* A connection, and its addresses on both sides. */
        c = tcp_socket();
        CHECK(send(c, "x", 1, MSG_NOSIGNAL) == -1 && errno == ENOTCONN);
        CHECK(connect_to(c, LOOPBACK, PORT) == 0);
        CHECK(connect_to(c, LOOPBACK, PORT) == -1 && errno == EISCONN);
        CHECK(name_of(c, 0, &sin) == 0 &&
            sin.sin_addr.s_addr == htonl(LOOPBACK) &&
            ntohs(sin.sin_port) >= 49152);
        CHECK(name_of(c, 1, &from) == 0 && ntohs(from.sin_port) == PORT);
        CHECK(readable(s, 2000));
        len = sizeof(from);
        a = accept(s, (struct sockaddr *)&from, &len);
        CHECK(a >= 0 && len == sizeof(from) && from.sin_port == sin.sin_port &&
            from.sin_addr.s_addr == htonl(LOOPBACK));
        CHECK(name_of(a, 0, &from) == 0 && ntohs(from.sin_port) == PORT);

        /* Bytes both ways; peeking; and nothing waiting. */
        CHECK(write(c, "hello, ", 7) == 7);
        CHECK(send(c, "world", 5, 0) == 5);
        CHECK(readable(a, 2000));
        n = (int)recv(a, buf, 3, MSG_PEEK);
        CHECK(n == 3 && memcmp(buf, "hel", 3) == 0);
        n = (int)read_all(a, (unsigned char *)buf, 12);
        CHECK(n == 12 && memcmp(buf, "hello, world", 12) == 0);
        CHECK(recv(a, buf, sizeof(buf), MSG_DONTWAIT) == -1 &&
            errno == EAGAIN);
        CHECK(write(a, "back", 4) == 4);
        CHECK(read_all(c, (unsigned char *)buf, 4) == 4 &&
            memcmp(buf, "back", 4) == 0);
        close(c);
        CHECK(read(a, buf, sizeof(buf)) == 0);  /* its FIN */
        close(a);

        test_bulk(s);

        /*
         * A reader that does not read: the windows close and the writer
         * is held off (EAGAIN); then all of it comes, in order, as the
         * window opens again.
         */
        c = tcp_socket();
        CHECK(connect_to(c, LOOPBACK, PORT) == 0);
        a = accept(s, NULL, NULL);
        CHECK(a >= 0);
        {
                size_t sent = 0, rcvd = 0;
                int stuck = 0;

                for (int i = 0; i < 1000 && sent < BULK; i++) {
                        size_t want = BULK - sent > 4096 ? 4096 : BULK - sent;
                        ssize_t w = send(c, bulk + sent, want, MSG_DONTWAIT);

                        if (w < 0) {
                                stuck = errno == EAGAIN;
                                break;
                        }
                        sent += (size_t)w;
                }
                CHECK(stuck && sent > 65535 && sent < BULK);
                shutdown(c, SHUT_WR);
                for (;;) {
                        ssize_t r = read(a, got + rcvd, BULK - rcvd);

                        if (r <= 0)
                                break;
                        rcvd += (size_t)r;
                }
                CHECK(rcvd == sent && memcmp(got, bulk, sent) == 0);
        }
        close(a);
        close(c);

        /*
         * A full backlog drops a SYN, and the connect gets through on the
         * SYN it sends again a second later, once there is room.
         */
        {
                int l = tcp_socket(), c1 = tcp_socket(), status;
                struct timespec t0, t1;
                pid_t pid;

                CHECK(bind_to(l, LOOPBACK, PORT + 2) == 0 && listen(l, 1) == 0);
                CHECK(connect_to(c1, LOOPBACK, PORT + 2) == 0);
                clock_gettime(CLOCK_MONOTONIC, &t0);
                if ((pid = fork()) == 0) {
                        int c2 = tcp_socket();

                        _exit(connect_to(c2, LOOPBACK, PORT + 2) == 0 &&
                            write(c2, "late", 4) == 4 ? 0 : 1);
                }
                CHECK(pid > 0);
                usleep(300000);
                a = accept(l, NULL, NULL);      /* c1's: room for c2 */
                CHECK(a >= 0);
                close(a);
                c = accept(l, NULL, NULL);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                CHECK(c >= 0 && read_all(c, (unsigned char *)buf, 4) == 4 &&
                    memcmp(buf, "late", 4) == 0);
                CHECK(t1.tv_sec - t0.tv_sec >= 1 || (t1.tv_sec - t0.tv_sec == 0 &&
                    t1.tv_nsec - t0.tv_nsec >= 800000000));
                CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0);
                close(c);
                close(c1);
                close(l);
        }

        /* Nobody at the port: refused. */
        c = tcp_socket();
        CHECK(connect_to(c, LOOPBACK, PORT + 1) == -1 &&
            errno == ECONNREFUSED);
        close(c);

        /* Data closed on unread: a reset. */
        c = tcp_socket();
        CHECK(connect_to(c, LOOPBACK, PORT) == 0);
        a = accept(s, NULL, NULL);
        CHECK(a >= 0);
        CHECK(write(c, "unread", 6) == 6);
        CHECK(readable(a, 2000));
        close(a);
        CHECK(readable(c, 2000));
        CHECK(read(c, buf, sizeof(buf)) == -1 && errno == ECONNRESET);
        CHECK(read(c, buf, sizeof(buf)) == 0);
        close(c);

        close(s);
        CHECK(ioctl(ctl, I_PUNLINK, MUXID_ALL) == 0);
        close(ctl);
        printf("tcptest: %d failures\n", failures);
        return failures;
}
