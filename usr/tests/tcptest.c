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
 * connection, and a reset for data closed on unread; then the options
 * (by TPI and by getsockopt and setsockopt), SO_REUSEADDR, SO_RCVBUF,
 * keep-alives, an abortive close, connecting without waiting, and
 * poll. Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <sys/ipconf.h>
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

/* T_OPTMGMT_REQ of one int option (val NULL: none); the reply's length. */
static int
optmgmt(int fd, t_scalar_t mgmt, int level, int name, const int *val,
    unsigned char *ack, int size)
{
        struct {
                struct T_optmgmt_req req;
                struct t_opthdr oh;
                int v;
        } m;
        size_t vl = val != NULL ? sizeof(*val) : 0;
        struct strbuf ctl;
        int flags = 0;

        memset(&m, 0, sizeof(m));
        m.req.PRIM_type = T_OPTMGMT_REQ;
        m.req.OPT_offset = sizeof(m.req);
        m.req.OPT_length = (t_scalar_t)(sizeof(m.oh) + vl);
        m.req.MGMT_flags = mgmt;
        m.oh.len = (t_uscalar_t)(sizeof(m.oh) + vl);
        m.oh.level = (t_uscalar_t)level;
        m.oh.name = (t_uscalar_t)name;
        if (val != NULL)
                m.v = *val;
        ctl.len = (int)(sizeof(m.req) + sizeof(m.oh) + vl);
        ctl.buf = (char *)&m;
        if (putmsg(fd, &ctl, NULL, 0) != 0)
                return -1;
        ctl.maxlen = size;
        ctl.buf = (char *)ack;
        return getmsg(fd, &ctl, NULL, &flags) == 0 && flags == RS_HIPRI ?
            ctl.len : -1;
}

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

        /* Options: one set, one TCP has not, and a flag it does not do. */
        {
                unsigned char ack[128];
                struct T_optmgmt_ack oa;
                struct t_opthdr oh;
                int one = 1, v = 0, n;

                n = optmgmt(fd, T_NEGOTIATE, SOL_SOCKET, SO_KEEPALIVE, &one,
                    ack, sizeof(ack));
                memcpy(&oa, ack, sizeof(oa));
                memcpy(&oh, ack + sizeof(oa), sizeof(oh));
                memcpy(&v, ack + sizeof(oa) + sizeof(oh), sizeof(v));
                CHECK(n == (int)(sizeof(oa) + sizeof(oh) + sizeof(v)) &&
                    oa.PRIM_type == T_OPTMGMT_ACK &&
                    oa.OPT_offset == sizeof(oa) &&
                    oa.MGMT_flags == T_SUCCESS && oh.status == T_SUCCESS &&
                    oh.level == SOL_SOCKET && oh.name == SO_KEEPALIVE &&
                    oh.len == sizeof(oh) + sizeof(v) && v == 1);
                n = optmgmt(fd, T_CURRENT, IPPROTO_TCP, 999, NULL, ack,
                    sizeof(ack));
                memcpy(&oa, ack, sizeof(oa));
                memcpy(&oh, ack + sizeof(oa), sizeof(oh));
                CHECK(n == (int)(sizeof(oa) + sizeof(oh)) &&
                    oa.PRIM_type == T_OPTMGMT_ACK &&
                    oa.MGMT_flags == T_NOTSUPPORT &&
                    oh.status == T_NOTSUPPORT);
                n = optmgmt(fd, T_CHECK, SOL_SOCKET, SO_KEEPALIVE, &one, ack,
                    sizeof(ack));
                memcpy(&pr, ack, sizeof(pr.error_ack));
                CHECK(n == (int)sizeof(pr.error_ack) &&
                    pr.type == T_ERROR_ACK &&
                    pr.error_ack.ERROR_prim == T_OPTMGMT_REQ &&
                    pr.error_ack.TLI_error == TBADFLAG);
        }
        close(fd);
}

static int
getint(int fd, int level, int name, int *v)
{
        socklen_t len = sizeof(*v);

        *v = -1;
        return getsockopt(fd, level, name, v, &len) == 0 &&
            len == sizeof(*v) ? 0 : -1;
}

static int
setint(int fd, int level, int name, int v)
{
        return setsockopt(fd, level, name, &v, sizeof(v));
}

/* The packets lo0 has sent. */
static uint64_t
lo_opackets(void)
{
        struct ip_ifinfo ifs[8];
        struct strioctl sio = { IP_IFLIST, 0, sizeof(ifs), (char *)ifs };
        int fd = open("/dev/ip", O_RDWR), n;
        uint64_t out = 0;

        if (fd < 0)
                return 0;
        n = ioctl(fd, I_STR, &sio);
        for (int i = 0; i < n && i < 8; i++)
                if (strcmp(ifs[i].ii_name, "lo0") == 0)
                        out = ifs[i].ii_opackets;
        close(fd);
        return out;
}

/* A connection to the listener s: the connecting end and the accepted. */
static void
pair_on(int s, uint16_t port, int *c, int *a)
{
        *c = tcp_socket();
        CHECK(*c >= 0 && connect_to(*c, LOOPBACK, port) == 0);
        *a = accept(s, NULL, NULL);
        CHECK(*a >= 0);
}

/*
 * What fd can send before its peer, not reading, holds it off: until
 * EAGAIN twice running, 200 ms apart (the loopback catches up).
 */
static size_t
fill(int fd)
{
        size_t sent = 0;
        int waited = 0;

        while (sent < BULK) {
                ssize_t w = send(fd, bulk + sent, 4096, MSG_DONTWAIT);

                if (w > 0) {
                        sent += (size_t)w;
                        waited = 0;
                } else if (w < 0 && errno == EAGAIN && !waited) {
                        usleep(200000);
                        waited = 1;
                } else {
                        break;
                }
        }
        return sent;
}

/* getsockopt and setsockopt: the socket's own, SOL_SOCKET's and TCP's. */
static void
test_options(int s)
{
        struct linger lg = { 1, 5 }, lgot;
        int c, a, v, p[2], sp[2];
        socklen_t len;

        c = tcp_socket();
        CHECK(getint(c, SOL_SOCKET, SO_TYPE, &v) == 0 && v == SOCK_STREAM);
        CHECK(getint(c, SOL_SOCKET, SO_ACCEPTCONN, &v) == 0 && v == 0);
        CHECK(getint(s, SOL_SOCKET, SO_ACCEPTCONN, &v) == 0 && v == 1);
        CHECK(getint(c, SOL_SOCKET, SO_ERROR, &v) == 0 && v == 0);
        CHECK(setint(c, SOL_SOCKET, SO_TYPE, 1) == -1 &&
            errno == ENOPROTOOPT);

        CHECK(getint(c, SOL_SOCKET, SO_REUSEADDR, &v) == 0 && v == 0);
        CHECK(setint(c, SOL_SOCKET, SO_REUSEADDR, 7) == 0);
        CHECK(getint(c, SOL_SOCKET, SO_REUSEADDR, &v) == 0 && v == 1);
        CHECK(getint(c, SOL_SOCKET, SO_KEEPALIVE, &v) == 0 && v == 0);
        CHECK(setint(c, SOL_SOCKET, SO_KEEPALIVE, 1) == 0);
        CHECK(getint(c, SOL_SOCKET, SO_KEEPALIVE, &v) == 0 && v == 1);
        CHECK(getint(c, SOL_SOCKET, SO_RCVBUF, &v) == 0 && v == 131072);
        CHECK(setint(c, SOL_SOCKET, SO_RCVBUF, 1 << 20) == 0);
        CHECK(getint(c, SOL_SOCKET, SO_RCVBUF, &v) == 0 && v == 262144);
        CHECK(setint(c, SOL_SOCKET, SO_RCVBUF, 1) == 0);
        CHECK(getint(c, SOL_SOCKET, SO_RCVBUF, &v) == 0 && v == 2048);
        CHECK(setint(c, SOL_SOCKET, SO_SNDBUF, 8192) == 0);
        CHECK(getint(c, SOL_SOCKET, SO_SNDBUF, &v) == 0 && v == 8192);

        len = sizeof(lgot);
        CHECK(getsockopt(c, SOL_SOCKET, SO_LINGER, &lgot, &len) == 0 &&
            len == sizeof(lgot) && lgot.l_onoff == 0);
        CHECK(setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
        CHECK(setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, 4) == -1 &&
            errno == EINVAL);
        len = sizeof(lgot);
        CHECK(getsockopt(c, SOL_SOCKET, SO_LINGER, &lgot, &len) == 0 &&
            len == sizeof(lgot) && lgot.l_onoff == 1 && lgot.l_linger == 5);

        CHECK(setint(c, IPPROTO_TCP, TCP_NODELAY, 1) == 0);
        CHECK(getint(c, IPPROTO_TCP, TCP_NODELAY, &v) == 0 && v == 1);
        CHECK(getint(c, IPPROTO_TCP, TCP_MAXSEG, &v) == 0 && v == 536);
        CHECK(setint(c, IPPROTO_TCP, TCP_MAXSEG, 1000) == -1 &&
            errno == ENOPROTOOPT);
        CHECK(setint(c, IPPROTO_TCP, TCP_KEEPIDLE, 0) == -1 &&
            errno == EINVAL);
        CHECK(setint(c, IPPROTO_TCP, TCP_KEEPIDLE, 30) == 0);
        CHECK(getint(c, IPPROTO_TCP, TCP_KEEPIDLE, &v) == 0 && v == 30);
        CHECK(getint(c, IPPROTO_TCP, TCP_KEEPCNT, &v) == 0 && v == 8);

        /* Unknown, short, cut short, and not a socket at all. */
        CHECK(setint(c, SOL_SOCKET, 12345, 1) == -1 && errno == ENOPROTOOPT);
        CHECK(getint(c, 99, 1, &v) == -1 && errno == ENOPROTOOPT);
        v = 1;
        CHECK(setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, 2) == -1 &&
            errno == EINVAL);
        len = 2;
        v = 0;
        CHECK(getsockopt(c, SOL_SOCKET, SO_RCVBUF, &v, &len) == 0 &&
            len == 2 && v == 2048);
        CHECK(pipe(p) == 0);
        CHECK(getint(p[0], SOL_SOCKET, SO_TYPE, &v) == -1 &&
            errno == ENOTSOCK);
        CHECK(setint(p[0], SOL_SOCKET, SO_KEEPALIVE, 1) == -1 &&
            errno == ENOTSOCK);
        close(p[0]);
        close(p[1]);
        close(c);

        /* A local socket says what it is, and takes nothing. */
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
        CHECK(getint(sp[0], SOL_SOCKET, SO_TYPE, &v) == 0 &&
            v == SOCK_STREAM);
        CHECK(getint(sp[0], SOL_SOCKET, SO_SNDBUF, &v) == 0 && v > 0);
        CHECK(setint(sp[0], SOL_SOCKET, SO_KEEPALIVE, 1) == -1 &&
            errno == ENOPROTOOPT);
        close(sp[0]);
        close(sp[1]);

        /* An accepted connection has its listener's; MSS is the peer's. */
        CHECK(setint(s, SOL_SOCKET, SO_KEEPALIVE, 1) == 0);
        pair_on(s, PORT, &c, &a);
        CHECK(getint(a, SOL_SOCKET, SO_KEEPALIVE, &v) == 0 && v == 1);
        CHECK(getint(c, SOL_SOCKET, SO_KEEPALIVE, &v) == 0 && v == 0);
        CHECK(getint(c, IPPROTO_TCP, TCP_MAXSEG, &v) == 0 && v == 1460);
        CHECK(setint(s, SOL_SOCKET, SO_KEEPALIVE, 0) == 0);
        close(a);
        close(c);
}

/*
 * SO_REUSEADDR: a port its connections still hold (the listener gone)
 * is taken only with it, and never while another listens there.
 */
static void
test_reuse(void)
{
        int l = tcp_socket(), l2, l3, c, a, v;

        CHECK(bind_to(l, LOOPBACK, PORT + 3) == 0 && listen(l, 1) == 0);
        pair_on(l, PORT + 3, &c, &a);
        close(l);
        l2 = tcp_socket();
        CHECK(bind_to(l2, LOOPBACK, PORT + 3) == -1 && errno == EADDRINUSE);
        CHECK(setint(l2, SOL_SOCKET, SO_REUSEADDR, 1) == 0);
        CHECK(bind_to(l2, LOOPBACK, PORT + 3) == 0 && listen(l2, 1) == 0);
        l3 = tcp_socket();
        CHECK(setint(l3, SOL_SOCKET, SO_REUSEADDR, 1) == 0);
        CHECK(bind_to(l3, 0, PORT + 3) == -1 && errno == EADDRINUSE);
        CHECK(getint(l2, SOL_SOCKET, SO_REUSEADDR, &v) == 0 && v == 1);
        close(l3);
        close(l2);
        close(a);
        close(c);
}

/* SO_RCVBUF: a smaller window, so a writer is held off sooner. */
static void
test_rcvbuf(int s)
{
        int l = tcp_socket(), c, a, c2, a2, v;
        size_t small, full;

        CHECK(setint(l, SOL_SOCKET, SO_RCVBUF, 4096) == 0);
        CHECK(bind_to(l, LOOPBACK, PORT + 4) == 0 && listen(l, 1) == 0);
        pair_on(l, PORT + 4, &c, &a);
        CHECK(getint(a, SOL_SOCKET, SO_RCVBUF, &v) == 0 && v == 4096);
        pair_on(s, PORT, &c2, &a2);
        small = fill(c);
        full = fill(c2);
        CHECK(small > 0 && small + 32768 < full);
        close(a);
        close(c);
        close(a2);
        close(c2);
        close(l);
}

/*
 * Keep-alives on an idle connection: probes (and their answers) on the
 * loopback, and the connection, which answers them, lives on.
 */
static void
test_keepalive(int s)
{
        uint64_t before, after;
        char buf[8];
        int c, a;

        pair_on(s, PORT, &c, &a);
        CHECK(setint(c, IPPROTO_TCP, TCP_KEEPIDLE, 1) == 0 &&
            setint(c, IPPROTO_TCP, TCP_KEEPINTVL, 1) == 0 &&
            setint(c, IPPROTO_TCP, TCP_KEEPCNT, 2) == 0 &&
            setint(c, SOL_SOCKET, SO_KEEPALIVE, 1) == 0);
        before = lo_opackets();
        usleep(3500000);
        after = lo_opackets();
        CHECK(after >= before + 4);     /* three probes, three answers */
        CHECK(write(c, "alive", 5) == 5 && readable(a, 2000) &&
            read(a, buf, sizeof(buf)) == 5);
        close(a);
        close(c);
}

/* SO_LINGER of no time: close resets the connection. */
static void
test_linger(int s)
{
        struct linger lg = { 1, 0 };
        char buf[8];
        int c, a;

        pair_on(s, PORT, &c, &a);
        CHECK(setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
        close(c);
        CHECK(readable(a, 2000));
        CHECK(read(a, buf, sizeof(buf)) == -1 && errno == ECONNRESET);
        close(a);
}

/*
 * Connecting without waiting: EINPROGRESS (or done at once), writable
 * when made, and how it went from SO_ERROR or connecting again.
 */
static void
test_nonblock(int s)
{
        struct pollfd p;
        char buf[8];
        int c, a, r, v;

        c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        r = connect_to(c, LOOPBACK, PORT);
        CHECK(r == 0 || (r == -1 && errno == EINPROGRESS));
        p = (struct pollfd){ c, POLLOUT, 0 };
        CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT) != 0 &&
            (p.revents & (POLLERR | POLLHUP)) == 0);
        CHECK(getint(c, SOL_SOCKET, SO_ERROR, &v) == 0 && v == 0);
        CHECK(connect_to(c, LOOPBACK, PORT) == -1 && errno == EISCONN);
        a = accept(s, NULL, NULL);
        CHECK(a >= 0 && write(c, "nb", 2) == 2 && readable(a, 2000) &&
            read(a, buf, sizeof(buf)) == 2);
        close(a);
        close(c);

        /* Refused: SO_ERROR says so, once. */
        c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        r = connect_to(c, LOOPBACK, PORT + 1);
        CHECK(r == -1 && (errno == EINPROGRESS || errno == ECONNREFUSED));
        if (r == -1 && errno == EINPROGRESS) {
                p = (struct pollfd){ c, POLLOUT, 0 };
                CHECK(poll(&p, 1, 2000) == 1 &&
                    (p.revents & (POLLOUT | POLLERR)) == (POLLOUT | POLLERR));
                CHECK(getint(c, SOL_SOCKET, SO_ERROR, &v) == 0 &&
                    v == ECONNREFUSED);
        }
        CHECK(getint(c, SOL_SOCKET, SO_ERROR, &v) == 0 && v == 0);
        close(c);

        /* Refused, and connecting again says so. */
        c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        r = connect_to(c, LOOPBACK, PORT + 1);
        if (r == -1 && errno == EINPROGRESS) {
                p = (struct pollfd){ c, POLLOUT, 0 };
                CHECK(poll(&p, 1, 2000) == 1);
                CHECK(connect_to(c, LOOPBACK, PORT + 1) == -1 &&
                    errno == ECONNREFUSED);
        } else {
                CHECK(r == -1 && errno == ECONNREFUSED);
        }
        CHECK(getint(c, SOL_SOCKET, SO_ERROR, &v) == 0 && v == 0);
        close(c);
}

/*
 * MSG_WAITALL: a read waits for all it asked for, sent in two parts
 * 100 ms apart; at the end of the peer's data it gives what there was.
 */
static void
test_waitall(int s)
{
        char buf[8];
        int c, a, status = -1;
        pid_t pid;

        pair_on(s, PORT, &c, &a);
        CHECK(write(c, "ab", 2) == 2);
        if ((pid = fork()) == 0) {
                usleep(100000);
                _exit(write(c, "cd", 2) == 2 ? 0 : 1);
        }
        CHECK(recv(a, buf, 4, MSG_WAITALL) == 4 &&
            memcmp(buf, "abcd", 4) == 0);
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(write(c, "e", 1) == 1 && shutdown(c, SHUT_WR) == 0);
        CHECK(recv(a, buf, 4, MSG_WAITALL) == 1 && buf[0] == 'e');
        close(a);
        close(c);
}

/* What poll says of stream sockets in their states. */
static void
test_poll(int s)
{
        struct pollfd p;
        char buf[8];
        int c, a;

        c = tcp_socket();
        p = (struct pollfd){ c, POLLIN | POLLOUT, 0 };
        CHECK(poll(&p, 1, 0) == 1 && p.revents == (POLLOUT | POLLHUP));
        close(c);

        p = (struct pollfd){ s, POLLOUT, 0 };
        CHECK(poll(&p, 1, 0) == 0);     /* a listener is not for writing */
        pair_on(s, PORT, &c, &a);
        p = (struct pollfd){ c, POLLIN | POLLOUT, 0 };
        CHECK(poll(&p, 1, 0) == 1 && p.revents == POLLOUT);
        CHECK(shutdown(c, SHUT_WR) == 0);
        p = (struct pollfd){ a, POLLIN, 0 };
        CHECK(poll(&p, 1, 2000) == 1 && p.revents == POLLIN);
        CHECK(read(a, buf, sizeof(buf)) == 0);
        CHECK(poll(&p, 1, 0) == 1 && p.revents == POLLIN);     /* still */
        close(a);
        p = (struct pollfd){ c, POLLIN, 0 };
        CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN) != 0 &&
            (p.revents & POLLHUP) != 0);
        CHECK(read(c, buf, sizeof(buf)) == 0);
        close(c);
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

        test_options(s);
        test_reuse();
        test_rcvbuf(s);
        test_keepalive(s);
        test_linger(s);
        test_nonblock(s);
        test_waitall(s);
        test_poll(s);

        close(s);
        CHECK(ioctl(ctl, I_PUNLINK, MUXID_ALL) == 0);
        close(ctl);
        printf("tcptest: %d failures\n", failures);
        return failures;
}
