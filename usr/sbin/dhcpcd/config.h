/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/*
 * dhcpcd's configuration for Belka, written here rather than by its
 * configure script: IPv4 and ARP only (no IPv6, IPv4LL, authentication
 * or privilege separation), Belka's own interface and link back ends
 * (if-belka.c, bpf-belka.c), and dhcpcd's portable versions of what the
 * C library lacks.
 */

#define CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SYSCONFDIR      "/etc"
#define SBINDIR         "/sbin"
#define LIBDIR          "/lib"
#define LIBEXECDIR      "/libexec"
#define DBDIR           "/var/db/dhcpcd"
#define RUNDIR          "/var/run/dhcpcd"
#define DATADIR         "/usr/share"

#define HAVE_REALLOCARRAY
#define HAVE_OPEN_MEMSTREAM
#define HAVE_EXPLICIT_BZERO
#define HAVE_STRLCPY
#define HAVE_DPRINTF
#define IOCTL_REQUEST_TYPE      unsigned long

/* dhcpcd's commands for if_address(), as the BSDs number them. */
#define RTM_NEWADDR     0xc
#define RTM_DELADDR     0xd

#include "compat/arc4random.h"
#include "compat/arc4random_uniform.h"
#include "compat/closefrom.h"
#include "compat/consttime_memequal.h"
#include "compat/getpeereid.h"
#include "compat/memset_explicit.h"
#include "compat/pidfile.h"
#include "compat/progname.h"
#include "compat/setproctitle.h"
#include "compat/strtoi.h"
#include "rbtree.h"
