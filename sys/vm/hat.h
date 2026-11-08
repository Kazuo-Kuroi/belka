/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Hardware address translation (HAT): the machine-dependent layer under
 * the virtual memory system, after the System V design.
 *
 * The kernel's upper-level tables are created once at hat_init() and
 * never change, so every user address space shares the kernel half by
 * copying the top-level entries.
 *
 * A user address space has a struct hat of its own: a PML4 whose lower
 * half it owns. The HAT treats its tables as a cache of what the
 * segment drivers decide; it never chooses pages itself. Changes are
 * made under the hat's spin lock, and translations removed or reduced
 * are flushed from every processor that has the hat loaded before the
 * call returns. Pages that mappings refer to belong to the caller.
 *
 * The kernel half holds three regions, given in <machine/vmparam.h>:
 * the direct map of physical memory, the kernel virtual area (KVA) for
 * mappings made at run time, and the kernel image.
 */

#ifndef _VM_HAT_H_
#define _VM_HAT_H_

#include <stddef.h>

#include <machine/types.h>

struct hat;
struct page;

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

/* User address spaces. */
struct hat *hat_alloc(void);
void hat_free(struct hat *);
void hat_activate(struct hat *);
int hat_memload(struct hat *, vaddr_t, paddr_t, unsigned int);
void hat_unload(struct hat *, vaddr_t, size_t,
    void (*)(void *, vaddr_t, paddr_t), void *);
void hat_chgprot(struct hat *, vaddr_t, size_t, unsigned int);
int hat_probe(struct hat *, vaddr_t, paddr_t *, unsigned int *);

#endif /* _VM_HAT_H_ */
