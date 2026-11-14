/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Mounted file systems, after System V: one struct vfs per mount, with
 * the operations of its type, found through vfssw by name.
 *
 * Locking: vfs_mutex serialises mount and unmount against each other
 * and against lookups crossing a mount point. A lookup that crosses
 * into a file system holds its root vnode, so a busy file system
 * refuses to unmount.
 */

#ifndef _SYS_VFS_H_
#define _SYS_VFS_H_

#include <stdint.h>

#include <sys/statvfs.h>
#include <sys/types.h>

struct cred;
struct vnode;

#define VFS_SPECSZ      32

struct vfs {
        struct vfs      *vfs_next;      /* mount list, from rootvfs */
        const struct vfsops *vfs_op;
        struct vnode    *vfs_vnodecovered; /* held while mounted */
        uint32_t        vfs_flag;       /* VFS_* */
        uint32_t        vfs_bsize;
        int             vfs_fstype;     /* index in vfssw */
        uint64_t        vfs_fsid;
        void            *vfs_data;

        /* For the mount table (/dev/mnttab), set at mount. */
        char            vfs_special[VFS_SPECSZ]; /* device, or the type */
        char            *vfs_mntpt;     /* where, as given */
        int64_t         vfs_mtime;      /* when, seconds since 1970 */
};

/* vfs_flag */
#define VFS_RDONLY      0x0001          /* the file system set it */

struct vfsops {
        int     (*vfs_mount)(struct vfs *, const void *, struct cred *);
        int     (*vfs_unmount)(struct vfs *, struct cred *);
        int     (*vfs_root)(struct vfs *, struct vnode **);
        int     (*vfs_statvfs)(struct vfs *, struct statvfs *);
        int     (*vfs_sync)(struct vfs *, struct cred *);
};

#define VFS_MOUNT(vfsp, data, cr)       (vfsp)->vfs_op->vfs_mount(vfsp, data, cr)
#define VFS_UNMOUNT(vfsp, cr)           (vfsp)->vfs_op->vfs_unmount(vfsp, cr)
#define VFS_ROOT(vfsp, vpp)             (vfsp)->vfs_op->vfs_root(vfsp, vpp)
#define VFS_STATVFS(vfsp, sp)           (vfsp)->vfs_op->vfs_statvfs(vfsp, sp)
#define VFS_SYNC(vfsp, cr)              (vfsp)->vfs_op->vfs_sync(vfsp, cr)

struct vfssw {
        const char              *vsw_name;
        const struct vfsops     *vsw_vfsops;
};

extern const struct vfssw vfssw[];
extern struct vfs *rootvfs;

void vfs_init(void);
int vfs_mountroot(const char *, const void *);
void vfs_syncall(void);
void vfs_shutdown(void);
extern int vfs_shutting_down;   /* the last sync: leave them clean */
int vfs_mount(const char *, const char *, const void *);
int vfs_unmount(const char *);
int vfs_statvfs_path(const char *, struct statvfs *);
int vfs_statvfs(struct vfs *, struct statvfs *);
size_t vfs_mnttab(char *, size_t);

/* Cross mount points downward: replace *vpp by the mounted root. */
int vfs_traverse(struct vnode **);

#endif /* _SYS_VFS_H_ */
