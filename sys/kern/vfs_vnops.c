/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * File operations by path, for the kernel and the system calls, which
 * are thin layers over these. The *at forms start a relative path at a
 * given directory. Name-space changes purge the name cache here, since
 * the generic layer owns it.
 *
 * The permission rules common to every file system are here too, with
 * the caller's credentials (CRED()), as in System V:
 * - Making or removing a name needs write and search permission on
 *   the directory. In a sticky directory (S_ISVTX) only the owner of
 *   the file or of the directory, or the superuser, may remove or
 *   rename a name. Moving a directory to another parent needs write
 *   permission on it, for its "..".
 * - A new file belongs to the effective user, and to the effective
 *   group, or to the directory's group if the directory is set-group-ID
 *   (a new directory then inherits the bit). Asking for set-group-ID on
 *   a group the caller is not in clears it.
 * - Only the owner or the superuser may change a mode; anyone else
 *   loses S_ISVTX on a file other than a directory, and S_ISGID if not
 *   in the file's group. The owner may give a file to one of its own
 *   groups but not to another user (_POSIX_CHOWN_RESTRICTED), and the
 *   set-ID bits go when anyone but the superuser changes an owner.
 * Search permission on each directory of a path is checked in
 * lookuppn(), and the file systems check a file's own bits in
 * VOP_ACCESS (vaccess()).
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
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

static int vn_parent(struct vnode *, const char *, struct vnode **, char *,
    struct vnode **, int);
static int vn_dirwrite(struct vnode *, struct cred *);
static int vn_sticky(struct vnode *, struct vnode *, struct cred *);
static int vn_newattr(struct vnode *, struct vattr *, struct cred *);

/*
 * Look up path's parent and last name; *vpp is the target if it exists.
 * A relative path starts at sdir, or the current directory if it is NULL.
 */
static int
vn_parent(struct vnode *sdir, const char *path, struct vnode **dvpp,
    char *name, struct vnode **vpp, int follow)
{
        return lookuppn(path, follow, sdir, dvpp, name, vpp);
}

/*
 * The System V check of mode (VREAD, VWRITE, VEXEC) against a file of
 * type type with permission bits fmode, owner uid, and group gid: the
 * owner's bits if cr's effective user owns it, else the group's if cr
 * is in its group, else the others'. The superuser may do anything but
 * execute a file that no one may execute.
 */
int
vaccess(enum vtype type, mode_t fmode, uid_t uid, gid_t gid, int mode,
    struct cred *cr)
{
        if (CRED_ISROOT(cr)) {
                if ((mode & VEXEC) != 0 && type != VDIR &&
                    (fmode & 0111) == 0)
                        return EACCES;
                return 0;
        }
        if (cr->cr_uid == uid)
                fmode >>= 6;
        else if (groupmember(gid, cr))
                fmode >>= 3;
        return ((fmode & 7) & mode) == (mode & 7) ? 0 : EACCES;
}

/* Whether cr may make or remove names in directory dvp. */
static int
vn_dirwrite(struct vnode *dvp, struct cred *cr)
{
        return VOP_ACCESS(dvp, VWRITE | VEXEC, cr);
}

/* Whether cr may remove vp from dvp if dvp is sticky. */
static int
vn_sticky(struct vnode *dvp, struct vnode *vp, struct cred *cr)
{
        struct vattr va;
        int error;

        if (CRED_ISROOT(cr))
                return 0;
        va.va_mask = AT_ALL;
        if ((error = VOP_GETATTR(dvp, &va, cr)) != 0)
                return error;
        if ((va.va_mode & S_ISVTX) == 0 || va.va_uid == cr->cr_uid)
                return 0;
        va.va_mask = AT_ALL;
        if ((error = VOP_GETATTR(vp, &va, cr)) != 0)
                return error;
        return va.va_uid == cr->cr_uid ? 0 : EPERM;
}

/*
 * Check that cr may make a name in dvp, and set the owner, group, and
 * set-group-ID bit of the new file whose type and mode va holds.
 */
static int
vn_newattr(struct vnode *dvp, struct vattr *va, struct cred *cr)
{
        struct vattr dva;
        int error;

        if ((error = vn_dirwrite(dvp, cr)) != 0)
                return error;
        dva.va_mask = AT_ALL;
        if ((error = VOP_GETATTR(dvp, &dva, cr)) != 0)
                return error;
        va->va_mask |= AT_UID | AT_GID;
        va->va_uid = cr->cr_uid;
        if ((dva.va_mode & S_ISGID) != 0) {
                va->va_gid = dva.va_gid;
                if (va->va_type == VDIR)
                        va->va_mode |= S_ISGID;
        } else {
                va->va_gid = cr->cr_gid;
        }
        if ((va->va_mode & S_ISGID) != 0 && va->va_type != VDIR &&
            !CRED_ISROOT(cr) && !groupmember(va->va_gid, cr))
                va->va_mode &= ~(mode_t)S_ISGID;
        return 0;
}

/*
 * Open path with FREAD/FWRITE/FCREAT/FEXCL/FTRUNC/FAPPEND flags,
 * creating it with mode if asked. The vnode is returned held.
 */
int
vn_openat(struct vnode *sdir, const char *path, int flags, mode_t mode,
    struct vnode **vpp)
{
        struct cred *cr = CRED();
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        int created = 0, error;

        if ((flags & FCREAT) != 0) {
again:
                if ((error = vn_parent(sdir, path, &dvp, name, &vp,
                    FOLLOW)) != 0)
                        return error;
                if (vp == NULL) {
                        struct vattr va;

                        /*
                         * Always exclusive, so that a file made by
                         * someone else meanwhile is opened only after
                         * the usual check: a new one needs none.
                         */
                        memset(&va, 0, sizeof(va));
                        va.va_mask = AT_TYPE | AT_MODE;
                        va.va_type = VREG;
                        va.va_mode = mode & 07777;
                        if ((error = vn_newattr(dvp, &va, cr)) == 0)
                                error = VOP_CREATE(dvp, name, &va, EXCL,
                                    &vp, cr);
                        VN_RELE(dvp);
                        if (error == EEXIST && (flags & FEXCL) == 0)
                                goto again;
                        if (error != 0)
                                return error;
                        created = 1;
                } else {
                        VN_RELE(dvp);
                        if ((flags & FEXCL) != 0) {
                                VN_RELE(vp);
                                return EEXIST;
                        }
                }
        } else if ((error = lookuppn(path, FOLLOW, sdir, NULL, NULL,
            &vp)) != 0) {
                return error;
        }

        if (vp->v_type == VDIR && (flags & FWRITE) != 0) {
                VN_RELE(vp);
                return EISDIR;
        }
        if (!created && (error = VOP_ACCESS(vp, ((flags & FREAD) != 0 ?
            VREAD : 0) | ((flags & FWRITE) != 0 ? VWRITE : 0), cr)) != 0) {
                VN_RELE(vp);
                return error;
        }
        if (vp->v_type == VFIFO) {
                /* A named FIFO: open the FIFO node that stands for it. */
                struct vnode *fvp;

                error = fifo_vp(vp, &fvp);
                VN_RELE(vp);
                if (error != 0)
                        return error;
                vp = fvp;
        }
        if ((error = VOP_OPEN(&vp, flags, cr)) != 0) {
                VN_RELE(vp);
                return error;
        }
        if ((flags & FTRUNC) != 0 && (flags & FWRITE) != 0 &&
            vp->v_type == VREG) {
                struct vattr va;

                memset(&va, 0, sizeof(va));
                va.va_mask = AT_SIZE;
                if ((error = VOP_SETATTR(vp, &va, cr)) != 0) {
                        (void)VOP_CLOSE(vp, flags, cr);
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
        int error = VOP_CLOSE(vp, flags, CRED());

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
        uio.uio_limit = UIO_NOLIMIT;
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
vn_removeat(struct vnode *sdir, const char *path)
{
        struct cred *cr = CRED();
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        int error;

        if ((error = vn_parent(sdir, path, &dvp, name, &vp,
            NO_FOLLOW)) != 0)
                return error;
        if (vp == NULL) {
                VN_RELE(dvp);
                return ENOENT;
        }
        if (vp->v_type == VDIR) {
                error = EPERM;
        } else if ((error = vn_dirwrite(dvp, cr)) == 0 &&
            (error = vn_sticky(dvp, vp, cr)) == 0 &&
            (error = VOP_REMOVE(dvp, name, cr)) == 0) {
                dnlc_remove(dvp, name);
        }
        VN_RELE(vp);
        VN_RELE(dvp);
        return error;
}

/*
 * Make topath another name for the existing file frompath, which is
 * looked up with follow (System V link() does not follow a last link).
 */
int
vn_linkat(struct vnode *fsdir, const char *frompath, struct vnode *tsdir,
    const char *topath, int follow)
{
        struct cred *cr = CRED();
        char name[MAXNAMELEN];
        struct vnode *svp, *dvp, *vp;
        int error;

        if ((error = lookuppn(frompath, follow, fsdir, NULL, NULL,
            &svp)) != 0)
                return error;
        if ((error = vn_parent(tsdir, topath, &dvp, name, &vp, NO_FOLLOW)) !=
            0) {
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
        } else if ((error = vn_dirwrite(dvp, cr)) == 0) {
                error = VOP_LINK(dvp, svp, name, cr);
        }
        VN_RELE(dvp);
        VN_RELE(svp);
        return error;
}

/*
 * Whether cr may rename svp in sdvp to a name in tdvp that is now tvp
 * (or NULL).
 */
static int
vn_rename_ok(struct vnode *sdvp, struct vnode *svp, struct vnode *tdvp,
    struct vnode *tvp, struct cred *cr)
{
        int error;

        if ((error = vn_dirwrite(sdvp, cr)) != 0 ||
            (error = vn_sticky(sdvp, svp, cr)) != 0 ||
            (error = vn_dirwrite(tdvp, cr)) != 0 ||
            (tvp != NULL && (error = vn_sticky(tdvp, tvp, cr)) != 0))
                return error;
        if (svp->v_type == VDIR && sdvp != tdvp)
                return VOP_ACCESS(svp, VWRITE, cr);
        return 0;
}

int
vn_renameat(struct vnode *fsdir, const char *frompath, struct vnode *tsdir,
    const char *topath)
{
        struct cred *cr = CRED();
        char sname[MAXNAMELEN], tname[MAXNAMELEN];
        struct vnode *sdvp, *svp, *tdvp, *tvp;
        int error;

        if ((error = vn_parent(fsdir, frompath, &sdvp, sname, &svp,
            NO_FOLLOW)) != 0)
                return error;
        if (svp == NULL) {
                VN_RELE(sdvp);
                return ENOENT;
        }
        if ((error = vn_parent(tsdir, topath, &tdvp, tname, &tvp,
            NO_FOLLOW)) != 0) {
                VN_RELE(svp);
                VN_RELE(sdvp);
                return error;
        }
        if (sdvp->v_vfsp != tdvp->v_vfsp) {
                error = EXDEV;
        } else if ((error = vn_rename_ok(sdvp, svp, tdvp, tvp, cr)) == 0 &&
            (error = VOP_RENAME(sdvp, sname, tdvp, tname, cr)) == 0) {
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
vn_mkdirat(struct vnode *sdir, const char *path, mode_t mode)
{
        struct cred *cr = CRED();
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        struct vattr va;
        int error;

        if ((error = vn_parent(sdir, path, &dvp, name, &vp,
            NO_FOLLOW)) != 0)
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
        if ((error = vn_newattr(dvp, &va, cr)) == 0 &&
            (error = VOP_MKDIR(dvp, name, &va, &vp, cr)) == 0)
                VN_RELE(vp);
        VN_RELE(dvp);
        return error;
}

/*
 * Make a node of type type (only VFIFO so far; there are no device
 * files yet) through VOP_CREATE, failing with EEXIST if path exists.
 */
int
vn_mknodat(struct vnode *sdir, const char *path, enum vtype type, mode_t mode)
{
        struct cred *cr = CRED();
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        struct vattr va;
        int error;

        if (type != VFIFO)
                return EINVAL;
        if ((error = vn_parent(sdir, path, &dvp, name, &vp,
            NO_FOLLOW)) != 0)
                return error;
        if (vp != NULL) {
                VN_RELE(vp);
                VN_RELE(dvp);
                return EEXIST;
        }
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_TYPE | AT_MODE;
        va.va_type = type;
        va.va_mode = mode & 07777;
        if ((error = vn_newattr(dvp, &va, cr)) == 0 &&
            (error = VOP_CREATE(dvp, name, &va, EXCL, &vp, cr)) == 0)
                VN_RELE(vp);
        VN_RELE(dvp);
        return error;
}

int
vn_rmdirat(struct vnode *sdir, const char *path)
{
        struct cred *cr = CRED();
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        int error;

        if ((error = vn_parent(sdir, path, &dvp, name, &vp,
            NO_FOLLOW)) != 0)
                return error;
        if (vp == NULL) {
                VN_RELE(dvp);
                return ENOENT;
        }
        if (vp->v_type != VDIR) {
                error = ENOTDIR;
        } else if ((vp->v_flag & VROOT) != 0 || vp->v_vfsmountedhere != NULL) {
                error = EBUSY;
        } else if ((error = vn_dirwrite(dvp, cr)) == 0 &&
            (error = vn_sticky(dvp, vp, cr)) == 0 &&
            (error = VOP_RMDIR(dvp, name, cr)) == 0) {
                dnlc_remove(dvp, name);
                dnlc_purge_vp(vp);
        }
        VN_RELE(vp);
        VN_RELE(dvp);
        return error;
}

/* Create the symbolic link path whose text is target. */
int
vn_symlinkat(const char *target, struct vnode *sdir, const char *path)
{
        struct cred *cr = CRED();
        char name[MAXNAMELEN];
        struct vnode *dvp, *vp;
        struct vattr va;
        int error;

        if (strlen(target) >= MAXPATHLEN)
                return ENAMETOOLONG;
        if ((error = vn_parent(sdir, path, &dvp, name, &vp,
            NO_FOLLOW)) != 0)
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
        if ((error = vn_newattr(dvp, &va, cr)) == 0)
                error = VOP_SYMLINK(dvp, name, &va, target, cr);
        VN_RELE(dvp);
        return error;
}

/* Read a link's text into buf (size bytes, NUL-terminated). */
int
vn_readlinkat(struct vnode *sdir, const char *path, char *buf, size_t size,
    size_t *lenp)
{
        struct vnode *vp;
        struct iovec iov;
        struct uio uio;
        int error;

        if (size == 0)
                return EINVAL;
        if ((error = lookuppn(path, NO_FOLLOW, sdir, NULL, NULL, &vp)) != 0)
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
        uio.uio_limit = UIO_NOLIMIT;
        uio.uio_rw = UIO_READ;
        error = VOP_READLINK(vp, &uio, CRED());
        VN_RELE(vp);
        if (error == 0) {
                *lenp = size - 1 - (size_t)uio.uio_resid;
                buf[*lenp] = '\0';
        }
        return error;
}

int
vn_statat(struct vnode *sdir, const char *path, int follow, struct vattr *va)
{
        struct vnode *vp;
        int error;

        if ((error = lookuppn(path, follow, sdir, NULL, NULL, &vp)) != 0)
                return error;
        va->va_mask = AT_ALL;
        error = VOP_GETATTR(vp, va, CRED());
        VN_RELE(vp);
        return error;
}

int
vn_truncate(const char *path, off_t size)
{
        struct cred *cr = CRED();
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
        if ((error = VOP_ACCESS(vp, VWRITE, cr)) == 0)
                error = VOP_SETATTR(vp, &va, cr);
        VN_RELE(vp);
        return error;
}

/*
 * Give vp the permission bits mode (07777), by the rules at the top of
 * this file.
 */
int
vn_chmod(struct vnode *vp, mode_t mode)
{
        struct cred *cr = CRED();
        struct vattr va;
        int error;

        va.va_mask = AT_ALL;
        if ((error = VOP_GETATTR(vp, &va, cr)) != 0)
                return error;
        if (!CRED_ISROOT(cr)) {
                if (cr->cr_uid != va.va_uid)
                        return EPERM;
                if (vp->v_type != VDIR)
                        mode &= ~(mode_t)S_ISVTX;
                if (!groupmember(va.va_gid, cr))
                        mode &= ~(mode_t)S_ISGID;
        }
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_MODE;
        va.va_mode = mode & 07777;
        return VOP_SETATTR(vp, &va, cr);
}

/*
 * Give vp a new owner and group; (uid_t)-1 or (gid_t)-1 leaves that
 * one as it is. See the top of this file for who may.
 */
int
vn_chown(struct vnode *vp, uid_t uid, gid_t gid)
{
        struct cred *cr = CRED();
        struct vattr cur, va;
        int error;

        cur.va_mask = AT_ALL;
        if ((error = VOP_GETATTR(vp, &cur, cr)) != 0)
                return error;
        memset(&va, 0, sizeof(va));
        if (uid != (uid_t)-1) {
                va.va_mask |= AT_UID;
                va.va_uid = uid;
        }
        if (gid != (gid_t)-1) {
                va.va_mask |= AT_GID;
                va.va_gid = gid;
        }
        if (!CRED_ISROOT(cr)) {
                if (cr->cr_uid != cur.va_uid ||
                    (uid != (uid_t)-1 && uid != cur.va_uid) ||
                    (gid != (gid_t)-1 && gid != cur.va_gid &&
                    !groupmember(gid, cr)))
                        return EPERM;
                if ((cur.va_mode & (S_ISUID | S_ISGID)) != 0 &&
                    vp->v_type != VDIR) {
                        va.va_mask |= AT_MODE;
                        va.va_mode = cur.va_mode & 07777 &
                            ~(mode_t)(S_ISUID | S_ISGID);
                }
        }
        return VOP_SETATTR(vp, &va, cr);
}

/*
 * Set vp's access and modification times. times[0] and times[1] are
 * the new ones, each possibly UTIME_NOW or UTIME_OMIT; times NULL means
 * both now. The owner and the superuser may set any times; anyone who
 * may write the file may set both to now (System V's utime rule).
 */
int
vn_utimes(struct vnode *vp, const struct timespec *times)
{
        struct cred *cr = CRED();
        struct timespec now, t[2];
        struct vattr va;
        int error, bothnow;

        gethrestime(&now);
        for (int i = 0; i < 2; i++) {
                t[i] = times != NULL ? times[i] :
                    (struct timespec){ 0, UTIME_NOW };
                if (t[i].tv_nsec == UTIME_NOW)
                        t[i] = now;
                else if (t[i].tv_nsec != UTIME_OMIT &&
                    (t[i].tv_nsec < 0 || t[i].tv_nsec >= (long)NANOSEC))
                        return EINVAL;
        }
        bothnow = times == NULL || (times[0].tv_nsec == UTIME_NOW &&
            times[1].tv_nsec == UTIME_NOW);

        va.va_mask = AT_ALL;
        if ((error = VOP_GETATTR(vp, &va, cr)) != 0)
                return error;
        if (!CRED_ISROOT(cr) && cr->cr_uid != va.va_uid) {
                if (!bothnow)
                        return EPERM;
                if ((error = VOP_ACCESS(vp, VWRITE, cr)) != 0)
                        return error;
        }
        memset(&va, 0, sizeof(va));
        if (t[0].tv_nsec != UTIME_OMIT) {
                va.va_mask |= AT_ATIME;
                va.va_atime = t[0];
        }
        if (t[1].tv_nsec != UTIME_OMIT) {
                va.va_mask |= AT_MTIME;
                va.va_mtime = t[1];
        }
        if (va.va_mask == 0)
                return 0;
        return VOP_SETATTR(vp, &va, cr);
}
