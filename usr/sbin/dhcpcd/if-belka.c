/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * dhcpcd's interface back end for Belka, in place of its if-bsd.c,
 * if-linux.c and if-sun.c: interfaces, addresses and routes through
 * the IP driver's ioctls on /dev/ip (<sys/ipconf.h>), as netcfg(8) uses
 * them. Belka has no routing socket, so there are no change messages
 * to follow (link_fd stays -1): dhcpcd reads the routes again whenever
 * it builds them, and records an address once it has set it.
 *
 * Each IP interface has one address. Setting another replaces it, and
 * clearing it (IP_IFADDR with address 0) takes away every route by the
 * interface. The kernel adds the subnet route itself.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>

#include <sys/ipconf.h>

#include "config.h"
#include "common.h"
#include "dhcpcd.h"
#include "if.h"
#include "ipv4.h"
#include "logerr.h"
#include "route.h"
#include "sa.h"

static int ip_fd = -1;

/* /dev/ip, opened once and kept (also across daemonising). */
static int
ipdev(void)
{
        if (ip_fd == -1)
                ip_fd = open("/dev/ip", O_RDWR | O_CLOEXEC);
        return ip_fd;
}

static int
istr(int cmd, void *data, size_t len)
{
        struct strioctl sio = { cmd, 0, (int)len, data };
        int fd = ipdev();

        if (fd == -1)
                return -1;
        return ioctl(fd, I_STR, &sio);
}

/* An IP_IFLIST or IP_ROUTELIST: a malloc'd array and its length. */
static void *
iplist(int cmd, size_t size, int *np)
{
        void *buf = NULL;
        int n = 16;

        for (;;) {
                void *nb = reallocarray(buf, (size_t)n, size);
                int got;

                if (nb == NULL) {
                        free(buf);
                        return NULL;
                }
                buf = nb;
                if ((got = istr(cmd, buf, (size_t)n * size)) == -1) {
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
ifinfo(const char *name, struct ip_ifinfo *out)
{
        struct ip_ifinfo *ifs;
        int n, i;

        if ((ifs = iplist(IP_IFLIST, sizeof(*ifs), &n)) == NULL)
                return -1;
        for (i = 0; i < n; i++)
                if (strncmp(ifs[i].ii_name, name, sizeof(ifs[i].ii_name)) == 0)
                        break;
        if (i < n)
                *out = ifs[i];
        free(ifs);
        if (i == n) {
                errno = ENXIO;
                return -1;
        }
        return 0;
}

int
os_init(void)
{
        return 0;
}

int
if_opensockets_os(struct dhcpcd_ctx *ctx)
{
        ctx->link_fd = -1;
        return ipdev() == -1 ? -1 : 0;
}

void
if_closesockets_os(struct dhcpcd_ctx *ctx)
{
        UNUSED(ctx);
        if (ip_fd != -1) {
                close(ip_fd);
                ip_fd = -1;
        }
}

int
if_handlelink(struct dhcpcd_ctx *ctx)
{
        UNUSED(ctx);
        return 0;
}

int
if_init(struct interface *ifp)
{
        UNUSED(ifp);
        return 0;
}

int
if_init_os(struct interface *ifp)
{
        UNUSED(ifp);
        return 0;
}

int
if_conf(struct interface *ifp)
{
        UNUSED(ifp);
        return 0;
}

/* A virtio-net link is up while the device is there. */
int
if_carrier(struct interface *ifp, const void *ifadata)
{
        UNUSED(ifadata);
        return ifp->flags & IFF_RUNNING ? LINK_UP : LINK_UNKNOWN;
}

bool
if_roaming(struct interface *ifp)
{
        UNUSED(ifp);
        return false;
}

int
if_mtu_os(const struct interface *ifp)
{
        struct ip_ifinfo ii;

        if (ifinfo(ifp->name, &ii) == -1)
                return -1;
        return (int)ii.ii_mtu;
}

int
if_setmac(struct interface *ifp, void *mac, uint8_t maclen)
{
        UNUSED(ifp);
        UNUSED(mac);
        UNUSED(maclen);
        errno = ENOTSUP;
        return -1;
}

int
if_getssid(struct interface *ifp)
{
        UNUSED(ifp);
        errno = ENOTSUP;
        return -1;
}

bool
if_ignore(struct dhcpcd_ctx *ctx, const char *ifname)
{
        UNUSED(ctx);
        UNUSED(ifname);
        return false;
}

unsigned short
if_vlanid(const struct interface *ifp)
{
        UNUSED(ifp);
        return 0;
}

int
if_vimaster(struct dhcpcd_ctx *ctx, const char *ifname)
{
        UNUSED(ctx);
        UNUSED(ifname);
        return 0;
}

int
if_machinearch(char *str, size_t len)
{
        UNUSED(str);
        UNUSED(len);
        return 0;
}

/*
 * The interface list: for each IP interface, its station address as an
 * AF_PACKET entry (the loopback's as ARPHRD_LOOPBACK, which dhcpcd
 * passes over), then its IPv4 address, if it has one. The index is the
 * interface's place in IP's list, from 1.
 */
static struct sockaddr *
sa_in_new(in_addr_t a)
{
        struct sockaddr_in *sin = calloc(1, sizeof(*sin));

        if (sin != NULL) {
                sin->sin_family = AF_INET;
                sin->sin_addr.s_addr = a;
        }
        return (struct sockaddr *)sin;
}

static struct ifaddrs *
ifa_new(const struct ip_ifinfo *ii, unsigned int flags)
{
        struct ifaddrs *ifa = calloc(1, sizeof(*ifa));

        if (ifa == NULL)
                return NULL;
        if ((ifa->ifa_name = strndup(ii->ii_name, sizeof(ii->ii_name))) ==
            NULL) {
                free(ifa);
                return NULL;
        }
        ifa->ifa_flags = flags;
        return ifa;
}

void
freeifaddrs(struct ifaddrs *ifa)
{
        while (ifa != NULL) {
                struct ifaddrs *next = ifa->ifa_next;

                free(ifa->ifa_name);
                free(ifa->ifa_addr);
                free(ifa->ifa_netmask);
                free(ifa->ifa_broadaddr);
                free(ifa);
                ifa = next;
        }
}

int
getifaddrs(struct ifaddrs **ifap)
{
        struct ifaddrs *head = NULL, **tail = &head;
        struct ip_ifinfo *ifs;
        int n;

        if ((ifs = iplist(IP_IFLIST, sizeof(*ifs), &n)) == NULL)
                return -1;
        for (int i = 0; i < n; i++) {
                const struct ip_ifinfo *ii = &ifs[i];
                int loop = (ii->ii_flags & IPIF_LOOPBACK) != 0;
                unsigned int flags = IFF_UP | IFF_RUNNING |
                    (loop ? IFF_LOOPBACK : IFF_BROADCAST);
                struct sockaddr_ll *sll;
                struct ifaddrs *ifa;

                if ((ifa = ifa_new(ii, flags)) == NULL)
                        goto fail;
                *tail = ifa;
                tail = &ifa->ifa_next;
                if ((sll = calloc(1, sizeof(*sll))) == NULL)
                        goto fail;
                sll->sll_family = AF_PACKET;
                sll->sll_ifindex = i + 1;
                sll->sll_hatype = loop ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
                sll->sll_halen = loop ? 0 : ii->ii_hwlen;
                if (sll->sll_halen > sizeof(sll->sll_addr))
                        sll->sll_halen = 0;
                memcpy(sll->sll_addr, ii->ii_hwaddr, sll->sll_halen);
                ifa->ifa_addr = (struct sockaddr *)sll;

                if (ii->ii_addr == INADDR_ANY)
                        continue;
                if ((ifa = ifa_new(ii, flags)) == NULL)
                        goto fail;
                *tail = ifa;
                tail = &ifa->ifa_next;
                ifa->ifa_addr = sa_in_new(ii->ii_addr);
                ifa->ifa_netmask = sa_in_new(ii->ii_mask);
                if (!loop)
                        ifa->ifa_broadaddr = sa_in_new(ii->ii_addr |
                            ~ii->ii_mask);
                if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL ||
                    (!loop && ifa->ifa_broadaddr == NULL))
                        goto fail;
        }
        free(ifs);
        *ifap = head;
        return 0;

fail:
        free(ifs);
        freeifaddrs(head);
        errno = ENOMEM;
        return -1;
}

int
if_address(unsigned char cmd, const struct ipv4_addr *ia)
{
        struct ip_ifaddr ifa;
        struct ip_ifinfo ii;

        memset(&ifa, 0, sizeof(ifa));
        strlcpy(ifa.ifa_name, ia->iface->name, sizeof(ifa.ifa_name));
        switch (cmd) {
        case RTM_NEWADDR:
                ifa.ifa_addr = ia->addr.s_addr;
                ifa.ifa_mask = ia->mask.s_addr;
                break;
        case RTM_DELADDR:
                /* Only if it is still this one: another replaced it. */
                if (ifinfo(ia->iface->name, &ii) == -1)
                        return -1;
                if (ii.ii_addr != ia->addr.s_addr) {
                        errno = EADDRNOTAVAIL;
                        return -1;
                }
                break;
        default:
                errno = EINVAL;
                return -1;
        }
        return istr(IP_IFADDR, &ifa, sizeof(ifa)) == -1 ? -1 : 0;
}

int
if_addrflags(const struct interface *ifp, const struct in_addr *ia,
    const char *alias)
{
        UNUSED(ifp);
        UNUSED(ia);
        UNUSED(alias);
        return 0;
}

static in_addr_t
rt_in(const struct sockaddr *sa)
{
        if (sa == NULL || sa->sa_family != AF_INET)
                return INADDR_ANY;
        return satosin(sa)->sin_addr.s_addr;
}

int
if_route(unsigned char cmd, const struct rt *rt)
{
        struct ip_route r;

        if (rt->rt_dest->sa_family != AF_INET) {
                errno = EAFNOSUPPORT;
                return -1;
        }
        memset(&r, 0, sizeof(r));
        r.rt_dst = rt_in(rt->rt_dest);
        r.rt_mask = rt_in(rt->rt_netmask);
        r.rt_gateway = rt_in(rt->rt_gateway);
        if (rt->rt_ifp != NULL)
                strlcpy(r.rt_ifname, rt->rt_ifp->name, sizeof(r.rt_ifname));
        switch (cmd) {
        case RTM_ADD:
                return istr(IP_ROUTEADD, &r, sizeof(r)) == -1 ? -1 : 0;
        case RTM_DELETE:
                return istr(IP_ROUTEDEL, &r, sizeof(r)) == -1 ? -1 : 0;
        case RTM_CHANGE:
                /* One route per destination: replace it. */
                if (istr(IP_ROUTEDEL, &(struct ip_route){ .rt_dst = r.rt_dst,
                    .rt_mask = r.rt_mask }, sizeof(r)) == -1 &&
                    errno != ESRCH)
                        return -1;
                return istr(IP_ROUTEADD, &r, sizeof(r)) == -1 ? -1 : 0;
        default:
                errno = EINVAL;
                return -1;
        }
}

/* The kernel's IPv4 routes on interfaces dhcpcd knows. */
int
if_initrt(struct dhcpcd_ctx *ctx, rb_tree_t *routes, int af)
{
        struct ip_route *rts;
        struct in_addr in;
        int n;

        if (af != AF_UNSPEC && af != AF_INET)
                return 0;
        if ((rts = iplist(IP_ROUTELIST, sizeof(*rts), &n)) == NULL)
                return -1;
        for (int i = 0; i < n; i++) {
                char name[sizeof(rts[i].rt_ifname) + 1];
                struct interface *ifp;
                struct rt *rt;

                memcpy(name, rts[i].rt_ifname, sizeof(rts[i].rt_ifname));
                name[sizeof(rts[i].rt_ifname)] = '\0';
                if ((ifp = if_find(ctx->ifaces, name)) == NULL)
                        continue;
                if ((rt = rt_new(ifp)) == NULL) {
                        free(rts);
                        return -1;
                }
                in.s_addr = rts[i].rt_dst;
                sa_in_init(rt->rt_dest, &in);
                in.s_addr = rts[i].rt_mask;
                sa_in_init(rt->rt_netmask, &in);
                if (rts[i].rt_gateway != INADDR_ANY) {
                        in.s_addr = rts[i].rt_gateway;
                        sa_in_init(rt->rt_gateway, &in);
                } else {
                        /* As dhcpcd writes a subnet route. */
                        rt->rt_gateway->sa_family = AF_UNSPEC;
                }
                {
                        struct ipv4_state *state = IPV4_STATE(ifp);
                        struct ipv4_addr *ia;

                        if (state != NULL &&
                            (ia = TAILQ_FIRST(&state->addrs)) != NULL)
                                sa_in_init(rt->rt_ifa, &ia->addr);
                }
                if (rb_tree_insert_node(routes, rt) != rt)
                        rt_free(rt);
        }
        free(rts);
        return 0;
}
