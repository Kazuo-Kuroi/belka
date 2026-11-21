/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Address spaces and segments. See <vm/as.h> and <vm/seg.h>.
 *
 * The segment list is kept in address order and searched linearly; a
 * process has a handful of segments until shared libraries and threads
 * arrive, and the list can become a tree then.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/mman.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/param.h>
#include <machine/atomic.h>
#include <machine/vmparam.h>

#include <vm/as.h>
#include <vm/hat.h>
#include <vm/seg.h>
#include <vm/seg_vn.h>

static struct kmem_zone *as_zone;
static struct kmem_zone *seg_zone;

void
as_init(void)
{
        as_zone = kmem_zone_init(sizeof(struct as), "as");
        seg_zone = kmem_zone_init(sizeof(struct seg), "seg");
}

/*
 * A new segment of as, linked into place. The range must be free and
 * page-aligned; a_lock is held for update.
 */
struct seg *
seg_alloc(struct as *as, vaddr_t base, size_t size)
{
        struct seg *seg = kmem_zone_alloc(seg_zone, KM_SLEEP);
        struct seg **link = &as->a_segs;

        while (*link != NULL && (*link)->s_base < base)
                link = &(*link)->s_next;
        seg->s_base = base;
        seg->s_size = size;
        seg->s_as = as;
        seg->s_ops = NULL;
        seg->s_data = NULL;
        seg->s_next = *link;
        *link = seg;
        as->a_nsegs++;
        return seg;
}

/* Unlink and free a segment whose driver has let it go. */
void
seg_free(struct seg *seg)
{
        struct as *as = seg->s_as;
        struct seg **link = &as->a_segs;

        while (*link != seg)
                link = &(*link)->s_next;
        *link = seg->s_next;
        as->a_nsegs--;
        kmem_zone_free(seg_zone, seg);
}

struct as *
as_alloc(void)
{
        struct as *as = kmem_zone_alloc(as_zone, KM_SLEEP);

        mrinit(&as->a_lock, "as");
        mutex_init(&as->a_brklock, "brk");
        as->a_brkbase = 0;
        as->a_brksize = 0;
        as->a_segs = NULL;
        as->a_hat = hat_alloc();
        as->a_nsegs = 0;
        as->a_ref = 1;
        return as;
}

/* The caller already owns a reference or holds its owner's interlock. */
void
as_hold(struct as *as)
{
        (void)atomic_inc_32_nv(&as->a_ref);
}

/* Drop an owner; the last release requires that no processor has it loaded. */
void
as_free(struct as *as)
{
        if (atomic_dec_32_nv(&as->a_ref) != 0)
                return;
        mrupdate(&as->a_lock);
        while (as->a_segs != NULL) {
                struct seg *seg = as->a_segs;

                seg->s_ops->so_free(seg);
                seg_free(seg);
        }
        mrunlock(&as->a_lock);
        hat_free(as->a_hat);
        kmem_zone_free(as_zone, as);
}

/* The bytes as maps, all its segments together. */
size_t
as_size(struct as *as)
{
        size_t n = 0;

        mraccess(&as->a_lock);
        for (struct seg *s = as->a_segs; s != NULL; s = s->s_next)
                n += s->s_size;
        mrunlock(&as->a_lock);
        return n;
}

/* fork's copy of as. */
struct as *
as_dup(struct as *as)
{
        struct as *nas = as_alloc();

        mutex_lock(&as->a_brklock);
        nas->a_brkbase = as->a_brkbase;
        nas->a_brksize = as->a_brksize;
        mrupdate(&as->a_lock);
        for (struct seg *seg = as->a_segs; seg != NULL; seg = seg->s_next) {
                struct seg *nseg = seg_alloc(nas, seg->s_base, seg->s_size);

                if (seg->s_ops->so_dup(seg, nseg) != 0) {
                        seg_free(nseg);
                        mrunlock(&as->a_lock);
                        mutex_unlock(&as->a_brklock);
                        as_free(nas);
                        return NULL;
                }
        }
        mrunlock(&as->a_lock);
        mutex_unlock(&as->a_brklock);
        return nas;
}

/* The segment containing va, or NULL. a_lock is held. */
struct seg *
as_findseg(struct as *as, vaddr_t va)
{
        for (struct seg *seg = as->a_segs; seg != NULL; seg = seg->s_next) {
                if (va < seg->s_base)
                        return NULL;
                if (va - seg->s_base < seg->s_size)
                        return seg;
        }
        return NULL;
}

/* A non-empty page-aligned range inside user space. */
static int
range_ok(vaddr_t base, size_t len)
{
        return len != 0 && (base & PAGEOFFSET) == 0 &&
            (len & PAGEOFFSET) == 0 && base >= VM_USER_MIN &&
            base < VM_USER_MAX && len <= VM_USER_MAX - base;
}

/* Whether [base, base + len) overlaps a segment. a_lock is held. */
static int
as_overlaps(struct as *as, vaddr_t base, size_t len)
{
        for (struct seg *seg = as->a_segs; seg != NULL; seg = seg->s_next) {
                if (seg->s_base >= base + len)
                        return 0;
                if (seg->s_base + seg->s_size > base)
                        return 1;
        }
        return 0;
}

/*
 * Map [base, base + len), which must be free, with a segment created by
 * crfn(seg, args).
 */
int
as_map(struct as *as, vaddr_t base, size_t len,
    int (*crfn)(struct seg *, void *), void *args)
{
        struct seg *seg;
        int error;

        if (!range_ok(base, len))
                return EINVAL;
        mrupdate(&as->a_lock);
        if (as_overlaps(as, base, len)) {
                mrunlock(&as->a_lock);
                return ENOMEM;
        }
        seg = seg_alloc(as, base, len);
        if ((error = crfn(seg, args)) != 0)
                seg_free(seg);
        mrunlock(&as->a_lock);
        return error;
}

/*
 * Find a free range of len bytes at or above hint (or VM_MMAP_BASE),
 * first fit. Returns 0 with *basep set, or ENOMEM. a_lock is held.
 */
int
as_gap(struct as *as, size_t len, vaddr_t hint, vaddr_t *basep)
{
        vaddr_t base = hint >= VM_USER_MIN ? ptrunc(hint) : VM_MMAP_BASE;

        len = pround(len);
        for (struct seg *seg = as->a_segs; seg != NULL; seg = seg->s_next) {
                if (seg->s_base + seg->s_size <= base)
                        continue;
                if (seg->s_base >= base + len)
                        break;
                base = seg->s_base + seg->s_size;
        }
        if (base + len > VM_USER_MAX - VM_STACK_SIZE || base + len < base)
                return ENOMEM;
        *basep = base;
        return 0;
}

/* Remove every mapping in [base, base + len); holes are fine. */
int
as_unmap(struct as *as, vaddr_t base, size_t len)
{
        vaddr_t end = base + len;
        int error = 0;

        if (!range_ok(base, len))
                return EINVAL;
        mrupdate(&as->a_lock);
        for (struct seg *seg = as->a_segs, *next; seg != NULL && error == 0;
            seg = next) {
                vaddr_t s = seg->s_base, e = seg->s_base + seg->s_size;
                vaddr_t ub, ue;

                next = seg->s_next;
                if (e <= base)
                        continue;
                if (s >= end)
                        break;
                ub = s > base ? s : base;
                ue = e < end ? e : end;
                if (ub == s && ue == e) {
                        seg->s_ops->so_free(seg);
                        seg_free(seg);
                } else {
                        /* The driver shrinks or splits it. */
                        error = seg->s_ops->so_unmap(seg, ub, ue - ub);
                        next = seg->s_next;
                }
        }
        mrunlock(&as->a_lock);
        return error;
}

/*
 * Set the protection of [base, base + len), which must be mapped
 * throughout, splitting segments at its ends.
 */
int
as_setprot(struct as *as, vaddr_t base, size_t len, unsigned int prot)
{
        vaddr_t end = base + len, va = base;
        int error = 0;

        if (!range_ok(base, len))
                return EINVAL;
        mrupdate(&as->a_lock);
        while (va < end && error == 0) {
                struct seg *seg = as_findseg(as, va);

                if (seg == NULL) {
                        error = ENOMEM;
                        break;
                }
                if (seg->s_base < va) {
                        if ((error = seg->s_ops->so_split(seg, va)) != 0)
                                break;
                        continue;
                }
                if (seg->s_base + seg->s_size > end &&
                    (error = seg->s_ops->so_split(seg, end)) != 0)
                        break;
                error = seg->s_ops->so_setprot(seg, prot);
                va = seg->s_base + seg->s_size;
        }
        mrunlock(&as->a_lock);
        return error;
}

/* Resolve a fault at va. Returns an FC_* code. */
int
as_fault(struct as *as, vaddr_t va, enum seg_rw rw)
{
        struct seg *seg;
        int fc;

        if (va < VM_USER_MIN || va >= VM_USER_MAX)
                return FC_NOMAP;
        mraccess(&as->a_lock);
        if ((seg = as_findseg(as, va)) == NULL)
                fc = FC_NOMAP;
        else
                fc = seg->s_ops->so_fault(seg, ptrunc(va), rw);
        mrunlock(&as->a_lock);
        return fc;
}

/*
 * Move the end of the heap at base from base + oldsize to base + newsize
 * (page multiples): grow the anonymous segment there, or create it, or
 * unmap what is no longer wanted. ENOMEM if the new range is not free.
 */
int
as_brk(struct as *as, vaddr_t base, size_t oldsize, size_t newsize)
{
        struct segvn_crargs a;
        struct seg *seg;
        int error;

        if (newsize < oldsize)
                return as_unmap(as, base + newsize, oldsize - newsize);
        if (newsize == oldsize)
                return 0;
        if (!range_ok(base, newsize))
                return ENOMEM;
        if (oldsize == 0) {
                a.vp = NULL;
                a.offset = 0;
                a.type = MAP_PRIVATE;
                a.prot = PROT_READ | PROT_WRITE;
                a.maxprot = PROT_ALL;
                return as_map(as, base, newsize, segvn_create, &a);
        }
        mrupdate(&as->a_lock);
        seg = as_findseg(as, base + oldsize - 1);
        if (seg == NULL || seg->s_base + seg->s_size != base + oldsize ||
            as_overlaps(as, base + oldsize, newsize - oldsize))
                error = ENOMEM;
        else
                error = segvn_extend(seg, newsize - oldsize);
        mrunlock(&as->a_lock);
        return error;
}

/*
 * Map len bytes with a segment from crfn(seg, args) at the first free
 * range at or above hint, finding and mapping in one locked step so
 * that another LWP cannot take the range in between.
 */
int
as_map_anywhere(struct as *as, size_t len, vaddr_t hint,
    int (*crfn)(struct seg *, void *), void *args, vaddr_t *basep)
{
        struct seg *seg;
        vaddr_t base;
        int error;

        len = pround(len);
        mrupdate(&as->a_lock);
        if ((error = as_gap(as, len, hint, &base)) == 0) {
                seg = seg_alloc(as, base, len);
                if ((error = crfn(seg, args)) != 0)
                        seg_free(seg);
                else
                        *basep = base;
        }
        mrunlock(&as->a_lock);
        return error;
}

/* Reserve the highest available range below ceiling in one layout update. */
int
as_map_topdown(struct as *as, size_t len, vaddr_t ceiling,
    int (*crfn)(struct seg *, void *), void *args, vaddr_t *basep)
{
        vaddr_t next = VM_USER_MIN, base = 0;
        struct seg *seg;
        int error = ENOMEM;

        if (len == 0 || len > VM_USER_MAX - VM_USER_MIN)
                return ENOMEM;
        len = pround(len);
        ceiling = ptrunc(ceiling);
        if (ceiling > VM_USER_MAX || ceiling < VM_USER_MIN)
                return EINVAL;
        mrupdate(&as->a_lock);
        for (seg = as->a_segs; seg != NULL; seg = seg->s_next) {
                vaddr_t end = seg->s_base < ceiling ? seg->s_base : ceiling;

                if (end >= next && end - next >= len)
                        base = end - len;
                if (seg->s_base >= ceiling)
                        break;
                next = seg->s_base + seg->s_size;
        }
        if (seg == NULL && ceiling >= next && ceiling - next >= len)
                base = ceiling - len;
        if (base != 0) {
                seg = seg_alloc(as, base, len);
                error = crfn(seg, args);
                if (error != 0)
                        seg_free(seg);
                else
                        *basep = base;
        }
        mrunlock(&as->a_lock);
        return error;
}

/* as_gap() for callers that do not hold the lock. */
int
as_findgap(struct as *as, size_t len, vaddr_t hint, vaddr_t *basep)
{
        int error;

        mraccess(&as->a_lock);
        error = as_gap(as, len, hint, basep);
        mrunlock(&as->a_lock);
        return error;
}
