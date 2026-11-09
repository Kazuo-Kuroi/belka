/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The vnode segment driver. See <vm/seg_vn.h>.
 *
 * Every private or anonymous segment has an anon map from the start,
 * one slot per page. A fault resolves the page in this order:
 *
 * - An anon in the slot: map it. A private segment maps a shared anon
 *   (one fork has not yet copied) read-only, and a write to it copies it
 *   first.
 * - A vnode: get the page from the file (VOP_GETPAGE, which returns it
 *   held). A write to a private mapping copies it into a new anon; any
 *   other access maps the file page itself, read-only if private, and
 *   the mapping keeps the hold until it is unloaded.
 * - Neither: a zero-filled anon.
 *
 * The slot array is protected by the map's lock, held across the whole
 * fault, so faults on a private or anonymous segment are serialised.
 * Faults on a shared file mapping (no map) can race between LWPs; the
 * loser finds the page already mapped and drops its own hold. A fault
 * on a page that is mapped already (another LWP got there first, or a
 * write to a page about to become writable) keeps that translation.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/mman.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/anon.h>
#include <vm/as.h>
#include <vm/hat.h>
#include <vm/page.h>
#include <vm/pvn.h>
#include <vm/seg.h>
#include <vm/seg_vn.h>

static int segvn_dup(struct seg *, struct seg *);
static int segvn_unmap(struct seg *, vaddr_t, size_t);
static void segvn_free(struct seg *);
static int segvn_fault(struct seg *, vaddr_t, enum seg_rw);
static int segvn_setprot(struct seg *, unsigned int);
static int segvn_split(struct seg *, vaddr_t);

static const struct seg_ops segvn_ops = {
        segvn_dup, segvn_unmap, segvn_free, segvn_fault, segvn_setprot,
        segvn_split
};

static struct kmem_zone *segvn_zone;

static struct segvn_data *
segvn_data_alloc(void)
{
        if (segvn_zone == NULL)
                segvn_zone = kmem_zone_init(sizeof(struct segvn_data),
                    "segvn");
        return kmem_zone_alloc(segvn_zone, KM_SLEEP);
}

static void
segvn_vnode_hold(struct vnode *vp)
{
        if (vp != NULL) {
                VN_HOLD(vp);
                (void)atomic_inc_32_nv(&vp->v_nmap);
        }
}

static void
segvn_vnode_rele(struct vnode *vp)
{
        if (vp != NULL) {
                (void)atomic_dec_32_nv(&vp->v_nmap);
                VN_RELE(vp);
        }
}

int
segvn_create(struct seg *seg, void *argsp)
{
        const struct segvn_crargs *a = argsp;
        struct segvn_data *svd;

        if ((a->prot & ~a->maxprot) != 0)
                return EACCES;
        if (a->type != MAP_SHARED && a->type != MAP_PRIVATE)
                return EINVAL;
        if (a->vp != NULL && a->type == MAP_SHARED &&
            (a->maxprot & PROT_WRITE) != 0)
                return EOPNOTSUPP;
        svd = segvn_data_alloc();
        svd->svd_vp = a->vp;
        svd->svd_offset = a->offset;
        svd->svd_type = a->type;
        svd->svd_prot = a->prot;
        svd->svd_maxprot = a->maxprot;
        svd->svd_anon_index = 0;
        svd->svd_amp = a->vp == NULL || a->type == MAP_PRIVATE ?
            amap_alloc(btop(seg->s_size)) : NULL;
        segvn_vnode_hold(a->vp);
        seg->s_ops = &segvn_ops;
        seg->s_data = svd;
        return 0;
}

/*
 * Grow an anonymous segment upwards by len bytes (brk). a_lock is held
 * for update, and the range above must be free.
 */
int
segvn_extend(struct seg *seg, size_t len)
{
        struct segvn_data *svd = seg->s_data;
        struct anon_map *amp = svd->svd_amp;
        size_t need;

        if (svd->svd_vp != NULL || amp == NULL ||
            svd->svd_type != MAP_PRIVATE)
                return EINVAL;
        need = svd->svd_anon_index + btop(seg->s_size + len);
        mutex_lock(&amp->am_lock);
        (void)amap_grow(amp, need);
        mutex_unlock(&amp->am_lock);
        seg->s_size += len;
        return 0;
}

/*
 * hat_unload callback: a page-cache page's mapping held it. The page's
 * identity tells which kind it is, which is sound because an anon page
 * is never freed while still mapped: every path unloads a translation
 * before it releases the anon behind it.
 */
static void
segvn_unloaded(void *arg __unused, vaddr_t va __unused, paddr_t pa)
{
        struct page *pp = pa_to_page(pa);

        if (pp->p_vnode != NULL)
                page_rele(pp);
}

static void
segvn_unload(struct seg *seg, vaddr_t va, size_t npages)
{
        hat_unload(seg->s_as->a_hat, va, npages, segvn_unloaded, NULL);
}

static unsigned int
prot_attr(unsigned int prot)
{
        return ((prot & PROT_WRITE) != 0 ? HAT_WRITE : 0) |
            ((prot & PROT_EXEC) != 0 ? HAT_EXEC : 0);
}

static int
segvn_dup(struct seg *seg, struct seg *nseg)
{
        struct segvn_data *svd = seg->s_data;
        struct segvn_data *nsvd = segvn_data_alloc();

        *nsvd = *svd;
        if (svd->svd_amp != NULL) {
                if (svd->svd_type == MAP_PRIVATE) {
                        /*
                         * Share the anons, then make our own mappings of
                         * them read-only, so that whichever side writes
                         * first copies.
                         */
                        nsvd->svd_amp = amap_copy(svd->svd_amp,
                            svd->svd_anon_index, btop(seg->s_size));
                        nsvd->svd_anon_index = 0;
                        if ((svd->svd_prot & PROT_WRITE) != 0)
                                hat_chgprot(seg->s_as->a_hat, seg->s_base,
                                    btop(seg->s_size),
                                    prot_attr(svd->svd_prot & ~PROT_WRITE));
                } else {
                        amap_hold(svd->svd_amp);
                }
        }
        segvn_vnode_hold(svd->svd_vp);
        nseg->s_ops = &segvn_ops;
        nseg->s_data = nsvd;
        return 0;
}

/*
 * Release the anons of npages pages from va. A private segment's map is
 * shared only with the other pieces of the same segment after a split,
 * which own other slots; a shared segment's map may be another
 * process's too, and keeps its anons until the last reference goes.
 */
static void
segvn_free_anons(struct seg *seg, vaddr_t va, size_t npages)
{
        struct segvn_data *svd = seg->s_data;

        if (svd->svd_amp != NULL && (svd->svd_type == MAP_PRIVATE ||
            svd->svd_amp->am_ref == 1))
                amap_free_range(svd->svd_amp, svd->svd_anon_index +
                    btop(va - seg->s_base), npages);
}

static int
segvn_split(struct seg *seg, vaddr_t addr)
{
        struct segvn_data *svd = seg->s_data;
        struct segvn_data *nsvd;
        struct seg *nseg;
        size_t off = addr - seg->s_base;

        if (off == 0 || off >= seg->s_size || (off & PAGEOFFSET) != 0)
                return EINVAL;
        nseg = seg_alloc(seg->s_as, addr, seg->s_size - off);
        nsvd = segvn_data_alloc();
        *nsvd = *svd;
        nsvd->svd_offset += (off_t)off;
        nsvd->svd_anon_index += btop(off);
        if (nsvd->svd_amp != NULL)
                amap_hold(nsvd->svd_amp);
        segvn_vnode_hold(svd->svd_vp);
        nseg->s_ops = &segvn_ops;
        nseg->s_data = nsvd;
        seg->s_size = off;
        return 0;
}

static int
segvn_unmap(struct seg *seg, vaddr_t addr, size_t len)
{
        struct segvn_data *svd = seg->s_data;
        vaddr_t end = seg->s_base + seg->s_size;
        int error;

        segvn_unload(seg, addr, btop(len));
        segvn_free_anons(seg, addr, btop(len));
        if (addr == seg->s_base) {
                seg->s_base += len;
                seg->s_size -= len;
                svd->svd_offset += (off_t)len;
                svd->svd_anon_index += btop(len);
                return 0;
        }
        if (addr + len == end) {
                seg->s_size -= len;
                return 0;
        }
        /* A hole in the middle: split, then drop the front of the tail. */
        if ((error = segvn_split(seg, addr)) != 0)
                return error;
        return segvn_unmap(seg->s_next, addr, len);
}

static void
segvn_free(struct seg *seg)
{
        struct segvn_data *svd = seg->s_data;

        segvn_unload(seg, seg->s_base, btop(seg->s_size));
        if (svd->svd_amp != NULL) {
                segvn_free_anons(seg, seg->s_base, btop(seg->s_size));
                amap_rele(svd->svd_amp);
        }
        segvn_vnode_rele(svd->svd_vp);
        kmem_zone_free(segvn_zone, svd);
}

static int
segvn_setprot(struct seg *seg, unsigned int prot)
{
        struct segvn_data *svd = seg->s_data;

        if ((prot & ~svd->svd_maxprot) != 0)
                return EACCES;
        svd->svd_prot = prot;
        /* Faults map the pages again with the new protection. */
        segvn_unload(seg, seg->s_base, btop(seg->s_size));
        return 0;
}

#define SEGVN_RACED     (-1)    /* the page was mapped already */

/*
 * Map pp at va with attr. A translation of the same page is kept and
 * only given attr, so that LWPs faulting on one page do not take each
 * other's mappings away (two of them doing so in turn can keep both
 * faulting for a long time); one of another page is replaced.
 * Returns FC_OK, FC_NOMEM, or SEGVN_RACED if a translation of pp was
 * there already, which the caller's own hold on pp is not needed for.
 */
static int
segvn_load(struct seg *seg, vaddr_t va, struct page *pp, unsigned int attr)
{
        struct hat *hat = seg->s_as->a_hat;
        unsigned int cur;
        paddr_t pa;
        int r;

        if (hat_probe(hat, va, &pa, &cur) && pa == page_to_pa(pp)) {
                if (cur != attr)
                        hat_chgprot(hat, va, 1, attr);
                return SEGVN_RACED;
        }
        segvn_unload(seg, va, 1);
        r = hat_memload(hat, va, page_to_pa(pp), attr);
        return r == 0 ? FC_OK : r == -1 ? SEGVN_RACED : FC_NOMEM;
}

static int
segvn_fault(struct seg *seg, vaddr_t va, enum seg_rw rw)
{
        struct segvn_data *svd = seg->s_data;
        struct anon_map *amp = svd->svd_amp;
        size_t idx = btop(va - seg->s_base);
        unsigned int need = rw == S_WRITE ? PROT_WRITE :
            rw == S_EXEC ? PROT_EXEC : PROT_READ;
        unsigned int attr = prot_attr(svd->svd_prot);
        int private = svd->svd_type == MAP_PRIVATE;
        struct anon **slot = NULL;
        struct anon *ap;
        struct page *pp;
        int fc;

        if ((svd->svd_prot & need) == 0)
                return FC_PROT;
        if (amp != NULL) {
                mutex_lock(&amp->am_lock);
                if (svd->svd_anon_index + idx >= amp->am_npages)
                        (void)amap_grow(amp, svd->svd_anon_index + idx + 1);
                slot = &amp->am_slot[svd->svd_anon_index + idx];
        }

        if (slot != NULL && (ap = *slot) != NULL) {
                if (private && ap->an_ref > 1) {
                        if (rw == S_WRITE) {
                                struct anon *nap = anon_copy(ap->an_page);

                                if (nap == NULL) {
                                        fc = FC_NOMEM;
                                        goto out;
                                }
                                /*
                                 * Drop our mapping of the shared anon
                                 * before our reference: once released, it
                                 * may be freed and its page reused at once
                                 * by the other process's copy.
                                 */
                                segvn_unload(seg, va, 1);
                                *slot = nap;
                                anon_rele(ap);
                                ap = nap;
                        } else {
                                attr &= ~(unsigned int)HAT_WRITE;
                        }
                }
                fc = segvn_load(seg, va, ap->an_page, attr);
        } else if (svd->svd_vp != NULL) {
                off_t off = svd->svd_offset + (off_t)ptob(idx);
                int error = VOP_GETPAGE(svd->svd_vp, off, &pp, kcred);

                if (error != 0) {
                        fc = error == ENOMEM ? FC_NOMEM : FC_OBJERR;
                        goto out;
                }
                if (private && rw == S_WRITE) {
                        struct anon *nap = anon_copy(pp);

                        page_rele(pp);
                        if (nap == NULL) {
                                fc = FC_NOMEM;
                                goto out;
                        }
                        *slot = nap;
                        fc = segvn_load(seg, va, nap->an_page, attr);
                } else {
                        if (private)
                                attr &= ~(unsigned int)HAT_WRITE;
                        /* The mapping takes over the hold. */
                        if ((fc = segvn_load(seg, va, pp, attr)) != FC_OK)
                                page_rele(pp);
                        if (fc == SEGVN_RACED)
                                fc = FC_OK;
                }
        } else {
                struct anon *nap = anon_zero();

                if (nap == NULL) {
                        fc = FC_NOMEM;
                        goto out;
                }
                if (slot == NULL)
                        panic("segvn_fault: anonymous segment without a map");
                *slot = nap;
                fc = segvn_load(seg, va, nap->an_page, attr);
        }
out:
        if (amp != NULL)
                mutex_unlock(&amp->am_lock);
        return fc == SEGVN_RACED ? FC_OK : fc;
}
