/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * File operations by path, for the kernel itself; system calls will be
 * thin layers over these. Name-space changes purge the name cache here,
 * since the generic layer owns it.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dnlc.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/pathname.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

static int vn_parent(const char *, struct vnode **, char *, struct vnode **,
    int);

/* Look up path's parent and last name; *vpp is the target if it exists. */
static int
vn_parent(const char *path, struct vnode **dvpp, char *name,
    struct vnode **vpp, int follow)
{
        return lookuppn(path, follow, NULL, dvpp, name, vpp);
}

/*
 * Open path with FREAD/FWRITE/FCREAT/FEXCL/FTRUNC/FAPPEND flags,
 * creating it with mode if asked. The vnode is returned held.
 */
int
vn_open(const char *path, int flags, mode_t mode, struct vnode **vpp)
{
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        int error;

        if ((flags & FCREAT) != 0) {
                if ((error = vn_parent(path, &dvp, name, &vp, FOLLOW)) != 0)
                        return error;
                if (vp == NULL) {
                        struct vattr va;

                        memset(&va, 0, sizeof(va));
                        va.va_mask = AT_TYPE | AT_MODE;
                        va.va_type = VREG;
                        va.va_mode = mode & 07777;
                        error = VOP_CREATE(dvp, name, &va,
                            (flags & FEXCL) != 0 ? EXCL : NONEXCL, &vp,
                            kcred);
                        VN_RELE(dvp);
                        if (error != 0)
                                return error;
                } else {
                        VN_RELE(dvp);
                        if ((flags & FEXCL) != 0) {
                                VN_RELE(vp);
                                return EEXIST;
                        }
                }
        } else if ((error = lookupname(path, FOLLOW, &vp)) != 0) {
                return error;
        }

        if (vp->v_type == VDIR && (flags & FWRITE) != 0) {
                VN_RELE(vp);
                return EISDIR;
        }
        if ((error = VOP_ACCESS(vp, ((flags & FREAD) != 0 ? VREAD : 0) |
            ((flags & FWRITE) != 0 ? VWRITE : 0), kcred)) != 0 ||
            (error = VOP_OPEN(&vp, flags, kcred)) != 0) {
                VN_RELE(vp);
                return error;
        }
        if ((flags & FTRUNC) != 0 && (flags & FWRITE) != 0 &&
            vp->v_type == VREG) {
                struct vattr va;

                memset(&va, 0, sizeof(va));
                va.va_mask = AT_SIZE;
                if ((error = VOP_SETATTR(vp, &va, kcred)) != 0) {
                        (void)VOP_CLOSE(vp, flags, kcred);
                        VN_RELE(vp);
                        return error;
                }
        }
        *vpp = vp;
        return 0;
}

/* Close and release a vnode from vn_open(). */
int
vn_close(struct vnode *vp, int flags)
{
        int error = VOP_CLOSE(vp, flags, kcred);

        VN_RELE(vp);
        return error;
}

/*
 * Read or write len bytes at offset off. *residp, if given, gets the
 * bytes not transferred.
 */
int
vn_rdwr(enum uio_rw rw, struct vnode *vp, void *buf, size_t len, off_t off,
    int ioflag, size_t *residp)
{
        struct iovec iov;
        struct uio uio;
        int error;

        iov.iov_base = buf;
        iov.iov_len = len;
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_offset = off;
        uio.uio_resid = (ssize_t)len;
        uio.uio_segflg = UIO_SYSSPACE;
        uio.uio_rw = rw;
        if (rw == UIO_READ)
                error = VOP_READ(vp, &uio, ioflag, kcred);
        else
                error = VOP_WRITE(vp, &uio, ioflag, kcred);
        if (residp != NULL)
                *residp = (size_t)uio.uio_resid;
        return error;
}

int
vn_create(const char *path, mode_t mode, int excl, struct vnode **vpp)
{
        return vn_open(path, FREAD | FWRITE | FCREAT | (excl ? FEXCL : 0),
            mode, vpp);
}

int
vn_remove(const char *path)
{
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        int error;

        if ((error = vn_parent(path, &dvp, name, &vp, NO_FOLLOW)) != 0)
                return error;
        if (vp == NULL) {
                VN_RELE(dvp);
                return ENOENT;
        }
        if (vp->v_type == VDIR) {
                error = EPERM;
        } else if ((error = VOP_REMOVE(dvp, name, kcred)) == 0) {
                dnlc_remove(dvp, name);
        }
        VN_RELE(vp);
        VN_RELE(dvp);
        return error;
}

/* Make topath another name for the existing file frompath. */
int
vn_link(const char *frompath, const char *topath)
{
        char name[MAXNAMELEN];
        struct vnode *svp, *dvp, *vp;
        int error;

        if ((error = lookupname(frompath, NO_FOLLOW, &svp)) != 0)
                return error;
        if ((error = vn_parent(topath, &dvp, name, &vp, NO_FOLLOW)) != 0) {
                VN_RELE(svp);
                return error;
        }
        if (vp != NULL) {
                error = EEXIST;
                VN_RELE(vp);
        } else if (svp->v_type == VDIR) {
                error = EPERM;
        } else if (svp->v_vfsp != dvp->v_vfsp) {
                error = EXDEV;
        } else {
                error = VOP_LINK(dvp, svp, name, kcred);
        }
        VN_RELE(dvp);
        VN_RELE(svp);
        return error;
}

int
vn_rename(const char *frompath, const char *topath)
{
        char sname[MAXNAMELEN], tname[MAXNAMELEN];
        struct vnode *sdvp, *svp, *tdvp, *tvp;
        int error;

        if ((error = vn_parent(frompath, &sdvp, sname, &svp, NO_FOLLOW)) !=
            0)
                return error;
        if (svp == NULL) {
                VN_RELE(sdvp);
                return ENOENT;
        }
        if ((error = vn_parent(topath, &tdvp, tname, &tvp, NO_FOLLOW)) != 0) {
                VN_RELE(svp);
                VN_RELE(sdvp);
                return error;
        }
        if (sdvp->v_vfsp != tdvp->v_vfsp) {
                error = EXDEV;
        } else if ((error = VOP_RENAME(sdvp, sname, tdvp, tname, kcred)) ==
            0) {
                dnlc_remove(sdvp, sname);
                dnlc_remove(tdvp, tname);
                /* A directory that was replaced takes its names with it. */
                if (tvp != NULL && tvp != svp && tvp->v_type == VDIR)
                        dnlc_purge_vp(tvp);
        }
        if (tvp != NULL)
                VN_RELE(tvp);
        VN_RELE(tdvp);
        VN_RELE(svp);
        VN_RELE(sdvp);
        return error;
}

int
vn_mkdir(const char *path, mode_t mode)
{
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        struct vattr va;
        int error;

        if ((error = vn_parent(path, &dvp, name, &vp, NO_FOLLOW)) != 0)
                return error;
        if (vp != NULL) {
                VN_RELE(vp);
                VN_RELE(dvp);
                return EEXIST;
        }
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_TYPE | AT_MODE;
        va.va_type = VDIR;
        va.va_mode = mode & 07777;
        error = VOP_MKDIR(dvp, name, &va, &vp, kcred);
        if (error == 0)
                VN_RELE(vp);
        VN_RELE(dvp);
        return error;
}

int
vn_rmdir(const char *path)
{
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        int error;

        if ((error = vn_parent(path, &dvp, name, &vp, NO_FOLLOW)) != 0)
                return error;
        if (vp == NULL) {
                VN_RELE(dvp);
                return ENOENT;
        }
        if (vp->v_type != VDIR) {
                error = ENOTDIR;
        } else if ((vp->v_flag & VROOT) != 0 || vp->v_vfsmountedhere != NULL) {
                error = EBUSY;
        } else if ((error = VOP_RMDIR(dvp, name, kcred)) == 0) {
                dnlc_remove(dvp, name);
                dnlc_purge_vp(vp);
        }
        VN_RELE(vp);
        VN_RELE(dvp);
        return error;
}

/* Create the symbolic link path whose text is target. */
int
vn_symlink(const char *target, const char *path)
{
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        struct vattr va;
        int error;

        if (strlen(target) >= MAXPATHLEN)
                return ENAMETOOLONG;
        if ((error = vn_parent(path, &dvp, name, &vp, NO_FOLLOW)) != 0)
                return error;
        if (vp != NULL) {
                VN_RELE(vp);
                VN_RELE(dvp);
                return EEXIST;
        }
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_TYPE | AT_MODE;
        va.va_type = VLNK;
        va.va_mode = 0777;
        error = VOP_SYMLINK(dvp, name, &va, target, kcred);
        VN_RELE(dvp);
        return error;
}

/* Read a link's text into buf (size bytes, NUL-terminated). */
int
vn_readlink(const char *path, char *buf, size_t size, size_t *lenp)
{
        struct vnode *vp;
        struct iovec iov;
        struct uio uio;
        int error;

        if (size == 0)
                return EINVAL;
        if ((error = lookupname(path, NO_FOLLOW, &vp)) != 0)
                return error;
        if (vp->v_type != VLNK) {
                VN_RELE(vp);
                return EINVAL;
        }
        iov.iov_base = buf;
        iov.iov_len = size - 1;
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_offset = 0;
        uio.uio_resid = (ssize_t)(size - 1);
        uio.uio_segflg = UIO_SYSSPACE;
        uio.uio_rw = UIO_READ;
        error = VOP_READLINK(vp, &uio, kcred);
        VN_RELE(vp);
        if (error == 0) {
                *lenp = size - 1 - (size_t)uio.uio_resid;
                buf[*lenp] = '\0';
        }
        return error;
}

int
vn_stat(const char *path, int follow, struct vattr *va)
{
        struct vnode *vp;
        int error;

        if ((error = lookupname(path, follow, &vp)) != 0)
                return error;
        va->va_mask = AT_ALL;
        error = VOP_GETATTR(vp, va, kcred);
        VN_RELE(vp);
        return error;
}

int
vn_truncate(const char *path, off_t size)
{
        struct vnode *vp;
        struct vattr va;
        int error;

        if (size < 0)
                return EINVAL;
        if ((error = lookupname(path, FOLLOW, &vp)) != 0)
                return error;
        if (vp->v_type == VDIR) {
                VN_RELE(vp);
                return EISDIR;
        }
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_SIZE;
        va.va_size = size;
        error = VOP_SETATTR(vp, &va, kcred);
        VN_RELE(vp);
        return error;
}
