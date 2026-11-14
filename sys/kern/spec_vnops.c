/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Vnode operations shared by device vnodes; see <sys/specvn.h>.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/specvn.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

static volatile uint64_t spec_nextino = 1;

void
specattr_init(struct specattr *sa, mode_t mode, uid_t uid, gid_t gid,
    dev_t rdev)
{
        sa->sa_mode = mode & 07777;
        sa->sa_uid = uid;
        sa->sa_gid = gid;
        sa->sa_rdev = rdev;
        sa->sa_ino = __atomic_fetch_add(&spec_nextino, 1, __ATOMIC_RELAXED);
        gethrestime(&sa->sa_time);
}

int
spec_getattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        const struct specattr *sa = vp->v_data;

        memset(va, 0, sizeof(*va));
        va->va_type = vp->v_type;
        va->va_mode = sa->sa_mode;
        va->va_uid = sa->sa_uid;
        va->va_gid = sa->sa_gid;
        va->va_fsid = vp->v_vfsp != NULL ? vp->v_vfsp->vfs_fsid : 0;
        va->va_nodeid = sa->sa_ino;
        va->va_nlink = vp->v_type == VDIR ? 2 : 1;
        va->va_rdev = sa->sa_rdev;
        va->va_atime = va->va_mtime = va->va_ctime = sa->sa_time;
        va->va_blksize = 128;
        return 0;
}

/*
 * The owner, group, and mode may change (the policy is vfs_vnops.c's);
 * a size, as for O_TRUNC, is ignored.
 */
int
spec_setattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        struct specattr *sa = vp->v_data;

        if ((va->va_mask & AT_MODE) != 0)
                sa->sa_mode = va->va_mode & 07777;
        if ((va->va_mask & AT_UID) != 0)
                sa->sa_uid = va->va_uid;
        if ((va->va_mask & AT_GID) != 0)
                sa->sa_gid = va->va_gid;
        return 0;
}

int
spec_access(struct vnode *vp, int mode, struct cred *cr)
{
        const struct specattr *sa = vp->v_data;

        return vaccess(vp->v_type, sa->sa_mode, sa->sa_uid, sa->sa_gid, mode,
            cr);
}

int
spec_lookup(struct vnode *dvp __unused, const char *nm __unused,
    struct vnode **vpp __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_create(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, int excl __unused, struct vnode **vpp __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_name1(struct vnode *dvp __unused, const char *nm __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_link(struct vnode *tdvp __unused, struct vnode *vp __unused,
    const char *nm __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_rename(struct vnode *sdvp __unused, const char *snm __unused,
    struct vnode *tdvp __unused, const char *tnm __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_mkdir(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, struct vnode **vpp __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_readdir(struct vnode *vp __unused, struct uio *uio __unused,
    int *eofp __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_symlink(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, const char *target __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

int
spec_readlink(struct vnode *vp __unused, struct uio *uio __unused,
    struct cred *cr __unused)
{
        return EINVAL;
}

int
spec_fsync(struct vnode *vp __unused, struct cred *cr __unused)
{
        return 0;
}

int
spec_putpage(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

int
spec_reclaim(struct vnode *vp __unused, int flags __unused)
{
        return EBUSY;
}

int
spec_getpage(struct vnode *vp __unused, off_t off __unused,
    struct page **ppp __unused, struct cred *cr __unused)
{
        return ENODEV;
}
