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
 * Pages of the page cache (<vm/pvn.h>) belong to a vnode and an index
 * in it. When nobody holds such a page and it is clean, it keeps its
 * identity and goes on its node's cache list, oldest first, rather than
 * back to the buddy lists (IRIX). A later lookup takes it back with no
 * I/O. The allocator recycles cache pages, oldest first, only when the
 * buddy lists cannot satisfy a request, so free memory and the page
 * cache are one pool.
 *
 * Memory is divided into nodes for NUMA. There is one node until ACPI
 * SRAT parsing exists; page_alloc_node() is already the primitive.
 *
 * Locking: each node's spin lock (pn_lock) protects its free lists, its
 * cache list, and p_flags. The owning vnode's page-cache lock protects
 * the identity fields, p_use, and p_vflags. Recycling, which goes from
 * the node lock to the vnode lock, follows the protocol in vm_pvn.c.
 */

#ifndef _VM_PAGE_H_
#define _VM_PAGE_H_

#include <stddef.h>
#include <stdint.h>

#include <machine/param.h>
#include <machine/types.h>

#define PAGE_NORDERS    11      /* orders 0 to 10: 4 KB to 4 MB */
#define PAGE_MAXSEGS    64      /* physical memory segments */

struct vnode;

struct page {
        struct page     *p_next;        /* free or cache list linkage */
        struct page     *p_prev;
        pfn_t           p_pfn;          /* physical frame number */
        void            *p_private;     /* owner's data, while allocated */
        uint32_t        p_flags;        /* PG_*; under the node lock */
        uint8_t         p_order;        /* order of the block, when free */
        uint8_t         p_node;         /* memory node */
        uint16_t        p_vflags;       /* P_*; under the vnode's lock */

        /* Page cache identity; under the owning vnode's lock. */
        struct vnode    *p_vnode;       /* NULL if none */
        uint64_t        p_index;        /* page number in the vnode */
        struct page     *p_hnext;       /* vnode hash chain */
        uint32_t        p_use;          /* holds */
        uint32_t        p_pad;
};

/* p_flags */
#define PG_FREE         0x0001  /* first page of a free block */
#define PG_ALLOC        0x0002  /* first page of an allocated block */
#define PG_CACHE        0x0004  /* on the node's cache list */
#define PG_RECYCLE      0x0008  /* taken off it to be recycled */

/* p_vflags (see <vm/pvn.h>) */
#define P_HASH          0x0001  /* in a vnode's page cache */
#define P_BUSY          0x0002  /* being filled or written */
#define P_WANTED        0x0004  /* someone waits for P_BUSY to clear */
#define P_DIRTY         0x0008  /* newer than its backing store */

/* Flags to page_alloc(). */
#define PA_ZERO         0x0001  /* fill the block with zeroes */

/* Flags to page_seg_add(). */
#define PSEG_DEFER      0x0001  /* in use until page_release_deferred() */

/* Called by machine-dependent startup, before page_init(). */
void page_seg_add(paddr_t, paddr_t, int, int);

void page_init(void);
void page_cache_enter(struct page *);
int page_cache_take(struct page *);
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
        size_t  ps_cached;              /* pages on the cache lists */
        uint64_t ps_recycled;           /* cache pages recycled, ever */
        size_t  ps_blocks[PAGE_NORDERS]; /* free blocks of each order */
};

void page_get_stats(struct page_stats *);

#endif /* _VM_PAGE_H_ */
