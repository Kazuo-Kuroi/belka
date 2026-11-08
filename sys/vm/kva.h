/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel virtual address space.
 *
 * kva_alloc() hands out page-aligned ranges of the kernel virtual area
 * above the fixed mappings; it allocates addresses only, and the caller
 * maps them with hat_kenter(). kstack_alloc() builds a kernel stack in
 * KVA with an unmapped guard page below it, so that an overflow faults
 * instead of overwriting memory.
 */

#ifndef _VM_KVA_H_
#define _VM_KVA_H_

#include <stddef.h>

#include <machine/types.h>

#define KSTACK_PAGES    4       /* 16 KB stacks */

void kva_init(void);
vaddr_t kva_alloc(size_t, int);
void kva_free(vaddr_t, size_t);

struct kva_stats {
        size_t  kvs_free_pages;
        size_t  kvs_segments;   /* free ranges */
};

void kva_get_stats(struct kva_stats *);

/*
 * Map physical [pa, pa + len) into KVA with HAT attributes attr, for
 * firmware tables and device registers that the direct map does not
 * cover. Returns the virtual address of pa itself; kva_unmap_phys()
 * takes the same address and length back.
 */
void *kva_map_phys(paddr_t, size_t, unsigned int);
void kva_unmap_phys(void *, size_t);

/* Returns the top of the stack, or 0 under KM_NOSLEEP if out of memory. */
vaddr_t kstack_alloc(int);
void kstack_free(vaddr_t);

#endif /* _VM_KVA_H_ */
