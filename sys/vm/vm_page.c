/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Physical page allocator. See <vm/page.h> for the design.
 *
 * Physical memory is described as segments: maximal runs of usable page
 * frames, each belonging to one node. One array of struct page covers
 * every segment, and each segment points at its slice. A free block
 * never crosses a segment boundary, so the pages of a block are always
 * adjacent in the array.
 *
 * Each node also has a cache list of clean page-cache pages that nobody
 * holds, oldest at the head. When the buddy lists cannot satisfy a
 * request, the allocator recycles cache pages from the head, one at a
 * time, back to the buddy lists and tries again.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/page.h>
#include <vm/pvn.h>

struct page_seg {
        pfn_t           ps_start;       /* first frame */
        pfn_t           ps_end;         /* one past the last frame */
        struct page     *ps_pages;      /* struct page for ps_start */
        int             ps_node;
        int             ps_flags;       /* PSEG_* */
};

struct page_node {
        struct spinlock pn_lock;
        struct page     *pn_free[PAGE_NORDERS];
        size_t          pn_nblocks[PAGE_NORDERS];
        size_t          pn_free_pages;
        size_t          pn_managed;
        struct page     *pn_cache_head; /* oldest first */
        struct page     *pn_cache_tail;
        size_t          pn_ncache;
        uint64_t        pn_recycled;
};

static struct page_seg page_segs[PAGE_MAXSEGS];
static int page_nsegs;
static int page_segs_dropped;
static struct page_node page_nodes[MAXNODES];
static int page_ready;

static struct page_seg *page_seg_find(pfn_t);
static void page_list_insert(struct page_node *, struct page *, unsigned int);
static void page_list_remove(struct page_node *, struct page *, unsigned int);
static void page_seg_carve(struct page_seg *);
static int page_recycle_one(pfn_t);
static struct page *page_alloc_buddy(int, unsigned int, pfn_t);

static struct page_seg *
page_seg_find(pfn_t pfn)
{
        for (int i = 0; i < page_nsegs; i++) {
                struct page_seg *ps = &page_segs[i];

                if (pfn >= ps->ps_start && pfn < ps->ps_end)
                        return ps;
        }
        return NULL;
}

/* Put the block headed by pp on the free list for its order. */
static void
page_list_insert(struct page_node *pn, struct page *pp, unsigned int order)
{
        pp->p_flags = PG_FREE;
        pp->p_order = (uint8_t)order;
        pp->p_prev = NULL;
        pp->p_next = pn->pn_free[order];
        if (pp->p_next != NULL)
                pp->p_next->p_prev = pp;
        pn->pn_free[order] = pp;
        pn->pn_nblocks[order]++;
        pn->pn_free_pages += (size_t)1 << order;
}

static void
page_list_remove(struct page_node *pn, struct page *pp, unsigned int order)
{
        if (pp->p_prev != NULL)
                pp->p_prev->p_next = pp->p_next;
        else
                pn->pn_free[order] = pp->p_next;
        if (pp->p_next != NULL)
                pp->p_next->p_prev = pp->p_prev;
        pp->p_next = NULL;
        pp->p_prev = NULL;
        pp->p_flags = 0;
        pn->pn_nblocks[order]--;
        pn->pn_free_pages -= (size_t)1 << order;
}

/*
 * Record that physical memory [start, end) is RAM belonging to node.
 * Partial pages at either end are dropped, as is page 0, so that a zero
 * physical address is never handed out. A range adjacent to an existing
 * segment of the same node and flags extends it.
 *
 * With PSEG_DEFER the range gets struct pages but stays off the free
 * lists until page_release_deferred(), for memory that is still in use
 * during start-up.
 */
void
page_seg_add(paddr_t start, paddr_t end, int node, int flags)
{
        pfn_t first = btopr(start);
        pfn_t last = btop(end);
        int i;

        if (page_ready)
                panic("page_seg_add: called after page_init");
        if (node < 0 || node >= MAXNODES)
                panic("page_seg_add: bad node %d", node);
        if (first == 0)
                first = 1;
        if (first >= last)
                return;

        for (i = 0; i < page_nsegs; i++) {
                struct page_seg *ps = &page_segs[i];

                if (ps->ps_node != node || ps->ps_flags != flags)
                        continue;
                if (ps->ps_end == first) {
                        ps->ps_end = last;
                        return;
                }
                if (ps->ps_start == last) {
                        ps->ps_start = first;
                        return;
                }
        }

        if (page_nsegs == PAGE_MAXSEGS) {
                page_segs_dropped++;
                return;
        }

        /* Keep the segments sorted by address. */
        for (i = page_nsegs; i > 0 && page_segs[i - 1].ps_start > first; i--)
                page_segs[i] = page_segs[i - 1];
        page_segs[i].ps_start = first;
        page_segs[i].ps_end = last;
        page_segs[i].ps_pages = NULL;
        page_segs[i].ps_node = node;
        page_segs[i].ps_flags = flags;
        page_nsegs++;
}

/* Hand a segment's frames to the free lists as maximal aligned blocks. */
static void
page_seg_carve(struct page_seg *ps)
{
        struct page_node *pn = &page_nodes[ps->ps_node];
        pfn_t pfn = ps->ps_start;

        while (pfn < ps->ps_end) {
                unsigned int order = PAGE_NORDERS - 1;

                while (order > 0 &&
                    ((pfn & (((pfn_t)1 << order) - 1)) != 0 ||
                    pfn + ((pfn_t)1 << order) > ps->ps_end))
                        order--;
                page_list_insert(pn, &ps->ps_pages[pfn - ps->ps_start],
                    order);
                pfn += (pfn_t)1 << order;
        }
}

/*
 * Build the page array and the free lists from the registered segments.
 * The array is taken from the end of the largest segment.
 */
void
page_init(void)
{
        struct page_seg *host = NULL;
        struct page *array;
        size_t total = 0;
        size_t offset = 0;
        pfn_t need;

        if (page_nsegs == 0)
                panic("page_init: no usable memory");
        if (page_segs_dropped != 0)
                printf("page_init: %d memory ranges ignored\n",
                    page_segs_dropped);
        for (int n = 0; n < MAXNODES; n++)
                spinlock_init(&page_nodes[n].pn_lock, "page_node");

        for (int i = 0; i < page_nsegs; i++) {
                struct page_seg *ps = &page_segs[i];

                total += ps->ps_end - ps->ps_start;
                if ((ps->ps_flags & PSEG_DEFER) != 0)
                        continue;
                if (host == NULL ||
                    ps->ps_end - ps->ps_start > host->ps_end - host->ps_start)
                        host = ps;
        }

        need = btopr(total * sizeof(struct page));
        if (host == NULL || host->ps_end - host->ps_start <= need)
                panic("page_init: no segment can hold the page array");
        host->ps_end -= need;
        array = phys_to_kva(ptob(host->ps_end));

        for (int i = 0; i < page_nsegs; i++) {
                struct page_seg *ps = &page_segs[i];
                size_t npages = ps->ps_end - ps->ps_start;

                ps->ps_pages = &array[offset];
                for (size_t j = 0; j < npages; j++) {
                        struct page *pp = &ps->ps_pages[j];

                        memset(pp, 0, sizeof(*pp));
                        pp->p_pfn = ps->ps_start + j;
                        pp->p_node = (uint8_t)ps->ps_node;
                }
                offset += npages;
                if ((ps->ps_flags & PSEG_DEFER) != 0)
                        continue;
                page_nodes[ps->ps_node].pn_managed += npages;
                page_seg_carve(ps);
        }

        page_ready = 1;
}

/*
 * Put every deferred segment on the free lists. Returns the number of
 * pages released. The caller guarantees that nothing uses them any more.
 */
size_t
page_release_deferred(void)
{
        size_t released = 0;

        for (int i = 0; i < page_nsegs; i++) {
                struct page_seg *ps = &page_segs[i];
                struct page_node *pn = &page_nodes[ps->ps_node];
                size_t npages;
                int s;

                if ((ps->ps_flags & PSEG_DEFER) == 0)
                        continue;
                npages = ps->ps_end - ps->ps_start;
                s = spin_lock(&pn->pn_lock);
                ps->ps_flags &= ~PSEG_DEFER;
                pn->pn_managed += npages;
                page_seg_carve(ps);
                spin_unlock(&pn->pn_lock, s);
                released += npages;
        }
        return released;
}

/*
 * Put a page-cache page that nobody holds at the tail of its node's
 * cache list. The owning vnode's page-cache lock is held.
 */
void
page_cache_enter(struct page *pp)
{
        struct page_node *pn = &page_nodes[pp->p_node];
        int s = spin_lock(&pn->pn_lock);

        if (pp->p_flags != PG_ALLOC || pp->p_order != 0)
                panic("page_cache_enter: page %#jx is not an allocated page",
                    (uintmax_t)page_to_pa(pp));
        pp->p_flags = PG_CACHE;
        pp->p_next = NULL;
        pp->p_prev = pn->pn_cache_tail;
        if (pn->pn_cache_tail != NULL)
                pn->pn_cache_tail->p_next = pp;
        else
                pn->pn_cache_head = pp;
        pn->pn_cache_tail = pp;
        pn->pn_ncache++;
        spin_unlock(&pn->pn_lock, s);
}

/*
 * Take a page back off its node's cache list, for its owner. Returns 0
 * if the allocator is recycling it; the owner must then let go of its
 * lock and wait for the page to leave its cache. The owning vnode's
 * page-cache lock is held.
 */
int
page_cache_take(struct page *pp)
{
        struct page_node *pn = &page_nodes[pp->p_node];
        int s = spin_lock(&pn->pn_lock);

        if (pp->p_flags != PG_CACHE) {
                spin_unlock(&pn->pn_lock, s);
                return 0;
        }
        if (pp->p_prev != NULL)
                pp->p_prev->p_next = pp->p_next;
        else
                pn->pn_cache_head = pp->p_next;
        if (pp->p_next != NULL)
                pp->p_next->p_prev = pp->p_prev;
        else
                pn->pn_cache_tail = pp->p_prev;
        pp->p_next = pp->p_prev = NULL;
        pp->p_flags = PG_ALLOC;
        pn->pn_ncache--;
        spin_unlock(&pn->pn_lock, s);
        return 1;
}

/*
 * Recycle the oldest cache page of any node back to the buddy lists.
 * Returns 0 if every cache list is empty. The page is marked
 * PG_RECYCLE and its vnode's cache held under the node lock; it is
 * then unhashed under the vnode's lock, after the node lock is gone.
 */
static int
page_recycle_one(pfn_t limit)
{
        for (int n = 0; n < MAXNODES; n++) {
                struct page_node *pn = &page_nodes[n];
                struct page *pp;
                int s;

                if (pn->pn_ncache == 0)
                        continue;
                s = spin_lock(&pn->pn_lock);
                for (pp = pn->pn_cache_head; pp != NULL; pp = pp->p_next)
                        if (pp->p_pfn < limit)
                                break;
                if (pp == NULL) {
                        spin_unlock(&pn->pn_lock, s);
                        continue;
                }
                if (pp->p_prev != NULL)
                        pp->p_prev->p_next = pp->p_next;
                else
                        pn->pn_cache_head = pp->p_next;
                if (pp->p_next != NULL)
                        pp->p_next->p_prev = pp->p_prev;
                else
                        pn->pn_cache_tail = pp->p_prev;
                pp->p_next = pp->p_prev = NULL;
                pp->p_flags = PG_RECYCLE;
                pn->pn_ncache--;
                pn->pn_recycled++;
                pvn_recycle_hold(pp->p_vnode);
                /*
                 * Interrupts stay off until the page is unhashed: a lookup
                 * that meets it spins until then, and must never spin on
                 * a recycler preempted on its own processor.
                 */
                spin_unlock(&pn->pn_lock, 0);
                pvn_recycle(pp);
                (void)spin_lock(&pn->pn_lock);
                pp->p_flags = PG_ALLOC;
                pp->p_order = 0;
                spin_unlock(&pn->pn_lock, s);
                page_free(pp, 0);
                return 1;
        }
        return 0;
}

/*
 * Allocate a block of 2^order pages, preferring node. Returns the
 * struct page of the first page, or NULL if no block is available even
 * after recycling the page cache. Never sleeps.
 */
struct page *
page_alloc_node(int node, unsigned int order, int flags)
{
        struct page *pp;
        pfn_t limit = (flags & PA_DMA32) != 0 ?
            btop(UINT64_C(1) << 32) : (pfn_t)-1;

        if (!page_ready)
                panic("page_alloc: called before page_init");
        if (order >= PAGE_NORDERS)
                return NULL;
        if (node < 0 || node >= MAXNODES)
                node = 0;

        while ((pp = page_alloc_buddy(node, order, limit)) == NULL) {
                if (!page_recycle_one(limit))
                        return NULL;
        }
        pp->p_vnode = NULL;
        pp->p_vflags = 0;
        pp->p_use = 0;
        pp->p_hnext = NULL;
        if ((flags & PA_ZERO) != 0)
                memset(page_to_kva(pp), 0, (size_t)PAGESIZE << order);
        return pp;
}

/* Take a block from the buddy lists, preferring node, or NULL. */
static struct page *
page_alloc_buddy(int node, unsigned int order, pfn_t limit)
{
        struct page *pp = NULL;

        /* Try the preferred node, then the others in order. */
        for (int i = 0; i < MAXNODES && pp == NULL; i++) {
                struct page_node *pn = &page_nodes[(node + i) % MAXNODES];
                unsigned int o;
                int s;

                if (pn->pn_managed == 0)
                        continue;
                s = spin_lock(&pn->pn_lock);
                for (o = order; o < PAGE_NORDERS; o++) {
                        for (pp = pn->pn_free[o]; pp != NULL; pp = pp->p_next)
                                if (pp->p_pfn < limit)
                                        break;
                        if (pp != NULL)
                                break;
                }
                /* The 4 GiB boundary is aligned to every buddy order. */
                if (pp != NULL) {
                        page_list_remove(pn, pp, o);

                        /* Split, returning upper halves to the lists. */
                        while (o > order) {
                                o--;
                                page_list_insert(pn, pp + ((size_t)1 << o),
                                    o);
                        }
                        pp->p_flags = PG_ALLOC;
                        pp->p_order = (uint8_t)order;
                        pp->p_private = NULL;
                }
                spin_unlock(&pn->pn_lock, s);
        }
        return pp;
}

struct page *
page_alloc(unsigned int order, int flags)
{
        return page_alloc_node(0, order, flags);
}

/*
 * Free a block obtained from page_alloc(), merging it with its free
 * buddies. order must be the order it was allocated with.
 */
void
page_free(struct page *pp, unsigned int order)
{
        struct page_node *pn;
        struct page_seg *ps;
        pfn_t pfn;
        int s;

        pn = &page_nodes[pp->p_node];
        s = spin_lock(&pn->pn_lock);
        if ((pp->p_flags & PG_ALLOC) == 0)
                panic("page_free: page %#jx is not allocated",
                    (uintmax_t)page_to_pa(pp));
        if (pp->p_order != order)
                panic("page_free: page %#jx freed with order %u, "
                    "allocated with %u", (uintmax_t)page_to_pa(pp),
                    order, pp->p_order);
        if (pp->p_vnode != NULL)
                panic("page_free: page %#jx still belongs to a vnode",
                    (uintmax_t)page_to_pa(pp));

        ps = page_seg_find(pp->p_pfn);
        pfn = pp->p_pfn;
        pp->p_flags = 0;
        pp->p_private = NULL;

        while (order < PAGE_NORDERS - 1) {
                pfn_t buddy_pfn = pfn ^ ((pfn_t)1 << order);
                struct page *buddy;

                if (buddy_pfn < ps->ps_start || buddy_pfn >= ps->ps_end)
                        break;
                buddy = &ps->ps_pages[buddy_pfn - ps->ps_start];
                if ((buddy->p_flags & PG_FREE) == 0 || buddy->p_order != order)
                        break;
                page_list_remove(pn, buddy, order);
                if (buddy_pfn < pfn)
                        pfn = buddy_pfn;
                order++;
        }

        page_list_insert(pn, &ps->ps_pages[pfn - ps->ps_start], order);
        spin_unlock(&pn->pn_lock, s);
}

/* Return the struct page for a managed physical address, or NULL. */
struct page *
pa_to_page(paddr_t pa)
{
        pfn_t pfn = btop(pa);
        struct page_seg *ps = page_seg_find(pfn);

        if (ps == NULL || ps->ps_pages == NULL)
                return NULL;
        return &ps->ps_pages[pfn - ps->ps_start];
}

/* Return the direct-map address of a page. */
void *
page_to_kva(const struct page *pp)
{
        return phys_to_kva(page_to_pa(pp));
}

void
page_get_stats(struct page_stats *st)
{
        memset(st, 0, sizeof(*st));
        for (int n = 0; n < MAXNODES; n++) {
                struct page_node *pn = &page_nodes[n];
                int s;

                if (pn->pn_managed == 0)
                        continue;
                s = spin_lock(&pn->pn_lock);
                st->ps_managed += pn->pn_managed;
                st->ps_free += pn->pn_free_pages;
                st->ps_cached += pn->pn_ncache;
                st->ps_recycled += pn->pn_recycled;
                for (int o = 0; o < PAGE_NORDERS; o++)
                        st->ps_blocks[o] += pn->pn_nblocks[o];
                spin_unlock(&pn->pn_lock, s);
        }
}
