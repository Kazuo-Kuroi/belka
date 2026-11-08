/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel virtual memory layout.
 *
 *   dmap_base .. +DMAP_MAXSIZE         direct map of physical memory
 *   KVA_BASE .. KVA_END                mappings made at run time
 *   KERNBASE ..                        the kernel image
 *
 * The lower half of the address space is for user processes and is
 * empty in the kernel's page tables, so a stray low address faults.
 *
 * The direct map keeps the boot loader's base, so that pointers into it
 * made before the kernel's page tables existed stay valid afterwards.
 * Only RAM is direct-mapped, write-back; device memory is mapped in KVA
 * with the cache attributes it needs.
 */

#ifndef _MACHINE_VMPARAM_H_
#define _MACHINE_VMPARAM_H_

#include <stdint.h>

#include <machine/types.h>

#define VM_KERNEL_LOW   UINT64_C(0xffff800000000000)    /* kernel half */
#define DMAP_MAXSIZE    UINT64_C(0x0000400000000000)    /* 64 TB */
#define KVA_BASE        UINT64_C(0xffffc00000000000)
#define KVA_END         UINT64_C(0xffffc08000000000)    /* 512 GB */
#define KERNBASE        UINT64_C(0xffffffff80000000)

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
