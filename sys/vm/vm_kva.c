/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel virtual address allocation and kernel stacks.
 *
 * Free KVA is a list of ranges sorted by address. Allocation is first
 * fit from the low end; freeing merges with both neighbours. Range
 * records come from a kmem zone. kva_free() allocates its spare record
 * before taking kva_lock, and records are freed after dropping it, so
 * the lock is never held across the allocator.
 *
 * The list is linear in the number of free ranges. A vmem-style arena
 * with size-segregated free lists can replace it when KVA use grows.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/hat.h>
#include <vm/kva.h>
#include <vm/page.h>

struct kva_seg {
        struct kva_seg  *ks_next;
        vaddr_t         ks_start;
        size_t          ks_npages;
};

static struct spinlock kva_lock = SPINLOCK_INITIALIZER("kva");
static struct kva_seg *kva_free_list;
static struct kmem_zone *kva_seg_zone;

/* Called after kmem_init(). */
void
kva_init(void)
{
        struct kva_seg *ks;

        kva_seg_zone = kmem_zone_init(sizeof(struct kva_seg), "kva_seg");
        ks = kmem_zone_alloc(kva_seg_zone, KM_SLEEP);
        ks->ks_next = NULL;
        ks->ks_start = KVA_FIXED_END;
        ks->ks_npages = btop(KVA_END - KVA_FIXED_END);
        kva_free_list = ks;
}

/*
 * Allocate npages of KVA. Returns the start address, or 0 if the area is
 * exhausted and flags has KM_NOSLEEP.
 */
vaddr_t
kva_alloc(size_t npages, int flags)
{
        struct kva_seg **link, *ks;
        struct kva_seg *drop = NULL;
        vaddr_t va = 0;
        int s;

        if (npages == 0)
                panic("kva_alloc: zero pages");

        s = spin_lock(&kva_lock);
        for (link = &kva_free_list; (ks = *link) != NULL;
            link = &ks->ks_next) {
                if (ks->ks_npages < npages)
                        continue;
                va = ks->ks_start;
                ks->ks_start += ptob(npages);
                ks->ks_npages -= npages;
                if (ks->ks_npages == 0) {
                        *link = ks->ks_next;
                        drop = ks;
                }
                break;
        }
        spin_unlock(&kva_lock, s);

        if (drop != NULL)
                kmem_zone_free(kva_seg_zone, drop);
        if (va == 0 && (flags & KM_NOSLEEP) == 0)
                panic("kva_alloc: kernel virtual area exhausted");
        return va;
}

/* Return npages of KVA starting at va. The range must be unmapped. */
void
kva_free(vaddr_t va, size_t npages)
{
        struct kva_seg *rec, *prev, *next;
        struct kva_seg *merged = NULL;
        vaddr_t end = va + ptob(npages);
        int s;

        if (va < KVA_FIXED_END || end > KVA_END || end <= va ||
            (va & PAGEOFFSET) != 0)
                panic("kva_free: bad range %#jx, %zu pages",
                    (uintmax_t)va, npages);

        /* A record is needed only when neither neighbour can absorb it. */
        rec = kmem_zone_alloc(kva_seg_zone, KM_SLEEP);

        s = spin_lock(&kva_lock);
        prev = NULL;
        for (next = kva_free_list; next != NULL && next->ks_start < va;
            next = next->ks_next)
                prev = next;

        if ((prev != NULL && prev->ks_start + ptob(prev->ks_npages) > va) ||
            (next != NULL && end > next->ks_start))
                panic("kva_free: range %#jx, %zu pages is already free",
                    (uintmax_t)va, npages);

        if (prev != NULL && prev->ks_start + ptob(prev->ks_npages) == va) {
                prev->ks_npages += npages;
                if (next != NULL && end == next->ks_start) {
                        /* Bridges two ranges: next's record goes too. */
                        prev->ks_npages += next->ks_npages;
                        prev->ks_next = next->ks_next;
                        merged = next;
                }
        } else if (next != NULL && end == next->ks_start) {
                next->ks_start = va;
                next->ks_npages += npages;
        } else {
                rec->ks_start = va;
                rec->ks_npages = npages;
                rec->ks_next = next;
                if (prev != NULL)
                        prev->ks_next = rec;
                else
                        kva_free_list = rec;
                rec = NULL;
        }
        spin_unlock(&kva_lock, s);

        if (rec != NULL)
                kmem_zone_free(kva_seg_zone, rec);
        if (merged != NULL)
                kmem_zone_free(kva_seg_zone, merged);
}

void
kva_get_stats(struct kva_stats *st)
{
        int s;

        st->kvs_free_pages = 0;
        st->kvs_segments = 0;
        s = spin_lock(&kva_lock);
        for (struct kva_seg *ks = kva_free_list; ks != NULL; ks = ks->ks_next) {
                st->kvs_free_pages += ks->ks_npages;
                st->kvs_segments++;
        }
        spin_unlock(&kva_lock, s);
}

/*
 * Allocate a kernel stack: KSTACK_PAGES mapped pages above one unmapped
 * guard page. Returns the stack top.
 */
vaddr_t
kstack_alloc(int flags)
{
        vaddr_t base = kva_alloc(KSTACK_PAGES + 1, flags);
        vaddr_t va;

        if (base == 0)
                return 0;
        for (int i = 0; i < KSTACK_PAGES; i++) {
                struct page *pp = page_alloc(0, PA_ZERO);

                va = base + ptob(i + 1);
                if (pp == NULL) {
                        if ((flags & KM_NOSLEEP) == 0)
                                panic("kstack_alloc: out of memory");
                        if (i > 0)
                                kstack_free(base + ptob(KSTACK_PAGES + 1));
                        else
                                kva_free(base, KSTACK_PAGES + 1);
                        return 0;
                }
                if (hat_kenter(va, page_to_pa(pp), HAT_WRITE) != 0)
                        panic("kstack_alloc: cannot map %#jx", (uintmax_t)va);
        }
        return base + ptob(KSTACK_PAGES + 1);
}

/*
 * Free a stack from kstack_alloc(), given its top. Tolerates pages that
 * were never mapped, for the failure path above.
 */
void
kstack_free(vaddr_t top)
{
        vaddr_t base = top - ptob(KSTACK_PAGES + 1);
        paddr_t pa[KSTACK_PAGES];
        int mapped[KSTACK_PAGES];

        for (int i = 0; i < KSTACK_PAGES; i++)
                mapped[i] = hat_kextract(base + ptob(i + 1), &pa[i]) == 0;
        hat_kremove(base + PAGESIZE, KSTACK_PAGES);
        for (int i = 0; i < KSTACK_PAGES; i++) {
                if (mapped[i])
                        page_free(pa_to_page(pa[i]), 0);
        }
        kva_free(base, KSTACK_PAGES + 1);
}
