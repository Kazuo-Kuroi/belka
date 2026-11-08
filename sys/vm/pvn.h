/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The page cache: file pages, named by a vnode and a page number in it.
 *
 * Each vnode indexes its own pages in a hash that grows with the file
 * (IRIX keeps file pages per vnode, not in one global hash). A page is
 * held while someone uses it (p_use). When the last hold goes and the
 * page is clean, it stays in its vnode's cache but goes on its node's
 * cache list, where a later lookup finds it again with no I/O, or the
 * allocator recycles it when memory is short. Dirty pages stay off the
 * cache list, on their vnode's dirty list, until they are written back.
 *
 * A page is busy (P_BUSY) while its owner fills it from backing store or
 * writes it back; lookups wait for that to finish. A new page from
 * page_lookup_create() is returned busy, and the caller fills it and
 * calls page_unbusy(), or page_abort() if it could not.
 *
 * Locking: pc_lock, one per vnode, protects the hash, the dirty list,
 * and the cache fields of the vnode's pages. It is taken before a node
 * lock. Nothing is allocated while it is held, because the allocator
 * may need the pc_lock of another vnode to recycle one of its pages.
 *
 * Recycling goes the other way round: under the node lock, the
 * allocator marks a cache page PG_RECYCLE and adds a hold on its
 * vnode's cache (pc_ref); it then drops the node lock, unhashes the
 * page under pc_lock, and drops the hold. A lookup that meets a page
 * being recycled waits for it to leave. Whoever tears a vnode's cache
 * down waits for pc_ref to drain (pvn_destroy).
 */

#ifndef _VM_PVN_H_
#define _VM_PVN_H_

#include <stdint.h>

#include <sys/spinlock.h>

struct page;
struct vnode;

struct vpcache {
        struct spinlock pc_lock;
        struct page     **pc_hash;      /* pc_nbuckets chains */
        struct page     *pc_hash0;      /* the only chain, at first */
        uint32_t        pc_nbuckets;    /* a power of two */
        uint32_t        pc_npages;      /* pages in the cache */
        uint32_t        pc_ndirty;      /* pages on the dirty list */
        uint32_t        pc_flags;       /* PC_* */
        struct page     *pc_dhead;      /* dirty list, oldest first */
        struct page     *pc_dtail;
        uint64_t        pc_dirtytime;   /* lbolt when it became dirty */
        volatile uint32_t pc_ref;       /* recyclers at work */
};

/* pc_flags */
#define PC_KEEP         0x0001  /* pages never go on the cache list */

struct pvn_stats {
        uint64_t        pv_hits;        /* lookups that found the page */
        uint64_t        pv_misses;
        uint64_t        pv_reclaims;    /* hits taken off a cache list */
        uint64_t        pv_creates;
        uint64_t        pv_tossed;      /* pages freed by truncation */
        uint64_t        pv_recycled;    /* pages taken by the allocator */
        uint64_t        pv_cleaned;     /* dirty pages handed to writers */
};

void pvn_sysinit(void);
void pvn_init(struct vnode *, uint32_t);
void pvn_destroy(struct vnode *);

struct page *page_lookup(struct vnode *, uint64_t);
struct page *page_lookup_create(struct vnode *, uint64_t, int *);
void page_unbusy(struct page *);
void page_abort(struct page *);
void page_rele(struct page *);
void page_setdirty(struct page *);

uint64_t pvn_trunc(struct vnode *, uint64_t);
struct page *pvn_getdirty(struct vnode *);
void page_write_done(struct page *, int);
uint32_t pvn_npages(struct vnode *);
uint32_t pvn_ndirty(struct vnode *);

/* For the page allocator. */
void pvn_recycle_hold(struct vnode *);
void pvn_recycle(struct page *);

void pvn_get_stats(struct pvn_stats *);

#endif /* _VM_PVN_H_ */
