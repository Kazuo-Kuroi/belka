/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * <netinet/in_systm.h>: the BSDs' network-order integer names, which
 * mlibc's header leaves out.
 */

#ifndef _COMPAT_NETINET_IN_SYSTM_H_
#define _COMPAT_NETINET_IN_SYSTM_H_

#include_next <netinet/in_systm.h>
#include <stdint.h>

typedef uint16_t        n_short;
typedef uint32_t        n_long;
typedef uint32_t        n_time;

#endif /* _COMPAT_NETINET_IN_SYSTM_H_ */
