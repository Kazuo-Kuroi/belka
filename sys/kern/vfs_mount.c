/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The mount table: mounting, unmounting, the root file system, and
 * crossing mount points. See <sys/vfs.h>.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dnlc.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/pathname.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

extern const struct vfsops tmpfs_vfsops;
extern const struct vfsops ext2fs_vfsops;
extern const struct vfsops devfs_vfsops;

const struct vfssw vfssw[] = {
        { "tmpfs", &tmpfs_vfsops },
        { "ext2fs", &ext2fs_vfsops },
        { "devfs", &devfs_vfsops },
        { NULL, NULL }
};

struct vfs *rootvfs;
static struct mutex vfs_mutex;
static uint64_t vfs_next_fsid = 1;

static int vfs_type(const char *);

static int
vfs_type(const char *name)
{
        for (int i = 0; vfssw[i].vsw_name != NULL; i++) {
                if (strcmp(vfssw[i].vsw_name, name) == 0)
                        return i;
        }
        return -1;
}

/* Write back every mounted file system (sync). */
void
vfs_syncall(void)
{
        mutex_lock(&vfs_mutex);
        for (struct vfs *vfsp = rootvfs; vfsp != NULL; vfsp = vfsp->vfs_next)
                (void)VFS_SYNC(vfsp, kcred);
        mutex_unlock(&vfs_mutex);
}

void
vfs_init(void)
{
        mutex_init(&vfs_mutex, "vfs");
        dnlc_init();
}

/* Make a file system of type fstype the root, and set rootdir. */
int
vfs_mountroot(const char *fstype, const void *data)
{
        int t = vfs_type(fstype);
        struct vfs *vfsp;
        int error;

        if (t < 0)
                return ENODEV;
        vfsp = kmem_zalloc(sizeof(*vfsp), KM_SLEEP);
        vfsp->vfs_op = vfssw[t].vsw_vfsops;
        vfsp->vfs_fstype = t;
        vfsp->vfs_fsid = vfs_next_fsid++;
        if ((error = VFS_MOUNT(vfsp, data, kcred)) != 0 ||
            (error = VFS_ROOT(vfsp, &rootdir)) != 0) {
                kmem_free(vfsp, sizeof(*vfsp));
                return error;
        }
        rootvfs = vfsp;
        printf("vfs: root is %s\n", fstype);
        return 0;
}

/* Mount a file system of type fstype on the directory path. */
int
vfs_mount(const char *fstype, const char *path, const void *data)
{
        int t = vfs_type(fstype);
        struct vnode *vp;
        struct vfs *vfsp;
        int error;

        if (t < 0)
                return ENODEV;
        if ((error = lookupname(path, FOLLOW, &vp)) != 0)
                return error;
        if (vp->v_type != VDIR) {
                VN_RELE(vp);
                return ENOTDIR;
        }

        mutex_lock(&vfs_mutex);
        /*
         * Lookup crosses mount points, so a path already mounted on
         * names the mounted root; as in System V, mounting over a root
         * is refused.
         */
        if (vp->v_vfsmountedhere != NULL || (vp->v_flag & VROOT) != 0) {
                mutex_unlock(&vfs_mutex);
                VN_RELE(vp);
                return EBUSY;
        }
        vfsp = kmem_zalloc(sizeof(*vfsp), KM_SLEEP);
        vfsp->vfs_op = vfssw[t].vsw_vfsops;
        vfsp->vfs_fstype = t;
        vfsp->vfs_fsid = vfs_next_fsid++;
        if ((error = VFS_MOUNT(vfsp, data, kcred)) != 0) {
                mutex_unlock(&vfs_mutex);
                kmem_free(vfsp, sizeof(*vfsp));
                VN_RELE(vp);
                return error;
        }
        vfsp->vfs_vnodecovered = vp;            /* keeps our reference */
        vfsp->vfs_next = rootvfs->vfs_next;
        rootvfs->vfs_next = vfsp;
        vp->v_vfsmountedhere = vfsp;
        mutex_unlock(&vfs_mutex);
        /* Cached names below the covered directory are now hidden. */
        dnlc_purge_vp(vp);
        return 0;
}

/*
 * Unmount the file system whose root path names. EBUSY if it is the
 * root, has something mounted inside it, or has vnodes in use.
 */
int
vfs_unmount(const char *path)
{
        struct vnode *vp, *covered;
        struct vfs *vfsp, **link;
        int error;

        if ((error = lookupname(path, FOLLOW, &vp)) != 0)
                return error;
        if ((vp->v_flag & VROOT) == 0) {
                VN_RELE(vp);
                return EINVAL;
        }
        vfsp = vp->v_vfsp;
        VN_RELE(vp);
        if (vfsp == rootvfs)
                return EBUSY;

        mutex_lock(&vfs_mutex);
        for (struct vfs *v = rootvfs; v != NULL; v = v->vfs_next) {
                if (v->vfs_vnodecovered != NULL &&
                    v->vfs_vnodecovered->v_vfsp == vfsp) {
                        mutex_unlock(&vfs_mutex);
                        return EBUSY;
                }
        }
        /*
         * Write back the file system's pages and reclaim its cached
         * vnodes, so that only vnodes in use remain for the file system
         * to find. A vnode that bdflush holds for a moment while writing
         * it back makes the unmount fail once, so try for a while.
         */
        dnlc_purge_vfsp(vfsp);
        for (int tries = 0;; tries++) {
                vn_flush_vfs(vfsp);
                vn_purge_vfs(vfsp);
                if ((error = VFS_UNMOUNT(vfsp, kcred)) == 0)
                        break;
                if (error != EBUSY || tries == 20) {
                        mutex_unlock(&vfs_mutex);
                        return error;
                }
                delay(1);
        }
        for (link = &rootvfs->vfs_next; *link != NULL;
            link = &(*link)->vfs_next) {
                if (*link == vfsp) {
                        *link = vfsp->vfs_next;
                        break;
                }
        }
        covered = vfsp->vfs_vnodecovered;
        covered->v_vfsmountedhere = NULL;
        mutex_unlock(&vfs_mutex);
        VN_RELE(covered);
        kmem_free(vfsp, sizeof(*vfsp));
        return 0;
}

/*
 * While *vpp has a file system mounted on it, replace it by that file
 * system's root. Holds vfs_mutex across each step so an unmount cannot
 * slip in between reading v_vfsmountedhere and taking the root.
 */
int
vfs_traverse(struct vnode **vpp)
{
        while ((*vpp)->v_vfsmountedhere != NULL) {
                struct vnode *root;
                int error;

                mutex_lock(&vfs_mutex);
                if ((*vpp)->v_vfsmountedhere == NULL) {
                        mutex_unlock(&vfs_mutex);
                        break;
                }
                error = VFS_ROOT((*vpp)->v_vfsmountedhere, &root);
                mutex_unlock(&vfs_mutex);
                if (error != 0)
                        return error;
                VN_RELE(*vpp);
                *vpp = root;
        }
        return 0;
}

int
vfs_statvfs_path(const char *path, struct statvfs *sp)
{
        struct vnode *vp;
        int error;

        if ((error = lookupname(path, FOLLOW, &vp)) != 0)
                return error;
        error = VFS_STATVFS(vp->v_vfsp, sp);
        VN_RELE(vp);
        return error;
}
