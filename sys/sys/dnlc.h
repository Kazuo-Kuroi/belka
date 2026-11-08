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
 * Entries hold no references (IRIX): each keeps both vnode pointers
 * with the v_number each had, and a hit turns the target into a real
 * reference with vn_get(), which fails if the vnode has been reused or
 * is being inactivated. The callers that change the name space still
 * purge the names they change: remove, rename, rmdir, mount, and
 * unmount. A file system purges a private vnode before freeing it.
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
        uint64_t        ds_stale;       /* hits on vnodes since reused */
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
