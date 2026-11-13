/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * tmpfs: a file system in memory.
 *
 * Every file is a tmpnode with a private vnode embedded. Regular files
 * keep their data in the page cache of that vnode, which never lets the
 * pages go (PC_KEEP), with holes left unallocated; directories keep a
 * list of entries; symbolic links keep their text.
 *
 * Locking:
 * - tm_lock (a mutex, one per mount) serialises every name-space change
 *   and every change to node attributes and link counts, and guards the
 *   list of nodes.
 * - tn_rwlock (an mrlock, one per node) guards a regular file's size and
 *   which pages it has: readers share it, writers and truncation take
 *   it alone.
 * - Order: tm_lock, then tn_rwlock. Reads and writes take only
 *   tn_rwlock.
 *
 * A node whose link count reaches zero is freed when its vnode is no
 * longer referenced: tmpfs_inactive re-checks v_count under tm_lock,
 * which also serialises it with lookups that take new references.
 */

#ifndef _FS_TMPFS_TMPFS_H_
#define _FS_TMPFS_TMPFS_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/synch.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/vnode.h>

struct page;
struct vfs;

/* Mount argument; NULL means a quarter of memory. */
struct tmpfs_args {
        uint64_t        ta_maxpages;
};

struct tmpnode;

struct tdirent {
        struct tdirent  *td_next;
        struct tmpnode  *td_node;
        uint16_t        td_namelen;
        char            td_name[];
};

struct tmpnode {
        struct vnode    tn_vnode;
        struct tmpnode  *tn_allnext;
        struct tmpnode  *tn_allprev;
        struct tmount   *tn_mount;
        struct mrlock   tn_rwlock;

        enum vtype      tn_type;
        mode_t          tn_mode;
        uid_t           tn_uid;
        gid_t           tn_gid;
        nlink_t         tn_nlink;
        ino_t           tn_ino;
        volatile off_t  tn_size;
        struct timespec tn_atime;
        struct timespec tn_mtime;
        struct timespec tn_ctime;

        /* VDIR */
        struct tdirent  *tn_dirents;
        struct tmpnode  *tn_parent;
        uint32_t        tn_dirlen;      /* entries, not counting . and .. */

        /* VLNK */
        char            *tn_link;
        size_t          tn_linklen;
};

struct tmount {
        struct mutex    tm_lock;
        struct vfs      *tm_vfs;
        struct tmpnode  *tm_root;
        struct tmpnode  *tm_all;
        uint64_t        tm_maxpages;
        volatile uint64_t tm_pages;     /* file pages in use */
        ino_t           tm_nextino;
        uint32_t        tm_nnodes;
};

#define VTOTN(vp)       ((struct tmpnode *)(vp)->v_data)
#define TNTOV(tn)       (&(tn)->tn_vnode)

extern const struct vnodeops tmpfs_vnodeops;

struct tmpnode *tmpnode_alloc(struct tmount *, enum vtype, mode_t,
    const struct vattr *);
void tmpnode_free(struct tmount *, struct tmpnode *);

#endif /* _FS_TMPFS_TMPFS_H_ */
