/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Physical page management.
 *
 * Every managed physical page has a struct page. Free pages are kept by
 * a binary buddy allocator, one per memory node, in blocks of 2^order
 * pages aligned to their size in physical memory. Aligned blocks let
 * the same allocator supply 2 MB large pages (order 9).
 *
 * Planned, and why the structure looks the way it does:
 *
 * - Pages that hold file or anonymous data will keep their identity
 *   (object and offset) when freed, on a separate cache list, so that a
 *   later fault can reclaim them without I/O. The allocator takes cache
 *   pages only when the free lists are empty. That arrives with the VM
 *   object layer; today every free page is anonymous.
 * - Memory is divided into nodes for NUMA. There is one node until ACPI
 *   SRAT parsing exists; page_alloc_node() is already the primitive.
 * - Each node's free lists are protected by a per-node spin lock, so
 *   allocation is safe from any processor and at splhi.
 */

#ifndef _VM_PAGE_H_
#define _VM_PAGE_H_

#include <stddef.h>
#include <stdint.h>

#include <machine/param.h>
#include <machine/types.h>

#define PAGE_NORDERS    11      /* orders 0 to 10: 4 KB to 4 MB */
#define PAGE_MAXSEGS    64      /* physical memory segments */

struct page {
        struct page     *p_next;        /* free list linkage */
        struct page     *p_prev;
        pfn_t           p_pfn;          /* physical frame number */
        uint32_t        p_flags;
        uint8_t         p_order;        /* order of the block, when free */
        uint8_t         p_node;         /* memory node */
        uint16_t        p_pad;
};

/* p_flags */
#define PG_FREE         0x0001  /* first page of a free block */
#define PG_ALLOC        0x0002  /* first page of an allocated block */

/* Flags to page_alloc(). */
#define PA_ZERO         0x0001  /* fill the block with zeroes */

/* Flags to page_seg_add(). */
#define PSEG_DEFER      0x0001  /* in use until page_release_deferred() */

/* Called by machine-dependent startup, before page_init(). */
void page_seg_add(paddr_t, paddr_t, int, int);

void page_init(void);
size_t page_release_deferred(void);

struct page *page_alloc(unsigned int, int);
struct page *page_alloc_node(int, unsigned int, int);
void page_free(struct page *, unsigned int);

struct page *pa_to_page(paddr_t);
void *page_to_kva(const struct page *);

static inline paddr_t
page_to_pa(const struct page *pp)
{
        return ptob(pp->p_pfn);
}

/* Statistics, in pages. */
struct page_stats {
        size_t  ps_managed;             /* pages under the allocator */
        size_t  ps_free;                /* pages on the free lists */
        size_t  ps_blocks[PAGE_NORDERS]; /* free blocks of each order */
};

void page_get_stats(struct page_stats *);

#endif /* _VM_PAGE_H_ */
