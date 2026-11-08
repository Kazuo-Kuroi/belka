/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Anonymous memory. See <vm/anon.h>.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/param.h>

#include <vm/anon.h>
#include <vm/page.h>

static struct kmem_zone *anon_zone;
static struct kmem_zone *amap_zone;
static volatile uint64_t anon_npages;

void
anon_init(void)
{
        anon_zone = kmem_zone_init(sizeof(struct anon), "anon");
        amap_zone = kmem_zone_init(sizeof(struct anon_map), "anon_map");
}

static struct anon *
anon_new(struct page *pp)
{
        struct anon *ap = kmem_zone_alloc(anon_zone, KM_SLEEP);

        ap->an_ref = 1;
        ap->an_page = pp;
        atomic_add_64(&anon_npages, 1);
        return ap;
}

/* A new anon holding a zeroed page, or NULL if memory is short. */
struct anon *
anon_zero(void)
{
        struct page *pp = page_alloc(0, PA_ZERO);

        return pp != NULL ? anon_new(pp) : NULL;
}

/* A new anon holding a copy of src, or NULL if memory is short. */
struct anon *
anon_copy(const struct page *src)
{
        struct page *pp = page_alloc(0, 0);

        if (pp == NULL)
                return NULL;
        memcpy(page_to_kva(pp), page_to_kva(src), PAGESIZE);
        return anon_new(pp);
}

void
anon_rele(struct anon *ap)
{
        if (atomic_dec_32_nv(&ap->an_ref) != 0)
                return;
        page_free(ap->an_page, 0);
        kmem_zone_free(anon_zone, ap);
        atomic_add_64(&anon_npages, -1);
}

uint64_t
anon_pages(void)
{
        return anon_npages;
}

/* A map of npages empty slots. */
struct anon_map *
amap_alloc(size_t npages)
{
        struct anon_map *amp = kmem_zone_alloc(amap_zone, KM_SLEEP);

        mutex_init(&amp->am_lock, "anon_map");
        amp->am_ref = 1;
        amp->am_npages = npages;
        amp->am_slot = npages != 0 ?
            kmem_zalloc(npages * sizeof(struct anon *), KM_SLEEP) : NULL;
        return amp;
}

void
amap_hold(struct anon_map *amp)
{
        (void)atomic_inc_32_nv(&amp->am_ref);
}

void
amap_rele(struct anon_map *amp)
{
        if (atomic_dec_32_nv(&amp->am_ref) != 0)
                return;
        amap_free_range(amp, 0, amp->am_npages);
        if (amp->am_slot != NULL)
                kmem_free(amp->am_slot, amp->am_npages * sizeof(struct anon *));
        kmem_zone_free(amap_zone, amp);
}

/* Give the map at least npages slots. am_lock is held. */
int
amap_grow(struct anon_map *amp, size_t npages)
{
        struct anon **slot;

        if (npages <= amp->am_npages)
                return 0;
        slot = kmem_zalloc(npages * sizeof(*slot), KM_SLEEP);
        if (amp->am_slot != NULL) {
                memcpy(slot, amp->am_slot, amp->am_npages * sizeof(*slot));
                kmem_free(amp->am_slot, amp->am_npages * sizeof(*slot));
        }
        amp->am_slot = slot;
        amp->am_npages = npages;
        return 0;
}

/*
 * A new map of npages slots sharing, by reference, the anons of slots
 * [start, start + npages) of amp: the copy fork makes of a private
 * segment.
 */
struct anon_map *
amap_copy(struct anon_map *amp, size_t start, size_t npages)
{
        struct anon_map *nmp = amap_alloc(npages);

        mutex_lock(&amp->am_lock);
        for (size_t i = 0; i < npages && start + i < amp->am_npages; i++) {
                struct anon *ap = amp->am_slot[start + i];

                if (ap != NULL) {
                        (void)atomic_inc_32_nv(&ap->an_ref);
                        nmp->am_slot[i] = ap;
                }
        }
        mutex_unlock(&amp->am_lock);
        return nmp;
}

/* Release the anons of slots [start, start + npages). */
void
amap_free_range(struct anon_map *amp, size_t start, size_t npages)
{
        mutex_lock(&amp->am_lock);
        for (size_t i = start; i < start + npages && i < amp->am_npages;
            i++) {
                if (amp->am_slot[i] != NULL) {
                        anon_rele(amp->am_slot[i]);
                        amp->am_slot[i] = NULL;
                }
        }
        mutex_unlock(&amp->am_lock);
}
