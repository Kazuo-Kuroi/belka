/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel memory allocator: zones of fixed-size objects carved from
 * slabs of pages, and kmem_alloc() on top of a set of size-class zones.
 * See <sys/kmem.h> for the interface.
 *
 * A slab is 2^order pages from the page allocator, reached through the
 * direct map. Its header sits at the end of the slab, its objects from
 * the start. Every page of a slab points back to the header through
 * p_private, so freeing an object finds its slab in constant time.
 *
 * Each zone keeps slabs with free objects on a partial list, partly used
 * ones at the head and empty ones at the tail, and full slabs on a full
 * list. One empty slab is kept per zone to absorb alloc/free cycles;
 * further empty slabs go back to the page allocator.
 *
 * Debugging is always on for now: each slab keeps a bitmap of allocated
 * objects, so a double free or a bad pointer panics, and free objects
 * are filled with KMEM_FREE_BYTE, checked when they are handed out
 * again, so a write after free panics.
 *
 * Locking: kz_lock protects one zone's lists, slabs, and counters. It is
 * never held while calling the page allocator.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/page.h>

#define KMEM_FREE_BYTE  0xdb
#define KMEM_MINOBJ     16
#define KMEM_SLAB_MAXOBJ 256            /* bits in ks_map */
#define KMEM_SLAB_MAXORDER 3            /* slabs of up to 32 KB */

struct kmem_slab {
        struct kmem_slab *ks_next;
        struct kmem_slab *ks_prev;
        struct kmem_zone *ks_zone;
        char            *ks_base;       /* first object */
        void            *ks_free;       /* free objects, linked */
        struct page     *ks_page;       /* first page of the slab */
        unsigned int    ks_inuse;
        unsigned int    ks_onfull;      /* on the full list */
        uint64_t        ks_map[KMEM_SLAB_MAXOBJ / 64];
};

struct kmem_zone {
        struct spinlock kz_lock;
        const char      *kz_name;
        size_t          kz_size;        /* object size, rounded */
        unsigned int    kz_order;       /* slab size, as a page order */
        unsigned int    kz_perslab;
        struct kmem_slab *kz_partial;
        struct kmem_slab *kz_partial_tail;
        struct kmem_slab *kz_full;
        size_t          kz_nempty;      /* empty slabs held */
        size_t          kz_nslabs;
        size_t          kz_inuse;
        uint64_t        kz_allocs;
        uint64_t        kz_frees;
        struct kmem_zone *kz_next;      /* all zones */
};

/* Size classes served by kmem_alloc(); all multiples of KMEM_ALIGN. */
static const uint16_t kmem_sizes[] = {
        16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256,
        320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048,
        2560, 3072, 3584, 4096
};
#define KMEM_NSIZES     (sizeof(kmem_sizes) / sizeof(kmem_sizes[0]))

static struct kmem_zone kmem_zone_zone;         /* holds the others */
static struct kmem_zone *kmem_size_zone[KMEM_MAXZONE / KMEM_ALIGN + 1];
static struct kmem_zone *kmem_zones;
static struct spinlock kmem_zones_lock = SPINLOCK_INITIALIZER("kmem_zones");
static volatile uint64_t kmem_large_count;
static char kmem_large_tag;                     /* p_private of large blocks */
static int kmem_ready;

static void kmem_zone_setup(struct kmem_zone *, size_t, const char *);
static struct kmem_slab *kmem_slab_create(struct kmem_zone *, int);
static void kmem_list_remove(struct kmem_zone *, struct kmem_slab *);
static void kmem_list_push_head(struct kmem_zone *, struct kmem_slab *);
static void kmem_list_push_tail(struct kmem_zone *, struct kmem_slab *);
static void kmem_poison(void *, size_t);
static void kmem_check_poison(const struct kmem_zone *, const void *);
static struct kmem_slab *kmem_obj_slab(struct kmem_zone *, const void *,
    unsigned int *);
static unsigned int kmem_large_order(size_t);

static void
kmem_poison(void *obj, size_t size)
{
        memset((char *)obj + sizeof(void *), KMEM_FREE_BYTE,
            size - sizeof(void *));
}

static void
kmem_check_poison(const struct kmem_zone *kz, const void *obj)
{
        const unsigned char *p = obj;

        for (size_t i = sizeof(void *); i < kz->kz_size; i++) {
                if (p[i] != KMEM_FREE_BYTE)
                        panic("kmem: %s object %p modified after free "
                            "(offset %zu holds %#x)", kz->kz_name, obj, i,
                            p[i]);
        }
}

/* Unlink a slab from whichever list holds it. */
static void
kmem_list_remove(struct kmem_zone *kz, struct kmem_slab *ks)
{
        if (ks->ks_prev != NULL)
                ks->ks_prev->ks_next = ks->ks_next;
        else if (ks->ks_onfull)
                kz->kz_full = ks->ks_next;
        else
                kz->kz_partial = ks->ks_next;
        if (ks->ks_next != NULL)
                ks->ks_next->ks_prev = ks->ks_prev;
        else if (!ks->ks_onfull)
                kz->kz_partial_tail = ks->ks_prev;
        ks->ks_next = NULL;
        ks->ks_prev = NULL;
}

static void
kmem_list_push_head(struct kmem_zone *kz, struct kmem_slab *ks)
{
        struct kmem_slab **head = ks->ks_onfull ? &kz->kz_full :
            &kz->kz_partial;

        ks->ks_prev = NULL;
        ks->ks_next = *head;
        if (*head != NULL)
                (*head)->ks_prev = ks;
        else if (!ks->ks_onfull)
                kz->kz_partial_tail = ks;
        *head = ks;
}

/* Empty slabs go to the tail of the partial list. */
static void
kmem_list_push_tail(struct kmem_zone *kz, struct kmem_slab *ks)
{
        ks->ks_onfull = 0;
        ks->ks_next = NULL;
        ks->ks_prev = kz->kz_partial_tail;
        if (kz->kz_partial_tail != NULL)
                kz->kz_partial_tail->ks_next = ks;
        else
                kz->kz_partial = ks;
        kz->kz_partial_tail = ks;
}

static void
kmem_zone_setup(struct kmem_zone *kz, size_t size, const char *name)
{
        unsigned int order = 0;
        size_t hdr = sizeof(struct kmem_slab);

        if (size < KMEM_MINOBJ)
                size = KMEM_MINOBJ;
        size = (size + KMEM_ALIGN - 1) & ~(size_t)(KMEM_ALIGN - 1);

        /*
         * Use the smallest slab that holds at least 8 objects, or one
         * that wastes no more than an eighth of itself.
         */
        while (order < KMEM_SLAB_MAXORDER) {
                size_t bytes = (size_t)PAGESIZE << order;
                size_t n = (bytes - hdr) / size;

                if (n >= 8 || bytes - hdr - n * size <= bytes / 8)
                        break;
                order++;
        }
        if (((size_t)PAGESIZE << order) - hdr < size)
                panic("kmem_zone_init: %s: objects of %zu bytes are too big",
                    name, size);

        memset(kz, 0, sizeof(*kz));
        spinlock_init(&kz->kz_lock, "kmem_zone");
        kz->kz_name = name;
        kz->kz_size = size;
        kz->kz_order = order;
        kz->kz_perslab = (unsigned int)((((size_t)PAGESIZE << order) - hdr) /
            size);
        if (kz->kz_perslab > KMEM_SLAB_MAXOBJ)
                kz->kz_perslab = KMEM_SLAB_MAXOBJ;

        {
                int s = spin_lock(&kmem_zones_lock);

                kz->kz_next = kmem_zones;
                kmem_zones = kz;
                spin_unlock(&kmem_zones_lock, s);
        }
}

/* Build a new slab with every object free. Called without kz_lock. */
static struct kmem_slab *
kmem_slab_create(struct kmem_zone *kz, int flags)
{
        size_t bytes = (size_t)PAGESIZE << kz->kz_order;
        struct kmem_slab *ks;
        struct page *pp;
        char *base;

        if ((pp = page_alloc(kz->kz_order, 0)) == NULL) {
                if ((flags & KM_NOSLEEP) == 0)
                        panic("kmem: out of memory for zone %s", kz->kz_name);
                return NULL;
        }
        base = page_to_kva(pp);
        ks = (struct kmem_slab *)(void *)(base + bytes - sizeof(*ks));
        memset(ks, 0, sizeof(*ks));
        ks->ks_zone = kz;
        ks->ks_base = base;
        ks->ks_page = pp;

        for (size_t i = 0; i < ((size_t)1 << kz->kz_order); i++)
                pp[i].p_private = ks;

        /* Link the objects in address order, last first. */
        for (unsigned int i = kz->kz_perslab; i > 0; i--) {
                void *obj = base + (size_t)(i - 1) * kz->kz_size;

                kmem_poison(obj, kz->kz_size);
                *(void **)obj = ks->ks_free;
                ks->ks_free = obj;
        }
        return ks;
}

/*
 * Find the slab of obj, which must belong to kz, and its index there.
 * Panics on a pointer kz never handed out.
 */
static struct kmem_slab *
kmem_obj_slab(struct kmem_zone *kz, const void *obj, unsigned int *indexp)
{
        uintptr_t va = (uintptr_t)obj;
        struct kmem_slab *ks;
        struct page *pp;
        size_t off;

        if (va < dmap_base || va >= dmap_base + DMAP_MAXSIZE ||
            (pp = pa_to_page(kva_to_phys(obj))) == NULL)
                panic("kmem: %p is not kernel memory", obj);
        ks = pp->p_private;
        if (ks == NULL || (void *)ks == (void *)&kmem_large_tag ||
            ks->ks_zone != kz)
                panic("kmem: %p freed to zone %s, which does not own it",
                    obj, kz->kz_name);
        off = (size_t)((const char *)obj - ks->ks_base);
        if (off % kz->kz_size != 0 || off / kz->kz_size >= kz->kz_perslab)
                panic("kmem: %p is not an object of zone %s", obj,
                    kz->kz_name);
        *indexp = (unsigned int)(off / kz->kz_size);
        return ks;
}

struct kmem_zone *
kmem_zone_init(size_t size, const char *name)
{
        struct kmem_zone *kz = kmem_zone_alloc(&kmem_zone_zone, KM_SLEEP);

        kmem_zone_setup(kz, size, name);
        return kz;
}

void *
kmem_zone_alloc(struct kmem_zone *kz, int flags)
{
        struct kmem_slab *ks;
        unsigned int index;
        void *obj;
        int s;

        s = spin_lock(&kz->kz_lock);
        while ((ks = kz->kz_partial) == NULL) {
                struct kmem_slab *fresh;

                spin_unlock(&kz->kz_lock, s);
                if ((fresh = kmem_slab_create(kz, flags)) == NULL)
                        return NULL;
                s = spin_lock(&kz->kz_lock);
                fresh->ks_onfull = 0;
                kmem_list_push_head(kz, fresh);
                kz->kz_nslabs++;
        }

        if (ks->ks_inuse == 0 && kz->kz_nempty > 0)
                kz->kz_nempty--;
        obj = ks->ks_free;
        ks->ks_free = *(void **)obj;
        ks->ks_inuse++;
        index = (unsigned int)((size_t)((char *)obj - ks->ks_base) /
            kz->kz_size);
        ks->ks_map[index / 64] |= UINT64_C(1) << (index % 64);
        if (ks->ks_inuse == kz->kz_perslab) {
                kmem_list_remove(kz, ks);
                ks->ks_onfull = 1;
                kmem_list_push_head(kz, ks);
        }
        kz->kz_inuse++;
        kz->kz_allocs++;
        spin_unlock(&kz->kz_lock, s);

        kmem_check_poison(kz, obj);
        return obj;
}

void *
kmem_zone_zalloc(struct kmem_zone *kz, int flags)
{
        void *obj = kmem_zone_alloc(kz, flags);

        if (obj != NULL)
                memset(obj, 0, kz->kz_size);
        return obj;
}

void
kmem_zone_free(struct kmem_zone *kz, void *obj)
{
        struct kmem_slab *ks;
        struct kmem_slab *release = NULL;
        unsigned int index;
        uint64_t bit;
        int s;

        ks = kmem_obj_slab(kz, obj, &index);
        bit = UINT64_C(1) << (index % 64);

        /* The object is still the caller's until it is linked below. */
        kmem_poison(obj, kz->kz_size);

        s = spin_lock(&kz->kz_lock);
        if ((ks->ks_map[index / 64] & bit) == 0)
                panic("kmem: %s object %p freed twice", kz->kz_name, obj);
        ks->ks_map[index / 64] &= ~bit;
        *(void **)obj = ks->ks_free;
        ks->ks_free = obj;

        if (ks->ks_onfull) {
                kmem_list_remove(kz, ks);
                ks->ks_onfull = 0;
                kmem_list_push_head(kz, ks);
        }
        if (--ks->ks_inuse == 0) {
                kmem_list_remove(kz, ks);
                if (kz->kz_nempty >= 1) {
                        kz->kz_nslabs--;
                        release = ks;
                } else {
                        kz->kz_nempty++;
                        kmem_list_push_tail(kz, ks);
                }
        }
        kz->kz_inuse--;
        kz->kz_frees++;
        spin_unlock(&kz->kz_lock, s);

        if (release != NULL) {
                struct page *pp = release->ks_page;

                for (size_t i = 0; i < ((size_t)1 << kz->kz_order); i++)
                        pp[i].p_private = NULL;
                page_free(pp, kz->kz_order);
        }
}

void
kmem_zone_get_stats(struct kmem_zone *kz, struct kmem_zone_stats *st)
{
        int s = spin_lock(&kz->kz_lock);

        st->kzs_size = kz->kz_size;
        st->kzs_inuse = kz->kz_inuse;
        st->kzs_slabs = kz->kz_nslabs;
        st->kzs_allocs = kz->kz_allocs;
        st->kzs_frees = kz->kz_frees;
        spin_unlock(&kz->kz_lock, s);
}

void
kmem_init(void)
{
        unsigned int c = 0;

        kmem_zone_setup(&kmem_zone_zone, sizeof(struct kmem_zone),
            "kmem_zone");
        for (size_t i = 0; i < KMEM_NSIZES; i++) {
                static char names[KMEM_NSIZES][16];
                struct kmem_zone *kz;

                snprintf(names[i], sizeof(names[i]), "kmem_%u",
                    kmem_sizes[i]);
                kz = kmem_zone_init(kmem_sizes[i], names[i]);
                for (; c <= kmem_sizes[i] / KMEM_ALIGN; c++)
                        kmem_size_zone[c] = kz;
        }
        kmem_ready = 1;
}

/* The page order of a large block of size bytes. */
static unsigned int
kmem_large_order(size_t size)
{
        unsigned int order = 0;

        while (((size_t)PAGESIZE << order) < size)
                order++;
        return order;
}

void *
kmem_alloc(size_t size, int flags)
{
        struct page *pp;
        unsigned int order;

        if (!kmem_ready)
                panic("kmem_alloc: called before kmem_init");
        if (size == 0)
                return NULL;
        if (size <= KMEM_MAXZONE)
                return kmem_zone_alloc(
                    kmem_size_zone[(size + KMEM_ALIGN - 1) / KMEM_ALIGN],
                    flags);

        order = kmem_large_order(size);
        if (order >= PAGE_NORDERS ||
            (pp = page_alloc(order, 0)) == NULL) {
                if ((flags & KM_NOSLEEP) == 0)
                        panic("kmem_alloc: cannot allocate %zu bytes", size);
                return NULL;
        }
        pp->p_private = &kmem_large_tag;
        atomic_inc_64_nv(&kmem_large_count);
        return page_to_kva(pp);
}

void *
kmem_zalloc(size_t size, int flags)
{
        void *p = kmem_alloc(size, flags);

        if (p != NULL)
                memset(p, 0, size);
        return p;
}

/* Free a block from kmem_alloc(); size must be the size requested. */
void
kmem_free(void *p, size_t size)
{
        struct page *pp;

        if (p == NULL || size == 0)
                return;
        if (size <= KMEM_MAXZONE) {
                kmem_zone_free(
                    kmem_size_zone[(size + KMEM_ALIGN - 1) / KMEM_ALIGN], p);
                return;
        }

        if ((uintptr_t)p < dmap_base ||
            (uintptr_t)p >= dmap_base + DMAP_MAXSIZE ||
            ((uintptr_t)p & PAGEOFFSET) != 0 ||
            (pp = pa_to_page(kva_to_phys(p))) == NULL ||
            pp->p_private != (void *)&kmem_large_tag)
                panic("kmem_free: %p (%zu bytes) is not a large kmem block",
                    p, size);
        if (pp->p_order != kmem_large_order(size))
                panic("kmem_free: %p freed as %zu bytes, a different size "
                    "from its allocation", p, size);
        atomic_dec_64_nv(&kmem_large_count);
        page_free(pp, pp->p_order);
}

size_t
kmem_large_inuse(void)
{
        return (size_t)atomic_load_64(&kmem_large_count);
}
