/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Anonymous memory: pages with no file behind them, after the System V
 * anon layer.
 *
 * An anon is one page of anonymous data with a reference count. An
 * anon map is an array of anons, one slot per page of a private or
 * anonymous segment. fork copies a private segment's map by taking
 * another reference on each anon; a write to an anon with more than one
 * reference copies it (copy on write).
 *
 * There is no swap, so an anon's page is simply its own: page identity
 * apart from swap (anonfs vnodes, as in UnixWare) comes with paging.
 *
 * Locking: am_lock (a mutex) protects a map's slots. Anon reference
 * counts are atomic, since one anon may be shared by the maps of
 * several processes; an anon's page never changes once it exists.
 */

#ifndef _VM_ANON_H_
#define _VM_ANON_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/synch.h>

struct page;

struct anon {
        volatile uint32_t an_ref;
        struct page     *an_page;
};

struct anon_map {
        struct mutex    am_lock;
        volatile uint32_t am_ref;
        size_t          am_npages;
        struct anon     **am_slot;
};

void anon_init(void);
struct anon *anon_zero(void);
struct anon *anon_copy(const struct page *);
void anon_rele(struct anon *);

struct anon_map *amap_alloc(size_t);
void amap_hold(struct anon_map *);
void amap_rele(struct anon_map *);
int amap_grow(struct anon_map *, size_t);
struct anon_map *amap_copy(struct anon_map *, size_t, size_t);
void amap_free_range(struct anon_map *, size_t, size_t);

/* Statistics. */
uint64_t anon_pages(void);

#endif /* _VM_ANON_H_ */
