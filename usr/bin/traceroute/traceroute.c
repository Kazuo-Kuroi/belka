/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * traceroute: the routers on the way to a host, found by sending probes
 * whose TTL rises one at a time from first_ttl, and hearing who says the
 * TTL ran out (ICMP time exceeded) and, at the end, the host itself:
 * port unreachable for a UDP probe to an unlikely port, or an echo reply
 * for an ICMP echo request (-I).
 *
 * The probes go out on a raw IPPROTO_RAW socket, which takes our own
 * header, so each probe has its TTL; the answers come in on a raw ICMP
 * socket, and only those quoting one of our probes count. Both need the
 * superuser, so traceroute is set-user-ID root, and gives that up as
 * soon as both are open.
 */

#define _DEFAULT_SOURCE         /* getnameinfo's NI_MAXHOST, inet_ntoa */

#include <sys/socket.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IPHDR           20
#define UDPHDR          8
#define ICMPHDR         8
#define ICMP_ECHOREPLY  0
#define ICMP_UNREACH    3
#define ICMP_ECHO       8
#define ICMP_TIMXCEED   11
#define UNREACH_PORT    3

static const char *prog = "traceroute";
static int nflag, iflag;
static unsigned short ident;
static uint16_t baseport = 33434;
static int tos;

/* The probe we sent last, to match answers against. */
struct probe {
        int             ttl;
        uint16_t        seq;
        struct timespec sent;
};

static void
usage(void)
{
        fprintf(stderr, "usage: %s [-dIn] [-f first_ttl] [-m max_ttl] "
            "[-p port] [-q nqueries]\n           [-s src_addr] [-t tos] "
            "[-w waittime] host [packetlen]\n", prog);
        exit(2);
}

static void
fail(const char *what)
{
        fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(errno));
        exit(1);
}

static long
number(const char *s, long lo, long hi, const char *what)
{
        char *end;
        long v;

        errno = 0;
        v = strtol(s, &end, 10);
        if (*s == '\0' || *end != '\0' || errno != 0 || v < lo || v > hi) {
                fprintf(stderr, "%s: %s: %s must be %ld to %ld\n", prog, s,
                    what, lo, hi);
                exit(2);
        }
        return v;
}

static uint16_t
cksum(const uint8_t *p, size_t len)
{
        uint32_t sum = 0;

        for (; len > 1; p += 2, len -= 2)
                sum += (uint32_t)(p[0] << 8 | p[1]);
        if (len == 1)
                sum += (uint32_t)(p[0] << 8);
        while (sum >> 16)
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

static double
ms_since(const struct timespec *t0)
{
        struct timespec t1;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        return (double)(t1.tv_sec - t0->tv_sec) * 1e3 +
            (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

/*
 * One probe of len bytes in all, to dst with this TTL: a UDP datagram
 * to baseport + seq, or an ICMP echo request of sequence seq. IP fills
 * in the length, identification, header checksum, the source (if src is
 * 0.0.0.0) and UDP's checksum.
 */
static void
send_probe(int s, const struct sockaddr_in *dst, in_addr_t src, int ttl,
    uint16_t seq, size_t len)
{
        uint8_t pkt[1500];

        memset(pkt, 0, len);
        pkt[0] = 0x45;
        pkt[1] = (uint8_t)tos;
        pkt[8] = (uint8_t)ttl;
        pkt[9] = iflag ? IPPROTO_ICMP : IPPROTO_UDP;
        memcpy(pkt + 12, &src, 4);
        memcpy(pkt + 16, &dst->sin_addr.s_addr, 4);
        if (iflag) {
                pkt[IPHDR] = ICMP_ECHO;
                put16(pkt + IPHDR + 4, ident);
                put16(pkt + IPHDR + 6, seq);
                pkt[IPHDR + 8] = (uint8_t)ttl;
                put16(pkt + IPHDR + 2, cksum(pkt + IPHDR, len - IPHDR));
        } else {
                put16(pkt + IPHDR, ident);
                put16(pkt + IPHDR + 2, (uint16_t)(baseport + seq));
                put16(pkt + IPHDR + 4, (uint16_t)(len - IPHDR));
                pkt[IPHDR + UDPHDR] = (uint8_t)ttl;
        }
        if (sendto(s, pkt, len, 0, (const struct sockaddr *)dst,
            sizeof(*dst)) != (ssize_t)len)
                fail("sendto");
}

/*
 * Whether the ICMP message in pkt (n bytes, IP header first) answers
 * probe p to dst: its type and code in *type and *code, who sent it in
 * *from.
 */
static int
answers(const uint8_t *pkt, size_t n, const struct sockaddr_in *dst,
    const struct probe *p, int *type, int *code, in_addr_t *from)
{
        size_t hl, ihl;
        const uint8_t *icmp, *in;

        if (n < IPHDR || (pkt[0] >> 4) != 4 || pkt[9] != IPPROTO_ICMP)
                return 0;
        hl = (size_t)(pkt[0] & 0x0f) * 4;
        if (hl < IPHDR || n < hl + ICMPHDR)
                return 0;
        icmp = pkt + hl;
        *type = icmp[0];
        *code = icmp[1];
        memcpy(from, pkt + 12, 4);
        if (iflag && *type == ICMP_ECHOREPLY)
                return get16(icmp + 4) == ident && get16(icmp + 6) == p->seq;
        if (*type != ICMP_TIMXCEED && *type != ICMP_UNREACH)
                return 0;
        /* The probe it quotes: its header and at least 8 bytes on. */
        in = icmp + ICMPHDR;
        if (n < hl + ICMPHDR + IPHDR)
                return 0;
        ihl = (size_t)(in[0] & 0x0f) * 4;
        if (ihl < IPHDR || n < hl + ICMPHDR + ihl + 8 ||
            memcmp(in + 16, &dst->sin_addr.s_addr, 4) != 0)
                return 0;
        if (iflag)
                return in[9] == IPPROTO_ICMP && in[ihl] == ICMP_ECHO &&
                    get16(in + ihl + 4) == ident &&
                    get16(in + ihl + 6) == p->seq;
        return in[9] == IPPROTO_UDP && get16(in + ihl) == ident &&
            get16(in + ihl + 2) == (uint16_t)(baseport + p->seq);
}

static void
print_host(in_addr_t a)
{
        struct sockaddr_in sin;
        char name[NI_MAXHOST];
        struct in_addr in;

        in.s_addr = a;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr = in;
        if (!nflag && getnameinfo((struct sockaddr *)&sin, sizeof(sin), name,
            sizeof(name), NULL, 0, NI_NAMEREQD) == 0)
                printf(" %s (%s)", name, inet_ntoa(in));
        else
                printf(" %s", inet_ntoa(in));
}

/* An unreachable's mark, as traceroute has always printed them. */
static const char *
unreach_mark(int code)
{
        switch (code) {
        case 0:
                return " !N";
        case 1:
                return " !H";
        case 2:
                return " !P";
        case 4:
                return " !F";
        case 5:
                return " !S";
        case 13:
                return " !X";
        default:
                return " !U";
        }
}

int
main(int argc, char **argv)
{
        int first = 1, max = 30, nq = 3, waitsec = 5, c, rs, ss;
        size_t len = 0;
        struct addrinfo hints, *ai;
        struct sockaddr_in dst;
        in_addr_t src = INADDR_ANY;
        uint16_t seq = 0;
        int e;

        if ((rs = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1 ||
            (ss = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) == -1)
                fail("socket");
        if (setgid(getgid()) != 0 || setuid(getuid()) != 0)
                fail("setuid");

        while ((c = getopt(argc, argv, "dIf:m:np:q:s:t:w:")) != -1) {
                switch (c) {
                case 'd':
                        break;          /* SO_DEBUG: nothing here */
                case 'I':
                        iflag = 1;
                        break;
                case 'f':
                        first = (int)number(optarg, 1, 255, "first TTL");
                        break;
                case 'm':
                        max = (int)number(optarg, 1, 255, "maximum TTL");
                        break;
                case 'n':
                        nflag = 1;
                        break;
                case 'p':
                        baseport = (uint16_t)number(optarg, 1, 65535, "port");
                        break;
                case 'q':
                        nq = (int)number(optarg, 1, 10, "queries");
                        break;
                case 's':
                        if (inet_pton(AF_INET, optarg, &src) != 1) {
                                fprintf(stderr, "%s: %s: not an address\n",
                                    prog, optarg);
                                exit(2);
                        }
                        break;
                case 't':
                        tos = (int)number(optarg, 0, 255, "type of service");
                        break;
                case 'w':
                        waitsec = (int)number(optarg, 1, 86400, "wait");
                        break;
                default:
                        usage();
                }
        }
        argc -= optind;
        argv += optind;
        if (argc < 1 || argc > 2)
                usage();
        if (first > max) {
                fprintf(stderr, "%s: first TTL %d is past the maximum %d\n",
                    prog, first, max);
                exit(2);
        }
        len = IPHDR + (iflag ? ICMPHDR : UDPHDR) + 32;
        if (argc == 2)
                len = (size_t)number(argv[1], (long)(IPHDR + (iflag ?
                    ICMPHDR : UDPHDR) + 1), 1500, "packet length");

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        if ((e = getaddrinfo(argv[0], NULL, &hints, &ai)) != 0) {
                fprintf(stderr, "%s: %s: %s\n", prog, argv[0],
                    gai_strerror(e));
                exit(1);
        }
        memcpy(&dst, ai->ai_addr, sizeof(dst));
        freeaddrinfo(ai);
        ident = (unsigned short)((getpid() & 0xffff) | 0x8000);

        fprintf(stderr, "%s to %s (%s), %d hops max, %zu byte packets\n",
            prog, argv[0], inet_ntoa(dst.sin_addr), max, len);
        for (int ttl = first; ttl <= max; ttl++) {
                in_addr_t last = INADDR_ANY;
                int got = 0, done = 0, unreach = 0;

                printf("%2d ", ttl);
                for (int q = 0; q < nq; q++) {
                        struct probe p = { ttl, ++seq, { 0, 0 } };
                        int answered = 0;

                        clock_gettime(CLOCK_MONOTONIC, &p.sent);
                        send_probe(ss, &dst, src, ttl, p.seq, len);
                        while (!answered) {
                                double left = waitsec * 1e3 - ms_since(&p.sent);
                                struct pollfd pf = { rs, POLLIN, 0 };
                                uint8_t buf[1500];
                                int type, code;
                                in_addr_t from;
                                ssize_t n;

                                if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
                                        break;
                                if ((n = recv(rs, buf, sizeof(buf), 0)) <= 0)
                                        continue;
                                if (!answers(buf, (size_t)n, &dst, &p, &type,
                                    &code, &from))
                                        continue;
                                answered = 1;
                                got++;
                                if (from != last) {
                                        print_host(from);
                                        last = from;
                                }
                                printf("  %.3f ms", ms_since(&p.sent));
                                if (type == ICMP_ECHOREPLY ||
                                    (type == ICMP_UNREACH &&
                                    code == UNREACH_PORT)) {
                                        done = 1;
                                } else if (type == ICMP_UNREACH) {
                                        printf("%s", unreach_mark(code));
                                        unreach++;
                                }
                        }
                        if (!answered)
                                printf(" *");
                        fflush(stdout);
                }
                printf("\n");
                if (done || (got > 0 && unreach == got))
                        break;
        }
        return 0;
}
