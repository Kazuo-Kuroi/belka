/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Virtual memory layout.
 *
 * All physical memory is mapped at dmap_base (the direct map), so the
 * kernel can reach any physical page without creating a mapping. For
 * now the boot loader's direct map is used and dmap_base is its offset;
 * the kernel's own pmap will build an equivalent one.
 */

#ifndef _MACHINE_VMPARAM_H_
#define _MACHINE_VMPARAM_H_

#include <stdint.h>

#include <machine/types.h>

extern vaddr_t dmap_base;

static inline void *
phys_to_kva(paddr_t pa)
{
        return (void *)(uintptr_t)(dmap_base + pa);
}

static inline paddr_t
kva_to_phys(const void *va)
{
        return (paddr_t)(uintptr_t)va - dmap_base;
}

#endif /* _MACHINE_VMPARAM_H_ */
