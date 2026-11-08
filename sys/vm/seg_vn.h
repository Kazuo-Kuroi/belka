/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The vnode segment driver: mappings of files and anonymous memory,
 * after System V seg_vn.
 *
 * A segment maps pages of a vnode, or nothing (anonymous, zero-filled),
 * shared or private. Pages a private segment writes, and every page of
 * an anonymous one, are anons in the segment's anon map. Page-cache
 * pages are mapped directly while nobody writes them, each mapping
 * holding the page.
 *
 * Shared writable mappings of files are not supported yet: they need
 * write-back of pages modified through the mapping.
 */

#ifndef _VM_SEG_VN_H_
#define _VM_SEG_VN_H_

#include <stddef.h>

#include <sys/types.h>

struct anon_map;
struct seg;
struct vnode;

struct segvn_crargs {
        struct vnode    *vp;            /* NULL: anonymous */
        off_t           offset;         /* in vp of the first page */
        int             type;           /* MAP_SHARED or MAP_PRIVATE */
        unsigned int    prot;
        unsigned int    maxprot;
};

struct segvn_data {
        struct vnode    *svd_vp;
        off_t           svd_offset;
        int             svd_type;
        unsigned int    svd_prot;
        unsigned int    svd_maxprot;
        struct anon_map *svd_amp;
        size_t          svd_anon_index; /* slot of the first page */
};

int segvn_create(struct seg *, void *);
int segvn_extend(struct seg *, size_t);

#endif /* _VM_SEG_VN_H_ */
