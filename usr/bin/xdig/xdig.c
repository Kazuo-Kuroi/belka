/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * xdig (eXtended dig): what DNS and WHOIS say of a domain, all at once.
 *
 *   xdig [-cPW] domain | user@domain | url
 *
 * For the domain: its level, its root domain and the zone it is in; a
 * quick WHOIS (registrar, name servers, expiry); the zone's records
 * (SOA, NS, A, AAAA, MX, TXT, CAA, CNAME, asked for one by one, as ANY
 * is no longer answered); what the public resolvers (Google, Cloudflare,
 * Quad9) have cached, with the zone's serial at each; and where web and
 * mail go, each address with its PTR and its owner from WHOIS. -P leaves
 * out the public resolvers and -W WHOIS (both reach the Internet); -c
 * turns colour off, which is on only for a terminal.
 *
 * Queries go through djbdns' DNS library: to the resolver
 * /etc/resolv.conf names (dnscache), or to a public resolver directly.
 * WHOIS starts at whois.iana.org and follows its referrals.
 */

#define _DEFAULT_SOURCE

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "byte.h"
#include "dns.h"
#include "iopause.h"
#include "stralloc.h"
#include "taia.h"
#include "uint16.h"
#include "uint32.h"

#define VERSION         "2.0"
#define QUERY_SECS      8       /* a DNS query, at most */
#define WHOIS_SECS      8       /* a WHOIS connection, at most */
#define MAXRR           64
#define SECTION "|----------------------------------------------------------------|"

#define T_A     1
#define T_NS    2
#define T_CNAME 5
#define T_SOA   6
#define T_PTR   12
#define T_MX    15
#define T_TXT   16
#define T_AAAA  28
#define T_CAA   257

static const char *prog = "xdig";
static int color;

struct rr {
        char            name[256];
        unsigned        type;
        unsigned long   ttl;
        unsigned        pref;           /* MX */
        char            target[256];    /* NS, CNAME, PTR, MX: the name */
        unsigned char   addr[4];        /* A */
        unsigned long   serial;         /* SOA */
        char            data[1024];     /* as text */
};

struct reply {
        int             ok;             /* an answer came */
        int             rcode;
        int             nan, nns;
        struct rr       an[MAXRR];
        struct rr       ns[MAXRR];
};

static const char *
red(void)
{
        return color ? "\033[31m" : "";
}

static const char *
green(void)
{
        return color ? "\033[32m" : "";
}

static const char *
yellow(void)
{
        return color ? "\033[33m" : "";
}

static const char *
bold(void)
{
        return color ? "\033[1m" : "";
}

static const char *
plain(void)
{
        return color ? "\033[0m" : "";
}

static void
nomem(void)
{
        fprintf(stderr, "%s: out of memory\n", prog);
        exit(111);
}

static const char *
typename(unsigned t)
{
        static char buf[16];

        switch (t) {
        case T_A: return "A";
        case T_NS: return "NS";
        case T_CNAME: return "CNAME";
        case T_SOA: return "SOA";
        case T_PTR: return "PTR";
        case T_MX: return "MX";
        case T_TXT: return "TXT";
        case T_AAAA: return "AAAA";
        case T_CAA: return "CAA";
        }
        snprintf(buf, sizeof(buf), "TYPE%u", t);
        return buf;
}

/* --- DNS. --- */

/* The name at pos in buf, as text with no final dot; 0 if bad. */
static unsigned
getname(const char *buf, unsigned len, unsigned pos, char *out, size_t size)
{
        static stralloc sa;
        char *d = NULL;

        if ((pos = dns_packet_getname(buf, len, pos, &d)) == 0)
                return 0;
        sa.len = 0;
        if (!dns_domain_todot_cat(&sa, d))
                nomem();
        dns_domain_free(&d);
        if (sa.len >= size)
                sa.len = (unsigned)size - 1;
        memcpy(out, sa.s, sa.len);
        out[sa.len] = '\0';
        if (strcmp(out, ".") == 0 || out[0] == '\0')
                strcpy(out, ".");
        return pos;
}

/* A record's data, from buf at pos (len bytes of it), into r. */
static int
decode(const char *buf, unsigned blen, unsigned pos, unsigned len,
    struct rr *r)
{
        unsigned end = pos + len, i;
        char n1[256], n2[256];
        unsigned char b[2];
        uint32 u[5];

        r->data[0] = '\0';
        switch (r->type) {
        case T_A:
                if (len != 4 || !dns_packet_copy(buf, blen, pos,
                    (char *)r->addr, 4))
                        return 0;
                snprintf(r->data, sizeof(r->data), "%u.%u.%u.%u",
                    r->addr[0], r->addr[1], r->addr[2], r->addr[3]);
                return 1;
        case T_NS:
        case T_CNAME:
        case T_PTR:
                if (!getname(buf, blen, pos, r->target, sizeof(r->target)))
                        return 0;
                snprintf(r->data, sizeof(r->data), "%s.", r->target);
                return 1;
        case T_MX:
                if (!(pos = dns_packet_copy(buf, blen, pos, (char *)b, 2)) ||
                    !getname(buf, blen, pos, r->target, sizeof(r->target)))
                        return 0;
                r->pref = (unsigned)(b[0] << 8 | b[1]);
                snprintf(r->data, sizeof(r->data), "%u %s.", r->pref,
                    r->target);
                return 1;
        case T_SOA:
                if (!(pos = getname(buf, blen, pos, n1, sizeof(n1))) ||
                    !(pos = getname(buf, blen, pos, n2, sizeof(n2))))
                        return 0;
                for (i = 0; i < 5; i++) {
                        char w[4];

                        if (!(pos = dns_packet_copy(buf, blen, pos, w, 4)))
                                return 0;
                        uint32_unpack_big(w, &u[i]);
                }
                r->serial = u[0];
                snprintf(r->data, sizeof(r->data), "%s. %s. %lu %lu %lu "
                    "%lu %lu", n1, n2, (unsigned long)u[0],
                    (unsigned long)u[1], (unsigned long)u[2],
                    (unsigned long)u[3], (unsigned long)u[4]);
                return 1;
        case T_AAAA: {
                unsigned char a[16];
                char s[INET6_ADDRSTRLEN];

                if (len != 16 || !dns_packet_copy(buf, blen, pos,
                    (char *)a, 16) || inet_ntop(AF_INET6, a, s,
                    sizeof(s)) == NULL)
                        return 0;
                snprintf(r->data, sizeof(r->data), "%s", s);
                return 1;
        }
        default: {
                size_t o = 0;
                unsigned k = 0, txtlen = 0;
                int in_txt = r->type == T_TXT, caa = r->type == T_CAA;

                for (i = pos; i < end && o + 8 < sizeof(r->data); i++) {
                        unsigned char c;

                        if (!dns_packet_copy(buf, blen, i, (char *)&c, 1))
                                return 0;
                        if (in_txt && k == 0) {
                                /* A string's length: a new quoted string. */
                                if (o > 0)
                                        r->data[o++] = ' ';
                                r->data[o++] = '"';
                                txtlen = c;
                                k = 1;
                                if (txtlen == 0) {
                                        r->data[o++] = '"';
                                        k = 0;
                                }
                                continue;
                        }
                        if (caa && i == pos) {
                                o += (size_t)snprintf(r->data + o,
                                    sizeof(r->data) - o, "%u ", c);
                                continue;
                        }
                        if (caa && i == pos + 1) {
                                txtlen = c;     /* the tag's length */
                                continue;
                        }
                        if (c == '"' || c == '\\') {
                                r->data[o++] = '\\';
                                r->data[o++] = (char)c;
                        } else if (c >= 32 && c < 127) {
                                r->data[o++] = (char)c;
                        } else {
                                o += (size_t)snprintf(r->data + o,
                                    sizeof(r->data) - o, "\\%03o", c);
                        }
                        if (caa && i == pos + 1 + txtlen)
                                r->data[o++] = ' ';
                        if (in_txt && ++k > txtlen) {
                                r->data[o++] = '"';
                                k = 0;
                        }
                }
                if (in_txt && k != 0)
                        r->data[o++] = '"';
                r->data[o] = '\0';
                return 1;
        }
        }
}

/*
 * The records of one section, n of them from pos, into rr (up to max);
 * the position after them, or 0.
 */
static unsigned
section(const char *buf, unsigned len, unsigned pos, unsigned n,
    struct rr *rr, int *nrr, int max)
{
        for (unsigned i = 0; i < n; i++) {
                struct rr r;
                char hdr[10];
                uint16 type, dlen;
                uint32 ttl;

                memset(&r, 0, sizeof(r));
                if (!(pos = getname(buf, len, pos, r.name, sizeof(r.name))) ||
                    !(pos = dns_packet_copy(buf, len, pos, hdr, 10)))
                        return 0;
                uint16_unpack_big(hdr, &type);
                uint32_unpack_big(hdr + 4, &ttl);
                uint16_unpack_big(hdr + 8, &dlen);
                r.type = type;
                r.ttl = ttl;
                if (hdr[2] == 0 && hdr[3] == 1 && decode(buf, len, pos, dlen,
                    &r) && *nrr < max)
                        rr[(*nrr)++] = r;
                pos += dlen;
        }
        return pos;
}

/*
 * A recursive query for name's type to servers (djbdns' 64 bytes: up to
 * 16 addresses), its answer and authority records in *rp. 0 if no answer
 * came within QUERY_SECS.
 */
static int
query(const char *servers, const char *name, unsigned type, struct reply *rp)
{
        static struct dns_transmit tx;
        struct taia stamp, deadline;
        char *q = NULL, qt[2];
        iopause_fd x[1];
        unsigned pos, an, ns;
        char hdr[12];
        int r;

        memset(rp, 0, sizeof(*rp));
        if (!dns_domain_fromdot(&q, name, (unsigned)strlen(name)))
                nomem();
        uint16_pack_big(qt, (uint16)type);
        if (dns_transmit_start(&tx, servers, 1, q, qt, "\0\0\0\0") == -1) {
                dns_domain_free(&q);
                return 0;
        }
        dns_domain_free(&q);
        taia_now(&stamp);
        taia_uint(&deadline, QUERY_SECS);
        taia_add(&deadline, &deadline, &stamp);
        for (;;) {
                struct taia now;

                taia_now(&now);
                if (taia_less(&deadline, &now)) {
                        dns_transmit_free(&tx);
                        return 0;
                }
                dns_transmit_io(&tx, x, &deadline);
                iopause(x, 1, &deadline, &now);
                if ((r = dns_transmit_get(&tx, x, &now)) == -1) {
                        dns_transmit_free(&tx);
                        return 0;
                }
                if (r == 1)
                        break;
        }
        if (!(pos = dns_packet_copy(tx.packet, tx.packetlen, 0, hdr, 12)))
                return 0;
        rp->ok = 1;
        rp->rcode = hdr[3] & 15;
        an = (unsigned)((unsigned char)hdr[6] << 8 | (unsigned char)hdr[7]);
        ns = (unsigned)((unsigned char)hdr[8] << 8 | (unsigned char)hdr[9]);
        if (!(pos = dns_packet_skipname(tx.packet, tx.packetlen, pos)))
                return 1;
        pos += 4;
        if ((pos = section(tx.packet, tx.packetlen, pos, an, rp->an,
            &rp->nan, MAXRR)) != 0)
                (void)section(tx.packet, tx.packetlen, pos, ns, rp->ns,
                    &rp->nns, MAXRR);
        return 1;
}

static char resolver[64];

static void
server_of(const char *ip, char servers[64])
{
        memset(servers, 0, 64);
        (void)inet_pton(AF_INET, ip, servers);
}

/* addr's PTR name, through the resolver, or "" if none. */
static void
ptr_of(const unsigned char addr[4], char *out, size_t size)
{
        char name[64];
        struct reply r;

        out[0] = '\0';
        snprintf(name, sizeof(name), "%u.%u.%u.%u.in-addr.arpa", addr[3],
            addr[2], addr[1], addr[0]);
        if (!query(resolver, name, T_PTR, &r))
                return;
        for (int i = 0; i < r.nan; i++)
                if (r.an[i].type == T_PTR) {
                        snprintf(out, size, "%s", r.an[i].target);
                        return;
                }
}

/* --- WHOIS. --- */

/*
 * What server says of what (RFC 3912), into out; 0 if it could not be
 * asked. Connecting and reading are bounded by WHOIS_SECS.
 */
static int
whois(const char *server, const char *what, char *out, size_t size)
{
        struct addrinfo hints, *ai, *p;
        size_t got = 0;
        time_t end = time(NULL) + WHOIS_SECS;
        int fd = -1;

        out[0] = '\0';
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(server, "43", &hints, &ai) != 0)
                return 0;
        for (p = ai; p != NULL && fd == -1; p = p->ai_next) {
                struct pollfd pf;
                int err = 0;
                socklen_t el = sizeof(err);

                if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK,
                    0)) == -1)
                        continue;
                if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
                        break;
                pf.fd = fd;
                pf.events = POLLOUT;
                if (errno == EINPROGRESS && poll(&pf, 1,
                    (int)(end - time(NULL)) * 1000) == 1 &&
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
                    err == 0)
                        break;
                close(fd);
                fd = -1;
        }
        freeaddrinfo(ai);
        if (fd == -1)
                return 0;
        {
                char q[512];
                int n = snprintf(q, sizeof(q), "%s\r\n", what);

                if (n <= 0 || (size_t)n >= sizeof(q) || write(fd, q,
                    (size_t)n) != n) {
                        close(fd);
                        return 0;
                }
        }
        while (got + 1 < size) {
                struct pollfd pf = { fd, POLLIN, 0 };
                long left = (long)(end - time(NULL));
                ssize_t n;

                if (left <= 0 || poll(&pf, 1, (int)left * 1000) != 1)
                        break;
                if ((n = read(fd, out + got, size - 1 - got)) <= 0)
                        break;
                got += (size_t)n;
        }
        out[got] = '\0';
        close(fd);
        return got > 0;
}

/* The value of the first line of text whose field is one of names. */
static int
field(const char *text, const char *const *names, char *out, size_t size)
{
        for (const char *l = text; *l != '\0'; ) {
                const char *e = strchr(l, '\n'), *colon;
                size_t ll = e != NULL ? (size_t)(e - l) : strlen(l);

                while (ll > 0 && isspace((unsigned char)*l)) {
                        l++;
                        ll--;
                }
                colon = memchr(l, ':', ll);
                if (colon != NULL) {
                        size_t fl = (size_t)(colon - l);

                        for (const char *const *n = names; *n != NULL; n++)
                                if (strlen(*n) == fl &&
                                    strncasecmp(l, *n, fl) == 0) {
                                        const char *v = colon + 1;
                                        size_t vl = ll - fl - 1;

                                        while (vl > 0 && isspace(
                                            (unsigned char)*v)) {
                                                v++;
                                                vl--;
                                        }
                                        while (vl > 0 && isspace(
                                            (unsigned char)v[vl - 1]))
                                                vl--;
                                        if (vl == 0)
                                                continue;
                                        if (vl >= size)
                                                vl = size - 1;
                                        memcpy(out, v, vl);
                                        out[vl] = '\0';
                                        return 1;
                                }
                }
                if (e == NULL)
                        break;
                l = e + 1;
        }
        return 0;
}

/*
 * WHOIS for what, from whois.iana.org on along its referrals (and a
 * registry's to its registrar), into out: the last answer.
 */
static int
whois_referred(const char *what, char *out, size_t size)
{
        static const char *const refer[] = { "refer", "whois",
            "ReferralServer", "Registrar WHOIS Server", NULL };
        char server[256] = "whois.iana.org", next[256];

        for (int hop = 0; hop < 3; hop++) {
                if (!whois(server, what, out, size))
                        return hop > 0;
                if (!field(out, refer, next, sizeof(next)))
                        return 1;
                if (strncmp(next, "whois://", 8) == 0)
                        memmove(next, next + 8, strlen(next + 8) + 1);
                if (strncmp(next, "rwhois://", 9) == 0 ||
                    strcasecmp(next, server) == 0)
                        return 1;
                snprintf(server, sizeof(server), "%s", next);
        }
        return 1;
}

/* Who owns an address, from its registry (ARIN, RIPE and the rest). */
static int
owner_of(const unsigned char addr[4], char *out, size_t size)
{
        static const char *const org[] = { "OrgName", "org-name", "owner",
            "descr", "netname", NULL };
        static char text[65536];
        char ip[16];

        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", addr[0], addr[1], addr[2],
            addr[3]);
        return whois_referred(ip, text, sizeof(text)) &&
            field(text, org, out, size);
}

/* --- The report. --- */

static int do_whois = 1, do_public = 1;
static char domain[256], rootdomain[256], zone[256];

static void
heading(const char *what)
{
        printf("\n  %s%s%s\n%s\n", bold(), what, plain(), SECTION);
}

static void
banner(void)
{
        puts("");
        puts("                                      _ _       ");
        puts("                                     | |_|      ");
        puts("              ___ _  _ ____  ____  __| |_  ____ ");
        puts("             / __| || |  _ \\/ _  \\/ _  | |/ _  \\");
        puts("             \\__ \\ || | |_| ||_| | |_| | | |_| |");
        printf("             |___/___/| .__/\\__/_|\\____|_|\\__  | %s\n",
            VERSION);
        puts("                      | |                  __| |");
        puts("                      |_|                 |____/");
}

static void
usage(int status)
{
        banner();
        printf("\n   usage:  %s [-cPW] <domain.tld>\n"
            "           %s [-cPW] <email@domain.tld>\n"
            "           %s [-cPW] <http://hostname.tld/some/page.ext>\n"
            "           %s --about | --help\n\n"
            "   -P  no public resolvers   -W  no WHOIS   -c  no colour\n\n",
            prog, prog, prog, prog);
        exit(status);
}

static void
about(void)
{
        banner();
        puts("\n  xdig (eXtended dig): digs up a domain's DNS at once:\n"
            "   + its level, its root domain, and its zone;\n"
            "   + a quick WHOIS: registrar, name servers, expiry;\n"
            "   + the zone's records, as they can be seen;\n"
            "   + what public resolvers (Google, Cloudflare, Quad9) have\n"
            "     cached, with the zone's serial at each;\n"
            "   + the PTRs of its addresses, and who owns them;\n"
            "   + where web traffic and mail traffic go.");
        usage(0);
}

/* The domain in an address, a mail address or a URL; 0 if none. */
static int
domain_of(const char *arg)
{
        const char *p = strstr(arg, "://"), *at;
        size_t n, dots = 0;

        p = p != NULL ? p + 3 : arg;
        if ((at = strchr(p, '@')) != NULL && (strchr(p, '/') == NULL ||
            at < strchr(p, '/')))
                p = at + 1;
        n = strcspn(p, "/:?#");
        if (n == 0 || n >= sizeof(domain))
                return 0;
        for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)tolower((unsigned char)p[i]);

                if (!isalnum(c) && c != '-' && c != '.' && c != '_')
                        return 0;
                domain[i] = (char)c;
                dots += c == '.';
        }
        domain[n] = '\0';
        if (domain[n - 1] == '.')
                domain[--n] = '\0';
        return dots > 0 && domain[0] != '.' && strstr(domain, "..") == NULL;
}

/* The level, root domain and zone; 0 if the domain does not exist. */
static int
check_domain(void)
{
        const char *q = strrchr(domain, '.');
        int dots = 0;
        struct reply r;

        for (const char *s = domain; *s != '\0'; s++)
                dots += *s == '.';
        /* The root domain: the last two labels. */
        while (q > domain && *(q - 1) != '.')
                q--;
        snprintf(rootdomain, sizeof(rootdomain), "%s", q);
        snprintf(zone, sizeof(zone), "%s", domain);
        if (!query(resolver, domain, T_SOA, &r)) {
                fprintf(stderr, "%s: %s: the resolver did not answer\n", prog,
                    domain);
                return 0;
        }
        if (r.rcode == 3) {
                printf("\n  %s%s does not exist (NXDOMAIN).%s\n\n", red(),
                    domain, plain());
                return 0;
        }
        for (int i = 0; i < r.nan; i++)
                if (r.an[i].type == T_SOA)
                        snprintf(zone, sizeof(zone), "%s", r.an[i].name);
        for (int i = 0; i < r.nns; i++)
                if (r.ns[i].type == T_SOA)
                        snprintf(zone, sizeof(zone), "%s", r.ns[i].name);

        printf("\n  %s%s - ", bold(), domain);
        if (dots == 1)
                printf("2ND-LEVEL! (HAS 1 DOT)");
        else if (dots == 2)
                printf("3RD-LEVEL! (HAS 2 DOTS)");
        else
                printf("NOT A ROOT DOMAIN! (HAS %d DOTS)", dots);
        printf("%s\n%s\n  Root Domain: %s\n         Zone: %s\n", plain(),
            SECTION, rootdomain, zone);
        return 1;
}

static void
show_whois(void)
{
        static const char *const reg[] = { "Registrar", "registrar",
            "Sponsoring Registrar", NULL };
        static const char *const exp[] = { "Registry Expiry Date",
            "Registrar Registration Expiration Date", "Expiration Date",
            "Expiry Date", "expires", "paid-till", NULL };
        static char text[65536];
        char v[256];
        int any = 0;

        heading("CURRENT WHOIS");
        printf("  Registration & Nameservers Only:\n");
        if (!whois_referred(rootdomain, text, sizeof(text))) {
                printf("     %sNo WHOIS server answered.%s\n", red(), plain());
                return;
        }
        if (field(text, reg, v, sizeof(v))) {
                printf("     Registrar: %s\n", v);
                any = 1;
        }
        /* Every name server line, each its own. */
        for (const char *l = text; l != NULL && *l != '\0'; ) {
                static const char *const ns[] = { "Name Server", "nserver",
                    NULL };
                const char *e = strchr(l, '\n');
                char line[512];
                size_t ll = e != NULL ? (size_t)(e - l) : strlen(l);

                if (ll >= sizeof(line))
                        ll = sizeof(line) - 1;
                memcpy(line, l, ll);
                line[ll] = '\0';
                if (field(line, ns, v, sizeof(v))) {
                        printf("     Name Server: %s\n", v);
                        any = 1;
                }
                l = e != NULL ? e + 1 : NULL;
        }
        if (field(text, exp, v, sizeof(v))) {
                printf("     Expires: %s\n", v);
                any = 1;
        }
        if (!any)
                printf("     %sThe WHOIS record could not be parsed; try"
                    " whois %s%s\n", red(), rootdomain, plain());
}

static void
show_zone(void)
{
        static const unsigned types[] = { T_SOA, T_NS, T_A, T_AAAA, T_MX,
            T_TXT, T_CAA, T_CNAME };
        int any = 0;

        heading("ZONE RECORDS (has propagated)");
        printf("  Currently Visible:\n");
        for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
                struct reply r;

                if (!query(resolver, domain, types[t], &r))
                        continue;
                for (int i = 0; i < r.nan; i++) {
                        printf("     %s.\t%lu\tIN\t%s\t%s\n", r.an[i].name,
                            r.an[i].ttl, typename(r.an[i].type),
                            r.an[i].data);
                        any = 1;
                }
        }
        if (!any)
                printf("     %sNo Records Found!%s\n", red(), plain());
}

/* "A: addr [ PTR: name ]", and who owns it. */
static void
show_addr(const char *prefix, const unsigned char addr[4])
{
        char ptr[256], own[256];

        ptr_of(addr, ptr, sizeof(ptr));
        printf("      %s%u.%u.%u.%u [ ", prefix, addr[0], addr[1], addr[2],
            addr[3]);
        if (ptr[0] != '\0')
                printf("PTR: %s", ptr);
        else
                printf("%sNo PTR Configured%s", red(), plain());
        printf(" ]\n");
        if (do_whois && owner_of(addr, own, sizeof(own)))
                printf("\t     -> %sIP Owned by: %s%s\n", green(), own,
                    plain());
}

static void
show_public(void)
{
        static const struct { const char *who, *ip; } pub[] = {
                { "Google", "8.8.8.8" }, { "Google", "8.8.4.4" },
                { "Cloudflare", "1.1.1.1" }, { "Cloudflare", "1.0.0.1" },
                { "Quad9", "9.9.9.9" },
        };

        heading("PUBLIC DNS (what presumably has propagated & been cached)");
        for (size_t i = 0; i < sizeof(pub) / sizeof(pub[0]); i++) {
                char servers[64];
                struct reply r, s;
                int any = 0;

                if (i == 0 || strcmp(pub[i].who, pub[i - 1].who) != 0)
                        printf("  %s's Public DNS:\n", pub[i].who);
                server_of(pub[i].ip, servers);
                if (!query(servers, domain, T_A, &r)) {
                        printf("     @%s: %sConnection Timed Out!%s\n\n",
                            pub[i].ip, red(), plain());
                        continue;
                }
                for (int k = 0; k < r.nan; k++) {
                        char ptr[256];

                        if (r.an[k].type != T_A)
                                continue;
                        any = 1;
                        ptr_of(r.an[k].addr, ptr, sizeof(ptr));
                        printf("     @%s: %s [ ", pub[i].ip, r.an[k].data);
                        if (ptr[0] != '\0')
                                printf("PTR: %s", ptr);
                        else
                                printf("%sNo PTR Configured%s", red(),
                                    plain());
                        printf(" ]\n");
                }
                if (!any)
                        printf("     @%s: %sNo Records Found!%s\n", pub[i].ip,
                            red(), plain());
                if (query(servers, zone, T_SOA, &s) && s.nan > 0 &&
                    s.an[0].type == T_SOA)
                        printf("\t     -> %sSerial: %lu%s\n\n", yellow(),
                            s.an[0].serial, plain());
                else
                        printf("\t     -> %sThe serial cannot be "
                            "determined.%s\n\n", red(), plain());
        }
}

static int
by_pref(const void *a, const void *b)
{
        const struct rr *x = a, *y = b;

        return (int)x->pref - (int)y->pref;
}

static void
show_where(void)
{
        struct reply a, m;
        int any = 0;

        heading("WHERE WEB TRAFFIC GOES");
        printf("  Active A Record(s):\n");
        if (query(resolver, domain, T_A, &a))
                for (int i = 0; i < a.nan; i++) {
                        if (a.an[i].type == T_CNAME)
                                printf("      CNAME: %s.\n", a.an[i].target);
                        if (a.an[i].type != T_A)
                                continue;
                        show_addr("A: ", a.an[i].addr);
                        any = 1;
                }
        if (!any)
                printf("     %sNo Records Found!%s\n", red(), plain());

        heading("WHERE MAIL TRAFFIC GOES");
        printf("  Active MX Record(s):\n");
        if (!query(resolver, domain, T_MX, &m) || m.nan == 0)
                if (strcmp(zone, domain) != 0)
                        (void)query(resolver, zone, T_MX, &m);
        qsort(m.an, (size_t)m.nan, sizeof(m.an[0]), by_pref);
        any = 0;
        for (int i = 0; i < m.nan; i++) {
                struct reply h;
                char pre[300];
                int addrs = 0;

                if (m.an[i].type != T_MX)
                        continue;
                any = 1;
                snprintf(pre, sizeof(pre), "MX: %u %s => ", m.an[i].pref,
                    m.an[i].target);
                if (query(resolver, m.an[i].target, T_A, &h))
                        for (int k = 0; k < h.nan; k++)
                                if (h.an[k].type == T_A) {
                                        show_addr(pre, h.an[k].addr);
                                        addrs++;
                                }
                if (addrs == 0)
                        printf("      %s%sno address%s\n", pre, red(),
                            plain());
        }
        if (!any)
                printf("     %sNo Records Found!%s\n", red(), plain());
}

int
main(int argc, char **argv)
{
        int c, nocolor = 0;

        if (argc == 2 && strcmp(argv[1], "--help") == 0)
                usage(0);
        if (argc == 2 && strcmp(argv[1], "--about") == 0)
                about();
        while ((c = getopt(argc, argv, "cPW")) != -1) {
                switch (c) {
                case 'c':
                        nocolor = 1;
                        break;
                case 'P':
                        do_public = 0;
                        break;
                case 'W':
                        do_whois = 0;
                        break;
                default:
                        usage(2);
                }
        }
        if (optind != argc - 1)
                usage(optind == argc ? 0 : 2);
        if (!domain_of(argv[optind])) {
                banner();
                printf("\n   %sERROR!%s \"%s\" is an invalid entry!\n",
                    red(), plain(), argv[optind]);
                usage(1);
        }
        {
                const char *term = getenv("TERM");

                color = !nocolor && isatty(STDOUT_FILENO) && term != NULL &&
                    strcmp(term, "dumb") != 0;
        }
        if (dns_resolvconfip(resolver) == -1) {
                fprintf(stderr, "%s: no resolver: %s\n", prog,
                    strerror(errno));
                return 111;
        }
        banner();
        if (!check_domain())
                return 1;
        if (do_whois)
                show_whois();
        show_zone();
        if (do_public)
                show_public();
        show_where();
        puts("");
        return 0;
}
