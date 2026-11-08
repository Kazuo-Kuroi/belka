/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Machine-dependent kernel types.
 */

#ifndef _MACHINE_TYPES_H_
#define _MACHINE_TYPES_H_

#include <stdint.h>

typedef uint64_t        paddr_t;        /* physical address */
typedef uint64_t        psize_t;        /* size in physical memory */
typedef uint64_t        vaddr_t;        /* virtual address */
typedef uint64_t        vsize_t;        /* size in virtual memory */
typedef uint64_t        pfn_t;          /* physical page frame number */

#endif /* _MACHINE_TYPES_H_ */
