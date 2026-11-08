/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Machine-dependent constants. The page macro names are those of
 * System V.
 */

#ifndef _MACHINE_PARAM_H_
#define _MACHINE_PARAM_H_

#include <machine/types.h>

#define PAGESHIFT       12
#define PAGESIZE        (1UL << PAGESHIFT)
#define PAGEOFFSET      (PAGESIZE - 1)

/* Bytes to pages, rounding down (btop) or up (btopr); pages to bytes. */
#define btop(x)         ((pfn_t)(x) >> PAGESHIFT)
#define btopr(x)        (((pfn_t)(x) + PAGEOFFSET) >> PAGESHIFT)
#define ptob(x)         ((paddr_t)(x) << PAGESHIFT)

#define ptrunc(x)       ((paddr_t)(x) & ~(paddr_t)PAGEOFFSET)
#define pround(x)       (((paddr_t)(x) + PAGEOFFSET) & ~(paddr_t)PAGEOFFSET)

#define MAXCPUS         256
#define MAXNODES        16      /* NUMA nodes */

#endif /* _MACHINE_PARAM_H_ */
