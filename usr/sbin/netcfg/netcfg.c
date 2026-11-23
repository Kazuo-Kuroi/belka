/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * netcfg: plumbs and configures Belka's IPv4 stack, through the I_PLINK
 * of STREAMS and the ioctls of /dev/ip (<sys/ipconf.h>).
 *
 *      netcfg plumb                    link IP under UDP and TCP
 *      netcfg attach if                an interface from /dev/if
 *      netcfg addr if a.b.c.d/len      its address and subnet
 *      netcfg route add dest gw        a route (dest: a.b.c.d/len or
 *      netcfg route delete dest [gw]       default)
 *      netcfg forwarding [on|off]      forward, or not; or say which
 *      netcfg show                     interfaces, routes, ARP
 *
 * The links are persistent: they outlast netcfg, until the system goes
 * down. Plumbing what is plumbed already is not an error.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <sys/ipconf.h>
#include <unistd.h>

static const char *prog = "netcfg";

static void
usage(void)
{
        fprintf(stderr,
            "usage: %s plumb\n"
            "       %s attach interface\n"
            "       %s addr interface address/length\n"
            "       %s route add destination gateway\n"
            "       %s route delete destination [gateway]\n"
            "       %s forwarding [on | off]\n"
            "       %s show\n", prog, prog, prog, prog, prog, prog, prog);
        exit(2);
}

static void
warnerr(const char *what, int error)
{
        fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(error));
}

static int
open_dev(const char *name)
{
        char path[64];
        int fd;

        snprintf(path, sizeof(path), "/dev/%s", name);
        if ((fd = open(path, O_RDWR)) < 0)
                warnerr(path, errno);
        return fd;
}

static int
istr(int fd, int cmd, void *data, int len)
{
        struct strioctl sio = { cmd, 0, len, data };

        return ioctl(fd, I_STR, &sio);
}

/* "a.b.c.d/len", or "default" (with defok); network order. */
static int
parse_net(const char *s, int defok, uint32_t *addr, uint32_t *mask)
{
        char buf[32], *slash, *end;
        long len = 32;
        struct in_addr in;

        if (defok && strcmp(s, "default") == 0) {
                *addr = *mask = 0;
                return 0;
        }
        if (strlen(s) >= sizeof(buf))
                return -1;
        strcpy(buf, s);
        if ((slash = strchr(buf, '/')) != NULL) {
                *slash++ = '\0';
                len = strtol(slash, &end, 10);
                if (*slash == '\0' || *end != '\0' || len < 0 || len > 32)
                        return -1;
        }
        if (inet_pton(AF_INET, buf, &in) != 1)
                return -1;
        *addr = in.s_addr;
        *mask = len == 0 ? 0 : htonl(0xffffffffu << (32 - len));
        return 0;
}

static int
parse_addr(const char *s, uint32_t *addr)
{
        struct in_addr in;

        if (inet_pton(AF_INET, s, &in) != 1)
                return -1;
        *addr = in.s_addr;
        return 0;
}

static int
masklen(uint32_t mask)
{
        uint32_t m = ntohl(mask);
        int n = 0;

        while (m & 0x80000000u) {
                n++;
                m <<= 1;
        }
        return n;
}

static const char *
ntoa(uint32_t addr, char *buf)
{
        struct in_addr in = { addr };

        return inet_ntop(AF_INET, &in, buf, INET_ADDRSTRLEN);
}

/* Link an IP stream under each transport; already linked is fine. */
static int
cmd_plumb(void)
{
        static const char *const transports[] = { "udp", "tcp" };
        int failed = 0;

        for (size_t i = 0; i < sizeof(transports) / sizeof(transports[0]);
            i++) {
                int t = open_dev(transports[i]), ip = open_dev("ip");

                if (t < 0 || ip < 0) {
                        failed = 1;
                } else if (ioctl(t, I_PLINK, ip) < 0 && errno != EBUSY) {
                        warnerr(transports[i], errno);
                        failed = 1;
                }
                if (ip >= 0)
                        close(ip);
                if (t >= 0)
                        close(t);
        }
        return failed;
}

/* An interface: its DLPI device twice under IP, for IPv4 and ARP. */
static int
cmd_attach(const char *name)
{
        struct ip_ifattach ia;
        int ipfd, d1, d2, failed = 1;

        if (strlen(name) >= IPCONF_NAMELEN) {
                fprintf(stderr, "%s: %s: name too long\n", prog, name);
                return 1;
        }
        if ((ipfd = open_dev("ip")) < 0)
                return 1;
        d1 = open_dev(name);
        d2 = d1 >= 0 ? open_dev(name) : -1;
        memset(&ia, 0, sizeof(ia));
        strcpy(ia.ia_name, name);
        if (d1 >= 0 && d2 >= 0 &&
            (ia.ia_ipmux = ioctl(ipfd, I_PLINK, d1)) > 0 &&
            (ia.ia_arpmux = ioctl(ipfd, I_PLINK, d2)) > 0) {
                if (istr(ipfd, IP_IFATTACH, &ia, sizeof(ia)) == 0)
                        failed = 0;
                else
                        warnerr(name, errno);
                if (failed) {
                        (void)ioctl(ipfd, I_PUNLINK, ia.ia_ipmux);
                        (void)ioctl(ipfd, I_PUNLINK, ia.ia_arpmux);
                }
        } else if (d1 >= 0 && d2 >= 0) {
                warnerr(name, errno);
                if (ia.ia_ipmux > 0)
                        (void)ioctl(ipfd, I_PUNLINK, ia.ia_ipmux);
        }
        if (d2 >= 0)
                close(d2);
        if (d1 >= 0)
                close(d1);
        close(ipfd);
        return failed;
}

static int
cmd_addr(const char *name, const char *net)
{
        struct ip_ifaddr ifa;
        int ipfd, failed = 0;

        memset(&ifa, 0, sizeof(ifa));
        if (strlen(name) >= IPCONF_NAMELEN || strchr(net, '/') == NULL ||
            parse_net(net, 0, &ifa.ifa_addr, &ifa.ifa_mask) != 0) {
                fprintf(stderr, "%s: %s %s: not an interface and "
                    "address/length\n", prog, name, net);
                return 1;
        }
        strcpy(ifa.ifa_name, name);
        if ((ipfd = open_dev("ip")) < 0)
                return 1;
        if (istr(ipfd, IP_IFADDR, &ifa, sizeof(ifa)) != 0) {
                warnerr(name, errno);
                failed = 1;
        }
        close(ipfd);
        return failed;
}

static int
cmd_route(int add, const char *dest, const char *gw)
{
        struct ip_route rt;
        int ipfd, failed = 0;

        memset(&rt, 0, sizeof(rt));
        if (parse_net(dest, 1, &rt.rt_dst, &rt.rt_mask) != 0 ||
            (gw != NULL && parse_addr(gw, &rt.rt_gateway) != 0)) {
                fprintf(stderr, "%s: %s %s: not a destination and "
                    "gateway\n", prog, dest, gw != NULL ? gw : "");
                return 1;
        }
        if ((ipfd = open_dev("ip")) < 0)
                return 1;
        if (istr(ipfd, add ? IP_ROUTEADD : IP_ROUTEDEL, &rt,
            sizeof(rt)) != 0) {
                warnerr(dest, errno);
                failed = 1;
        }
        close(ipfd);
        return failed;
}

/* IP forwarding on or off (on: 1, 0), or (on: -1) say whether it is. */
static int
cmd_forwarding(int on)
{
        int32_t v = on;
        int ipfd, r, failed = 0;

        if ((ipfd = open_dev("ip")) < 0)
                return 1;
        if (on < 0) {
                if ((r = istr(ipfd, IP_GETFORWARD, NULL, 0)) < 0) {
                        warnerr("forwarding", errno);
                        failed = 1;
                } else {
                        printf("forwarding %s\n", r ? "on" : "off");
                }
        } else if (istr(ipfd, IP_SETFORWARD, &v, sizeof(v)) != 0) {
                warnerr("forwarding", errno);
                failed = 1;
        }
        close(ipfd);
        return failed;
}

/* One of the lists, in a buffer that grows until it all fits. */
static void *
getlist(int fd, int cmd, size_t size, int *np)
{
        void *buf = NULL;
        int n = 16;

        for (;;) {
                int got;
                void *nb = realloc(buf, (size_t)n * size);

                if (nb == NULL) {
                        free(buf);
                        return NULL;
                }
                buf = nb;
                if ((got = istr(fd, cmd, buf, (int)((size_t)n * size))) < 0) {
                        free(buf);
                        return NULL;
                }
                if (got <= n) {
                        *np = got;
                        return buf;
                }
                n = got;
        }
}

static int
cmd_show(void)
{
        char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
        struct ip_ifinfo *ifs;
        struct ip_route *rts;
        struct ip_arpinfo *arps;
        int ipfd, n;

        if ((ipfd = open_dev("ip")) < 0)
                return 1;
        if ((ifs = getlist(ipfd, IP_IFLIST, sizeof(*ifs), &n)) == NULL) {
                warnerr("interfaces", errno);
                close(ipfd);
                return 1;
        }
        for (int i = 0; i < n; i++) {
                struct ip_ifinfo *ii = &ifs[i];

                printf("%s: %s/%d mtu %u <%s%s%s%s>", ii->ii_name,
                    ntoa(ii->ii_addr, a), masklen(ii->ii_mask), ii->ii_mtu,
                    (ii->ii_flags & IPIF_UP) != 0 ? "UP" : "DOWN",
                    (ii->ii_flags & IPIF_BROADCAST) != 0 ? ",BROADCAST" : "",
                    (ii->ii_flags & IPIF_LOOPBACK) != 0 ? ",LOOPBACK" : "",
                    (ii->ii_flags & IPIF_RUNNING) != 0 ? ",RUNNING" : "");
                for (int j = 0; j < ii->ii_hwlen && j < 6; j++)
                        printf("%s%02x", j == 0 ? " ether " : ":",
                            ii->ii_hwaddr[j]);
                printf("\n\tpackets in %llu out %llu, errors %llu, "
                    "dropped %llu\n", (unsigned long long)ii->ii_ipackets,
                    (unsigned long long)ii->ii_opackets,
                    (unsigned long long)ii->ii_ierrors,
                    (unsigned long long)ii->ii_drops);
        }
        free(ifs);
        printf("forwarding %s\n", istr(ipfd, IP_GETFORWARD, NULL, 0) > 0 ?
            "on" : "off");
        if ((rts = getlist(ipfd, IP_ROUTELIST, sizeof(*rts), &n)) != NULL) {
                printf("routes:\n");
                for (int i = 0; i < n; i++) {
                        if (rts[i].rt_mask == 0)
                                printf("\tdefault");
                        else
                                printf("\t%s/%d", ntoa(rts[i].rt_dst, a),
                                    masklen(rts[i].rt_mask));
                        if (rts[i].rt_gateway != 0)
                                printf(" via %s", ntoa(rts[i].rt_gateway, b));
                        printf(" on %.*s\n", IPCONF_NAMELEN,
                            rts[i].rt_ifname);
                }
                free(rts);
        }
        if ((arps = getlist(ipfd, IP_ARPLIST, sizeof(*arps), &n)) != NULL) {
                if (n > 0)
                        printf("arp:\n");
                for (int i = 0; i < n; i++) {
                        printf("\t%s on %.*s", ntoa(arps[i].ai_addr, a),
                            IPCONF_NAMELEN, arps[i].ai_ifname);
                        if ((arps[i].ai_flags & IPARP_COMPLETE) != 0)
                                for (int j = 0; j < 6; j++)
                                        printf("%s%02x", j == 0 ? " at " :
                                            ":", arps[i].ai_hwaddr[j]);
                        else
                                printf(" (incomplete)");
                        printf("\n");
                }
                free(arps);
        }
        close(ipfd);
        return 0;
}

int
main(int argc, char **argv)
{
        if (argc > 0 && argv[0] != NULL) {
                const char *slash = strrchr(argv[0], '/');

                prog = slash != NULL ? slash + 1 : argv[0];
        }
        if (argc < 2)
                usage();
        if (strcmp(argv[1], "plumb") == 0 && argc == 2)
                return cmd_plumb();
        if (strcmp(argv[1], "attach") == 0 && argc == 3)
                return cmd_attach(argv[2]);
        if (strcmp(argv[1], "addr") == 0 && argc == 4)
                return cmd_addr(argv[2], argv[3]);
        if (strcmp(argv[1], "route") == 0 && argc >= 4 && argc <= 5) {
                if (strcmp(argv[2], "add") == 0 && argc == 5)
                        return cmd_route(1, argv[3], argv[4]);
                if (strcmp(argv[2], "delete") == 0)
                        return cmd_route(0, argv[3], argc == 5 ? argv[4] :
                            NULL);
        }
        if (strcmp(argv[1], "forwarding") == 0 && argc <= 3) {
                if (argc == 2)
                        return cmd_forwarding(-1);
                if (strcmp(argv[2], "on") == 0)
                        return cmd_forwarding(1);
                if (strcmp(argv[2], "off") == 0)
                        return cmd_forwarding(0);
        }
        if (strcmp(argv[1], "show") == 0 && argc == 2)
                return cmd_show();
        usage();
        return 2;
}
