/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The page cache. See <vm/pvn.h> for the design and the locking.
 *
 * Waiting for a busy page uses one of a small array of sync variables,
 * chosen by the page's address, with the vnode's pc_lock as the
 * interlock. Waiting for recyclers to finish with a vnode uses one sync
 * variable for the whole system, since it is rare.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include <machine/param.h>

#include <vm/page.h>
#include <vm/pvn.h>

#define PVN_NSV         32
#define PVN_MAXBUCKETS  65536
#define PVN_LOAD        2       /* pages per chain before growing */

static struct sv pvn_sv[PVN_NSV];
static struct spinlock pvn_ref_lock = SPINLOCK_INITIALIZER("pvn_ref");
static struct sv pvn_ref_sv;
static struct pvn_stats pvn_st;

static struct sv *page_sv(const struct page *);
static struct page *pc_find(struct vpcache *, uint64_t);
static void pc_insert(struct vpcache *, struct page *);
static void pc_unhash(struct vpcache *, struct page *);
static void pc_dirty_push(struct vnode *, struct page *);
static void pc_dirty_unlink(struct vnode *, struct page *);
static int page_on_cache_list(const struct vpcache *, const struct page *);
static void pvn_grow(struct vnode *);
static void pvn_wait_recycle(struct vnode *);

static struct sv *
page_sv(const struct page *pp)
{
        return &pvn_sv[((uintptr_t)pp / sizeof(*pp)) % PVN_NSV];
}

void
pvn_sysinit(void)
{
        for (int i = 0; i < PVN_NSV; i++)
                sv_init(&pvn_sv[i], "page");
        sv_init(&pvn_ref_sv, "pvn_ref");
}

/* Give vp an empty page cache. */
void
pvn_init(struct vnode *vp, uint32_t flags)
{
        struct vpcache *pc = &vp->v_pc;

        spinlock_init(&pc->pc_lock, "pcache");
        pc->pc_hash0 = NULL;
        pc->pc_hash = &pc->pc_hash0;
        pc->pc_nbuckets = 1;
        pc->pc_npages = 0;
        pc->pc_ndirty = 0;
        pc->pc_flags = flags;
        pc->pc_dhead = pc->pc_dtail = NULL;
        pc->pc_dirtytime = 0;
        pc->pc_ref = 0;
}

/*
 * Empty vp's page cache for good: free every page, wait for recyclers
 * to finish with it, and free the hash. The caller makes sure nobody
 * can look pages up any more. pvn_init() makes it usable again.
 */
void
pvn_destroy(struct vnode *vp)
{
        struct vpcache *pc = &vp->v_pc;

        (void)pvn_trunc(vp, 0);
        pvn_wait_recycle(vp);
        if (pc->pc_hash != &pc->pc_hash0)
                kmem_free(pc->pc_hash, pc->pc_nbuckets * sizeof(*pc->pc_hash));
        pc->pc_hash0 = NULL;
        pc->pc_hash = &pc->pc_hash0;
        pc->pc_nbuckets = 1;
}

static struct page *
pc_find(struct vpcache *pc, uint64_t idx)
{
        struct page *pp = pc->pc_hash[idx & (pc->pc_nbuckets - 1)];

        while (pp != NULL && pp->p_index != idx)
                pp = pp->p_hnext;
        return pp;
}

static void
pc_insert(struct vpcache *pc, struct page *pp)
{
        struct page **chain = &pc->pc_hash[pp->p_index &
            (pc->pc_nbuckets - 1)];

        pp->p_hnext = *chain;
        *chain = pp;
        pc->pc_npages++;
}

/* Take pp out of the hash and clear its identity. pc_lock is held. */
static void
pc_unhash(struct vpcache *pc, struct page *pp)
{
        struct page **link = &pc->pc_hash[pp->p_index &
            (pc->pc_nbuckets - 1)];

        while (*link != pp) {
                if (*link == NULL)
                        panic("pc_unhash: page %#jx is not in its vnode",
                            (uintmax_t)page_to_pa(pp));
                link = &(*link)->p_hnext;
        }
        *link = pp->p_hnext;
        pp->p_hnext = NULL;
        pp->p_vnode = NULL;
        pp->p_vflags = 0;
        pc->pc_npages--;
}

/*
 * The dirty list reuses the free-list links, which are the owner's while
 * the page is allocated. pc_lock is held.
 */
static void
pc_dirty_push(struct vnode *vp, struct page *pp)
{
        struct vpcache *pc = &vp->v_pc;

        pp->p_vflags |= P_DIRTY;
        pp->p_next = NULL;
        pp->p_prev = pc->pc_dtail;
        if (pc->pc_dtail != NULL)
                pc->pc_dtail->p_next = pp;
        else
                pc->pc_dhead = pp;
        pc->pc_dtail = pp;
        if (pc->pc_ndirty++ == 0) {
                pc->pc_dirtytime = lbolt;
                vn_dirty_add(vp);
        }
}

static void
pc_dirty_unlink(struct vnode *vp, struct page *pp)
{
        struct vpcache *pc = &vp->v_pc;

        if (pp->p_prev != NULL)
                pp->p_prev->p_next = pp->p_next;
        else
                pc->pc_dhead = pp->p_next;
        if (pp->p_next != NULL)
                pp->p_next->p_prev = pp->p_prev;
        else
                pc->pc_dtail = pp->p_prev;
        pp->p_next = pp->p_prev = NULL;
        pp->p_vflags &= (uint16_t)~P_DIRTY;
        if (--pc->pc_ndirty == 0)
                vn_dirty_remove(vp);
}

/* Whether a hashed page sits on its node's cache list. pc_lock is held. */
static int
page_on_cache_list(const struct vpcache *pc, const struct page *pp)
{
        return pp->p_use == 0 && (pp->p_vflags & (P_BUSY | P_DIRTY)) == 0 &&
            (pc->pc_flags & PC_KEEP) == 0;
}

/*
 * Return page idx of vp, held, or NULL if it is not cached. Waits while
 * the page is busy.
 */
struct page *
page_lookup(struct vnode *vp, uint64_t idx)
{
        struct vpcache *pc = &vp->v_pc;

        for (;;) {
                int s = spin_lock(&pc->pc_lock);
                struct page *pp = pc_find(pc, idx);

                if (pp == NULL) {
                        spin_unlock(&pc->pc_lock, s);
                        atomic_add_64(&pvn_st.pv_misses, 1);
                        return NULL;
                }
                if ((pp->p_vflags & P_BUSY) != 0) {
                        pp->p_vflags |= P_WANTED;
                        sv_wait(page_sv(pp), &pc->pc_lock, s);
                        continue;
                }
                if (page_on_cache_list(pc, pp)) {
                        if (!page_cache_take(pp)) {
                                /* Being recycled; it will be gone soon. */
                                spin_unlock(&pc->pc_lock, s);
                                cpu_spinwait();
                                continue;
                        }
                        atomic_add_64(&pvn_st.pv_reclaims, 1);
                }
                pp->p_use++;
                spin_unlock(&pc->pc_lock, s);
                atomic_add_64(&pvn_st.pv_hits, 1);
                return pp;
        }
}

/*
 * Return page idx of vp, held. If it was not cached, a new page is
 * entered busy, with undefined contents, and *newp is set; the caller
 * fills it and calls page_unbusy() or page_abort(). Returns NULL if no
 * memory is free.
 */
struct page *
page_lookup_create(struct vnode *vp, uint64_t idx, int *newp)
{
        struct vpcache *pc = &vp->v_pc;

        for (;;) {
                struct page *pp, *np;
                int s, grow;

                if ((pp = page_lookup(vp, idx)) != NULL) {
                        *newp = 0;
                        return pp;
                }
                if ((np = page_alloc(0, 0)) == NULL)
                        return NULL;
                s = spin_lock(&pc->pc_lock);
                if (pc_find(pc, idx) != NULL) {
                        /* Someone else entered it meanwhile. */
                        spin_unlock(&pc->pc_lock, s);
                        page_free(np, 0);
                        continue;
                }
                np->p_vnode = vp;
                np->p_index = idx;
                np->p_vflags = P_HASH | P_BUSY;
                np->p_use = 1;
                pc_insert(pc, np);
                grow = pc->pc_npages > PVN_LOAD * pc->pc_nbuckets &&
                    pc->pc_nbuckets < PVN_MAXBUCKETS;
                spin_unlock(&pc->pc_lock, s);
                atomic_add_64(&pvn_st.pv_creates, 1);
                if (grow)
                        pvn_grow(vp);
                *newp = 1;
                return np;
        }
}

/* Double the hash of vp; allocates, so no lock is held on entry. */
static void
pvn_grow(struct vnode *vp)
{
        struct vpcache *pc = &vp->v_pc;
        uint32_t n = pc->pc_nbuckets < 16 ? 16 : pc->pc_nbuckets * 2;
        struct page **nh = kmem_zalloc(n * sizeof(*nh), KM_NOSLEEP);
        struct page **oh;
        uint32_t on;
        int s;

        if (nh == NULL)
                return;
        s = spin_lock(&pc->pc_lock);
        if (pc->pc_nbuckets >= n) {
                spin_unlock(&pc->pc_lock, s);
                kmem_free(nh, n * sizeof(*nh));
                return;
        }
        oh = pc->pc_hash;
        on = pc->pc_nbuckets;
        for (uint32_t b = 0; b < on; b++) {
                while (oh[b] != NULL) {
                        struct page *pp = oh[b];

                        oh[b] = pp->p_hnext;
                        pp->p_hnext = nh[pp->p_index & (n - 1)];
                        nh[pp->p_index & (n - 1)] = pp;
                }
        }
        pc->pc_hash = nh;
        pc->pc_nbuckets = n;
        spin_unlock(&pc->pc_lock, s);
        if (oh != &pc->pc_hash0)
                kmem_free(oh, on * sizeof(*oh));
}

/* The owner is done filling or writing pp; wake whoever waits. */
void
page_unbusy(struct page *pp)
{
        struct vpcache *pc = &pp->p_vnode->v_pc;
        int s = spin_lock(&pc->pc_lock);
        int wanted = (pp->p_vflags & P_WANTED) != 0;

        pp->p_vflags &= (uint16_t)~(P_BUSY | P_WANTED);
        spin_unlock(&pc->pc_lock, s);
        if (wanted)
                sv_broadcast(page_sv(pp));
}

/*
 * A new page could not be filled: take it out of the cache and free it.
 * The caller holds it busy, as page_lookup_create() returned it.
 */
void
page_abort(struct page *pp)
{
        struct vpcache *pc = &pp->p_vnode->v_pc;
        int s = spin_lock(&pc->pc_lock);
        int wanted = (pp->p_vflags & P_WANTED) != 0;

        if (pp->p_use != 1 || (pp->p_vflags & P_BUSY) == 0)
                panic("page_abort: page %#jx is not a new busy page",
                    (uintmax_t)page_to_pa(pp));
        pc_unhash(pc, pp);
        pp->p_use = 0;
        spin_unlock(&pc->pc_lock, s);
        if (wanted)
                sv_broadcast(page_sv(pp));
        page_free(pp, 0);
}

/* Drop a hold. A clean page nobody holds goes on the cache list. */
void
page_rele(struct page *pp)
{
        struct vpcache *pc = &pp->p_vnode->v_pc;
        int s = spin_lock(&pc->pc_lock);

        if (pp->p_use == 0)
                panic("page_rele: page %#jx is not held",
                    (uintmax_t)page_to_pa(pp));
        pp->p_use--;
        if (page_on_cache_list(pc, pp))
                page_cache_enter(pp);
        spin_unlock(&pc->pc_lock, s);
}

/* Mark a held page newer than its backing store. */
void
page_setdirty(struct page *pp)
{
        struct vnode *vp = pp->p_vnode;
        int s = spin_lock(&vp->v_pc.pc_lock);

        if (pp->p_use == 0)
                panic("page_setdirty: page %#jx is not held",
                    (uintmax_t)page_to_pa(pp));
        if ((vp->v_pc.pc_flags & PC_KEEP) != 0)
                panic("page_setdirty: vnode %p keeps its pages", (void *)vp);
        if ((pp->p_vflags & P_DIRTY) == 0)
                pc_dirty_push(vp, pp);
        spin_unlock(&vp->v_pc.pc_lock, s);
}

/*
 * Free every page of vp from page from on, dirty or not, waiting for
 * busy ones. Nobody may hold them: the caller excludes readers and
 * writers. Returns the number freed.
 */
uint64_t
pvn_trunc(struct vnode *vp, uint64_t from)
{
        struct vpcache *pc = &vp->v_pc;
        uint64_t n = 0;
        int s;

restart:
        s = spin_lock(&pc->pc_lock);
        for (uint32_t b = 0; b < pc->pc_nbuckets; b++) {
                struct page **link = &pc->pc_hash[b];

                while (*link != NULL) {
                        struct page *pp = *link;

                        if (pp->p_index < from) {
                                link = &pp->p_hnext;
                                continue;
                        }
                        if ((pp->p_vflags & P_BUSY) != 0) {
                                pp->p_vflags |= P_WANTED;
                                sv_wait(page_sv(pp), &pc->pc_lock, s);
                                goto restart;
                        }
                        if (page_on_cache_list(pc, pp) &&
                            !page_cache_take(pp)) {
                                spin_unlock(&pc->pc_lock, s);
                                cpu_spinwait();
                                goto restart;
                        }
                        if (pp->p_use != 0)
                                panic("pvn_trunc: page %ju of vnode %p is "
                                    "held", (uintmax_t)pp->p_index,
                                    (void *)vp);
                        if ((pp->p_vflags & P_DIRTY) != 0)
                                pc_dirty_unlink(vp, pp);
                        pc_unhash(pc, pp);
                        page_free(pp, 0);
                        n++;
                }
        }
        spin_unlock(&pc->pc_lock, s);
        if (n != 0)
                atomic_add_64(&pvn_st.pv_tossed, n);
        return n;
}

/*
 * Take the oldest dirty page of vp for writing back: it is returned
 * held, busy, and no longer dirty. The caller writes it and calls
 * page_write_done(). NULL when vp has no dirty pages left.
 */
struct page *
pvn_getdirty(struct vnode *vp)
{
        struct vpcache *pc = &vp->v_pc;
        int s = spin_lock(&pc->pc_lock);
        struct page *pp = pc->pc_dhead;

        if (pp != NULL) {
                pc_dirty_unlink(vp, pp);
                pp->p_vflags |= P_BUSY;
                pp->p_use++;
                atomic_add_64(&pvn_st.pv_cleaned, 1);
        }
        spin_unlock(&pc->pc_lock, s);
        return pp;
}

/* A page from pvn_getdirty() was written; dirty again if it failed. */
void
page_write_done(struct page *pp, int error)
{
        struct vnode *vp = pp->p_vnode;

        if (error != 0) {
                int s = spin_lock(&vp->v_pc.pc_lock);

                if ((pp->p_vflags & P_DIRTY) == 0)
                        pc_dirty_push(vp, pp);
                spin_unlock(&vp->v_pc.pc_lock, s);
        }
        page_unbusy(pp);
        page_rele(pp);
}

uint32_t
pvn_npages(struct vnode *vp)
{
        return vp->v_pc.pc_npages;
}

uint32_t
pvn_ndirty(struct vnode *vp)
{
        return vp->v_pc.pc_ndirty;
}

/* Under the node lock, while taking a cache page of vp to recycle. */
void
pvn_recycle_hold(struct vnode *vp)
{
        (void)atomic_inc_32_nv(&vp->v_pc.pc_ref);
}

/*
 * Finish recycling pp, which the allocator has taken off its cache list:
 * take it out of its vnode and drop the recycler's hold. Afterwards the
 * page is the allocator's.
 */
void
pvn_recycle(struct page *pp)
{
        struct vnode *vp = pp->p_vnode;
        struct vpcache *pc = &vp->v_pc;
        int s = spin_lock(&pc->pc_lock);

        if (pp->p_use != 0 || (pp->p_vflags & (P_BUSY | P_DIRTY)) != 0)
                panic("pvn_recycle: page %#jx is in use",
                    (uintmax_t)page_to_pa(pp));
        pc_unhash(pc, pp);
        spin_unlock(&pc->pc_lock, s);
        atomic_add_64(&pvn_st.pv_recycled, 1);

        s = spin_lock(&pvn_ref_lock);
        if (atomic_dec_32_nv(&pc->pc_ref) == 0)
                sv_broadcast(&pvn_ref_sv);
        spin_unlock(&pvn_ref_lock, s);
}

/* Wait until no recycler is working on vp's cache. */
static void
pvn_wait_recycle(struct vnode *vp)
{
        int s = spin_lock(&pvn_ref_lock);

        while (vp->v_pc.pc_ref != 0) {
                sv_wait(&pvn_ref_sv, &pvn_ref_lock, s);
                s = spin_lock(&pvn_ref_lock);
        }
        spin_unlock(&pvn_ref_lock, s);
}

void
pvn_get_stats(struct pvn_stats *st)
{
        *st = pvn_st;
}
