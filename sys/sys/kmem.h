/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel memory allocation.
 *
 * kmem_alloc(), kmem_zalloc(), and kmem_free() have the System V DDI
 * interface: the caller passes the size again when freeing. Zones, after
 * IRIX's kmem_zone interface, give fixed-size object caches.
 *
 * Memory comes from the direct map and is physically contiguous.
 * Blocks of up to KMEM_MAXZONE bytes come from size-class zones and are
 * aligned to 16 bytes; larger blocks are whole pages from the page
 * allocator, aligned to a page.
 *
 * KM_SLEEP may not fail. Until the kernel can wait for memory to be
 * freed, a KM_SLEEP request that cannot be met panics. KM_NOSLEEP
 * returns NULL instead.
 *
 * Freed zone objects are filled with a pattern that is checked when the
 * object is next handed out, so a write after free is caught.
 */

#ifndef _SYS_KMEM_H_
#define _SYS_KMEM_H_

#include <stddef.h>
#include <stdint.h>

#define KM_SLEEP        0x0000
#define KM_NOSLEEP      0x0001

#define KMEM_MAXZONE    4096    /* largest size served from a zone */
#define KMEM_ALIGN      16

struct kmem_zone;

void kmem_init(void);

void *kmem_alloc(size_t, int);
void *kmem_zalloc(size_t, int);
void kmem_free(void *, size_t);

struct kmem_zone *kmem_zone_init(size_t, const char *);
void *kmem_zone_alloc(struct kmem_zone *, int);
void *kmem_zone_zalloc(struct kmem_zone *, int);
void kmem_zone_free(struct kmem_zone *, void *);

struct kmem_zone_stats {
        size_t  kzs_size;       /* object size */
        size_t  kzs_inuse;      /* objects allocated */
        size_t  kzs_slabs;      /* slabs held */
        uint64_t kzs_allocs;
        uint64_t kzs_frees;
};

void kmem_zone_get_stats(struct kmem_zone *, struct kmem_zone_stats *);

/* Large blocks currently allocated, for leak checks. */
size_t kmem_large_inuse(void);

#endif /* _SYS_KMEM_H_ */
