/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The directory name lookup cache: recent <directory, name> -> vnode
 * translations, kept by the generic lookup code (as in IRIX). Names up
 * to NC_NAMLEN bytes are cached; there are no negative entries.
 *
 * Each entry holds a reference on both vnodes, as in classic System V,
 * so the callers that change the name space purge the names they
 * change: remove, rename, rmdir, mount, and unmount. IRIX's soft
 * references need its global vnode cache and come with it.
 */

#ifndef _SYS_DNLC_H_
#define _SYS_DNLC_H_

#include <stdint.h>

#define NC_NAMLEN       31
#define DNLC_SIZE       1024

struct vfs;
struct vnode;

struct dnlc_stats {
        uint64_t        ds_hits;
        uint64_t        ds_misses;
        uint64_t        ds_enters;
        uint64_t        ds_purges;
        uint32_t        ds_entries;
};

void dnlc_init(void);
struct vnode *dnlc_lookup(struct vnode *, const char *);
void dnlc_enter(struct vnode *, const char *, struct vnode *);
void dnlc_remove(struct vnode *, const char *);
void dnlc_purge_vp(struct vnode *);
void dnlc_purge_vfsp(struct vfs *);
void dnlc_get_stats(struct dnlc_stats *);

#endif /* _SYS_DNLC_H_ */
