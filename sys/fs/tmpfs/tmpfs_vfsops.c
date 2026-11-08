/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * tmpfs mount operations and node allocation. See tmpfs.h.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/param.h>

#include <vm/page.h>

#include "fs/tmpfs/tmpfs.h"

static int tmpfs_mount(struct vfs *, const void *, struct cred *);
static int tmpfs_unmount(struct vfs *, struct cred *);
static int tmpfs_root(struct vfs *, struct vnode **);
static int tmpfs_statvfs(struct vfs *, struct statvfs *);
static int tmpfs_sync(struct vfs *, struct cred *);

const struct vfsops tmpfs_vfsops = {
        tmpfs_mount, tmpfs_unmount, tmpfs_root, tmpfs_statvfs, tmpfs_sync
};

/* Make a node of type type. tm_lock is held. */
struct tmpnode *
tmpnode_alloc(struct tmount *tm, enum vtype type, mode_t mode)
{
        struct tmpnode *tn = kmem_zalloc(sizeof(*tn), KM_SLEEP);
        struct timespec now;

        gethrestime(&now);
        vn_init(TNTOV(tn), tm->tm_vfs, type, &tmpfs_vnodeops, tn);
        mrinit(&tn->tn_rwlock, "tmpnode");
        tn->tn_mount = tm;
        tn->tn_type = type;
        tn->tn_mode = mode & 07777;
        tn->tn_uid = kcred->cr_uid;
        tn->tn_gid = kcred->cr_gid;
        tn->tn_ino = tm->tm_nextino++;
        tn->tn_atime = tn->tn_mtime = tn->tn_ctime = now;

        tn->tn_allprev = NULL;
        tn->tn_allnext = tm->tm_all;
        if (tm->tm_all != NULL)
                tm->tm_all->tn_allprev = tn;
        tm->tm_all = tn;
        tm->tm_nnodes++;
        return tn;
}

/* Free a node and everything it holds. tm_lock is held. */
void
tmpnode_free(struct tmount *tm, struct tmpnode *tn)
{
        for (size_t i = 0; i < tn->tn_npslots; i++) {
                if (tn->tn_pages[i] != NULL)
                        page_free(tn->tn_pages[i], 0);
        }
        if (tn->tn_npages != 0)
                atomic_add_64(&tm->tm_pages, -(int64_t)tn->tn_npages);
        if (tn->tn_pages != NULL)
                kmem_free(tn->tn_pages, tn->tn_npslots * sizeof(struct page *));
        while (tn->tn_dirents != NULL) {
                struct tdirent *de = tn->tn_dirents;

                tn->tn_dirents = de->td_next;
                kmem_free(de, offsetof(struct tdirent, td_name) +
                    de->td_namelen + 1);
        }
        if (tn->tn_link != NULL)
                kmem_free(tn->tn_link, tn->tn_linklen + 1);

        if (tn->tn_allprev != NULL)
                tn->tn_allprev->tn_allnext = tn->tn_allnext;
        else
                tm->tm_all = tn->tn_allnext;
        if (tn->tn_allnext != NULL)
                tn->tn_allnext->tn_allprev = tn->tn_allprev;
        tm->tm_nnodes--;
        kmem_free(tn, sizeof(*tn));
}

static int
tmpfs_mount(struct vfs *vfsp, const void *data, struct cred *cr __unused)
{
        const struct tmpfs_args *args = data;
        struct tmount *tm = kmem_zalloc(sizeof(*tm), KM_SLEEP);
        struct page_stats st;
        struct tmpnode *root;

        mutex_init(&tm->tm_lock, "tmpfs");
        tm->tm_vfs = vfsp;
        tm->tm_nextino = 2;
        page_get_stats(&st);
        tm->tm_maxpages = args != NULL && args->ta_maxpages != 0 ?
            args->ta_maxpages : st.ps_managed / 4;

        mutex_lock(&tm->tm_lock);
        root = tmpnode_alloc(tm, VDIR, 0755);
        root->tn_parent = root;
        root->tn_nlink = 2;
        TNTOV(root)->v_flag |= VROOT;
        VN_HOLD(TNTOV(root));                   /* the mount's reference */
        tm->tm_root = root;
        mutex_unlock(&tm->tm_lock);

        vfsp->vfs_data = tm;
        vfsp->vfs_bsize = PAGESIZE;
        return 0;
}

/*
 * Refuse while any vnode other than the root is referenced, or the root
 * has references beyond the mount's own; otherwise free everything.
 */
static int
tmpfs_unmount(struct vfs *vfsp, struct cred *cr __unused)
{
        struct tmount *tm = vfsp->vfs_data;

        mutex_lock(&tm->tm_lock);
        for (struct tmpnode *tn = tm->tm_all; tn != NULL; tn = tn->tn_allnext) {
                uint32_t allowed = tn == tm->tm_root ? 1 : 0;

                if (TNTOV(tn)->v_count > allowed ||
                    (TNTOV(tn)->v_flag & VINACT) != 0) {
                        mutex_unlock(&tm->tm_lock);
                        return EBUSY;
                }
        }
        while (tm->tm_all != NULL)
                tmpnode_free(tm, tm->tm_all);
        mutex_unlock(&tm->tm_lock);
        kmem_free(tm, sizeof(*tm));
        return 0;
}

static int
tmpfs_root(struct vfs *vfsp, struct vnode **vpp)
{
        struct tmount *tm = vfsp->vfs_data;

        *vpp = TNTOV(tm->tm_root);
        VN_HOLD(*vpp);
        return 0;
}

static int
tmpfs_statvfs(struct vfs *vfsp, struct statvfs *sp)
{
        struct tmount *tm = vfsp->vfs_data;
        uint64_t used = tm->tm_pages;

        memset(sp, 0, sizeof(*sp));
        sp->f_bsize = sp->f_frsize = PAGESIZE;
        sp->f_blocks = tm->tm_maxpages;
        sp->f_bfree = sp->f_bavail = used < tm->tm_maxpages ?
            tm->tm_maxpages - used : 0;
        sp->f_files = tm->tm_nnodes;
        sp->f_ffree = sp->f_favail = UINT32_MAX;
        sp->f_fsid = vfsp->vfs_fsid;
        memcpy(sp->f_basetype, "tmpfs", 6);
        sp->f_namemax = 255;
        return 0;
}

static int
tmpfs_sync(struct vfs *vfsp __unused, struct cred *cr __unused)
{
        return 0;
}
