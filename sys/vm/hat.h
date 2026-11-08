/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Hardware address translation (HAT): the machine-dependent layer under
 * the virtual memory system, after the System V design.
 *
 * Only the kernel's own address space exists so far. Its upper-level
 * tables are created once at hat_init() and never change, so every
 * future user address space can share the kernel half by copying the
 * top-level entries.
 *
 * The kernel half holds three regions, given in <machine/vmparam.h>:
 * the direct map of physical memory, the kernel virtual area (KVA) for
 * mappings made at run time, and the kernel image.
 */

#ifndef _VM_HAT_H_
#define _VM_HAT_H_

#include <stddef.h>

#include <machine/types.h>

/* Attributes for hat_kenter(). */
#define HAT_READ        0x00    /* readable; always implied */
#define HAT_WRITE       0x01
#define HAT_EXEC        0x02
#define HAT_NOCACHE     0x04    /* uncached, for device registers */
#define HAT_WCOMBINE    0x08    /* write-combining, for frame buffers */

/* Build the kernel page tables and switch the bootstrap processor. */
void hat_init(void);

/* Load the kernel page tables on the calling processor. */
void hat_cpu_init(void);

/*
 * Map one page at a KVA address. Returns 0, or -1 if the address is
 * already mapped or a page table cannot be allocated.
 */
int hat_kenter(vaddr_t, paddr_t, unsigned int);

/* Remove npages of KVA mappings and flush them from every processor. */
void hat_kremove(vaddr_t, size_t);

/* Translate a kernel virtual address. Returns 0, or -1 if unmapped. */
int hat_kextract(vaddr_t, paddr_t *);

#endif /* _VM_HAT_H_ */
