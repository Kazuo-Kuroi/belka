/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ext2fs vnode operations. See ext2fs.h for the locking. Every name-space
 * operation runs under em_lock and releases inodes it looked up with
 * ext2fs_rele_locked(), since a plain VN_RELE could re-enter the mount
 * lock from VOP_INACTIVE.
 *
 * File data goes through the buffer cache in file system blocks. A new
 * block, or one that a write covers entirely, is not read first.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/buf.h>
#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/pathname.h>
#include <sys/stat.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <machine/param.h>

#include <vm/page.h>
#include <vm/pvn.h>

#include "fs/ext2fs/ext2fs.h"

#define EXT2_MAXSMALLFILE       ((uint64_t)0x7fffffff)

static int is_dot(const char *);
static uint32_t ip_uid(const struct ext2inode *);
static uint32_t ip_gid(const struct ext2inode *);
static int ext2fs_lookup_locked(struct ext2inode *, const char *,
    struct ext2inode **);

static int
is_dot(const char *nm)
{
        return strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0;
}

static uint32_t
ip_uid(const struct ext2inode *ip)
{
        return ip->i_din.e2di_uid | ((uint32_t)ip->i_din.e2di_uid_high << 16);
}

static uint32_t
ip_gid(const struct ext2inode *ip)
{
        return ip->i_din.e2di_gid | ((uint32_t)ip->i_din.e2di_gid_high << 16);
}

/* Look name up in dp, returning the inode held. em_lock is held. */
static int
ext2fs_lookup_locked(struct ext2inode *dp, const char *nm,
    struct ext2inode **ipp)
{
        uint32_t ino;
        int error;

        if (strcmp(nm, ".") == 0 || nm[0] == '\0') {
                VN_HOLD(ITOV(dp));
                *ipp = dp;
                return 0;
        }
        if (dp->i_din.e2di_nlink == 0)
                return ENOENT;
        if ((error = ext2fs_dirlook(dp, nm, &ino)) != 0)
                return error;
        return ext2fs_vget(dp->i_mnt, ino, ipp);
}

static int
ext2fs_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
ext2fs_close(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
ext2fs_access(struct vnode *vp, int mode, struct cred *cr)
{
        struct ext2inode *ip = VTOI(vp);
        mode_t m = ip->i_din.e2di_mode;

        if ((mode & VWRITE) != 0 && ip->i_mnt->em_ronly &&
            vp->v_type != VCHR && vp->v_type != VBLK)
                return EROFS;
        return vaccess(vp->v_type, m & 07777, ip_uid(ip), ip_gid(ip), mode,
            cr);
}

static int
ext2fs_getattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        struct ext2mount *em = ip->i_mnt;

        mutex_lock(&em->em_lock);
        va->va_type = vp->v_type;
        va->va_mode = ip->i_din.e2di_mode & 07777;
        va->va_uid = ip_uid(ip);
        va->va_gid = ip_gid(ip);
        va->va_fsid = vp->v_vfsp->vfs_fsid;
        va->va_nodeid = ip->i_number;
        va->va_nlink = ip->i_din.e2di_nlink;
        va->va_size = (off_t)ip->i_size;
        va->va_atime.tv_sec = ip->i_din.e2di_atime;
        va->va_mtime.tv_sec = ip->i_din.e2di_mtime;
        va->va_ctime.tv_sec = ip->i_din.e2di_ctime;
        va->va_atime.tv_nsec = va->va_mtime.tv_nsec = va->va_ctime.tv_nsec = 0;
        va->va_rdev = 0;
        va->va_blksize = em->em_bsize;
        va->va_nblocks = ip->i_din.e2di_nblock;
        mutex_unlock(&em->em_lock);
        return 0;
}

static int
ext2fs_setattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        struct ext2mount *em = ip->i_mnt;
        int error = 0;

        if (em->em_ronly)
                return EROFS;
        if ((va->va_mask & AT_SIZE) != 0) {
                if (vp->v_type == VDIR)
                        return EISDIR;
                if (vp->v_type != VREG || va->va_size < 0)
                        return EINVAL;
                if ((uint64_t)va->va_size > EXT2_MAXSMALLFILE &&
                    !em->em_largefile)
                        return EFBIG;
                /* Mapped pages are held; they cannot be cut away. */
                if (vp->v_nmap != 0)
                        return ETXTBSY;
                mrupdate(&ip->i_rwlock);
        }
        mutex_lock(&em->em_lock);
        if ((va->va_mask & AT_SIZE) != 0) {
                error = ext2fs_truncate(ip, (uint64_t)va->va_size);
                ip->i_din.e2di_mtime = ext2fs_now();
        }
        if ((va->va_mask & AT_MODE) != 0)
                ip->i_din.e2di_mode = (uint16_t)((ip->i_din.e2di_mode &
                    S_IFMT) | (va->va_mode & 07777));
        if ((va->va_mask & AT_UID) != 0) {
                ip->i_din.e2di_uid = (uint16_t)va->va_uid;
                ip->i_din.e2di_uid_high = (uint16_t)(va->va_uid >> 16);
        }
        if ((va->va_mask & AT_GID) != 0) {
                ip->i_din.e2di_gid = (uint16_t)va->va_gid;
                ip->i_din.e2di_gid_high = (uint16_t)(va->va_gid >> 16);
        }
        if ((va->va_mask & AT_ATIME) != 0)
                ip->i_din.e2di_atime = (uint32_t)va->va_atime.tv_sec;
        if ((va->va_mask & AT_MTIME) != 0)
                ip->i_din.e2di_mtime = (uint32_t)va->va_mtime.tv_sec;
        ip->i_din.e2di_ctime = ext2fs_now();
        if (error == 0)
                error = ext2fs_iupdate(ip);
        mutex_unlock(&em->em_lock);
        if ((va->va_mask & AT_SIZE) != 0)
                mrunlock(&ip->i_rwlock);
        return error;
}

static int
ext2fs_lookup(struct vnode *dvp, const char *nm, struct vnode **vpp,
    struct cred *cr __unused)
{
        struct ext2inode *dp = VTOI(dvp), *ip;
        struct ext2mount *em = dp->i_mnt;
        int error;

        if (dvp->v_type != VDIR)
                return ENOTDIR;
        mutex_lock(&em->em_lock);
        error = ext2fs_lookup_locked(dp, nm, &ip);
        mutex_unlock(&em->em_lock);
        if (error == 0)
                *vpp = ITOV(ip);
        return error;
}

static int
ext2fs_create(struct vnode *dvp, const char *nm, struct vattr *va, int excl,
    struct vnode **vpp, struct cred *cr __unused)
{
        struct ext2inode *dp = VTOI(dvp), *ip;
        struct ext2mount *em = dp->i_mnt;
        int error;

        if (em->em_ronly)
                return EROFS;
        if (va->va_type != VREG && va->va_type != VFIFO)
                return EINVAL;
        if (is_dot(nm))
                return EEXIST;
        mutex_lock(&em->em_lock);
        if ((error = ext2fs_lookup_locked(dp, nm, &ip)) == 0) {
                if (excl == EXCL)
                        error = EEXIST;
                else if (ITOV(ip)->v_type == VDIR)
                        error = EISDIR;
                if (error != 0)
                        ext2fs_rele_locked(ip);
                else
                        *vpp = ITOV(ip);
                mutex_unlock(&em->em_lock);
                return error;
        }
        if (error != ENOENT || dp->i_din.e2di_nlink == 0) {
                mutex_unlock(&em->em_lock);
                return error == ENOENT ? ENOENT : error;
        }
        if ((error = ext2fs_newinode(em, dp, (uint16_t)((va->va_type == VFIFO ?
            S_IFIFO : S_IFREG) | (va->va_mode & 07777)), va, &ip)) == 0) {
                if ((error = ext2fs_direnter(dp, nm, ip->i_number,
                    ext2fs_ftype(va->va_type))) != 0) {
                        ip->i_din.e2di_nlink = 0;
                        ext2fs_rele_locked(ip);
                } else {
                        *vpp = ITOV(ip);
                }
        }
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_remove(struct vnode *dvp, const char *nm, struct cred *cr __unused)
{
        struct ext2inode *dp = VTOI(dvp), *ip;
        struct ext2mount *em = dp->i_mnt;
        int error;

        if (em->em_ronly)
                return EROFS;
        if (is_dot(nm))
                return EINVAL;
        mutex_lock(&em->em_lock);
        if ((error = ext2fs_lookup_locked(dp, nm, &ip)) == 0) {
                if (ITOV(ip)->v_type == VDIR) {
                        error = EPERM;
                } else if ((error = ext2fs_dirremove(dp, nm)) == 0) {
                        ip->i_din.e2di_nlink--;
                        ip->i_din.e2di_ctime = ext2fs_now();
                        error = ext2fs_iupdate(ip);
                }
                ext2fs_rele_locked(ip);
        }
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_link(struct vnode *tdvp, struct vnode *svp, const char *nm,
    struct cred *cr __unused)
{
        struct ext2inode *dp = VTOI(tdvp), *ip = VTOI(svp), *xp;
        struct ext2mount *em = dp->i_mnt;
        int error;

        if (em->em_ronly)
                return EROFS;
        if (svp->v_type == VDIR)
                return EPERM;
        if (is_dot(nm))
                return EEXIST;
        mutex_lock(&em->em_lock);
        if ((error = ext2fs_lookup_locked(dp, nm, &xp)) == 0) {
                ext2fs_rele_locked(xp);
                error = EEXIST;
        } else if (error == ENOENT) {
                if (ip->i_din.e2di_nlink >= EXT2_LINK_MAX)
                        error = EMLINK;
                else if (ip->i_din.e2di_nlink == 0)
                        error = ENOENT;
                else if ((error = ext2fs_direnter(dp, nm, ip->i_number,
                    ext2fs_ftype(svp->v_type))) == 0) {
                        ip->i_din.e2di_nlink++;
                        ip->i_din.e2di_ctime = ext2fs_now();
                        error = ext2fs_iupdate(ip);
                }
        }
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_rename(struct vnode *sdvp, const char *snm, struct vnode *tdvp,
    const char *tnm, struct cred *cr __unused)
{
        struct ext2inode *sdp = VTOI(sdvp), *tdp = VTOI(tdvp);
        struct ext2mount *em = sdp->i_mnt;
        struct ext2inode *sip = NULL, *tip = NULL;
        int isdir, error;

        if (em->em_ronly)
                return EROFS;
        if (is_dot(snm) || is_dot(tnm))
                return EINVAL;
        mutex_lock(&em->em_lock);
        if ((error = ext2fs_lookup_locked(sdp, snm, &sip)) != 0)
                goto out;
        isdir = ITOV(sip)->v_type == VDIR;
        if (tdp->i_din.e2di_nlink == 0) {
                error = ENOENT;
                goto out;
        }
        error = ext2fs_lookup_locked(tdp, tnm, &tip);
        if (error == ENOENT) {
                tip = NULL;
        } else if (error != 0) {
                goto out;
        } else if (tip == sip) {
                goto out;                       /* the same file */
        } else if (isdir && ITOV(tip)->v_type != VDIR) {
                error = ENOTDIR;
                goto out;
        } else if (!isdir && ITOV(tip)->v_type == VDIR) {
                error = EISDIR;
                goto out;
        } else if (ITOV(tip)->v_vfsmountedhere != NULL) {
                error = EBUSY;
                goto out;
        } else if (ITOV(tip)->v_type == VDIR) {
                int empty = ext2fs_dirempty(tip);

                if (empty < 0) {
                        error = -empty;
                        goto out;
                }
                if (!empty) {
                        error = ENOTEMPTY;
                        goto out;
                }
        }
        error = 0;

        /* A directory may not move into its own subtree. */
        if (isdir && sdp != tdp) {
                uint32_t cur = tdp->i_number;

                while (cur != EXT2_ROOTINO) {
                        struct ext2inode *cp;
                        uint32_t up;

                        if (cur == sip->i_number) {
                                error = EINVAL;
                                goto out;
                        }
                        if ((error = ext2fs_vget(em, cur, &cp)) != 0)
                                goto out;
                        error = ext2fs_dirlook(cp, "..", &up);
                        ext2fs_rele_locked(cp);
                        if (error != 0)
                                goto out;
                        if (up == cur)
                                break;
                        cur = up;
                }
        }
        if (isdir && sdp != tdp && tip == NULL &&
            tdp->i_din.e2di_nlink >= EXT2_LINK_MAX) {
                error = EMLINK;
                goto out;
        }

        if (tip != NULL) {
                if ((error = ext2fs_dirremove(tdp, tnm)) != 0)
                        goto out;
                if (ITOV(tip)->v_type == VDIR) {
                        tip->i_din.e2di_nlink = 0;
                        tdp->i_din.e2di_nlink--;
                } else {
                        tip->i_din.e2di_nlink--;
                }
                tip->i_din.e2di_ctime = ext2fs_now();
                (void)ext2fs_iupdate(tip);
        }
        if ((error = ext2fs_dirremove(sdp, snm)) != 0 ||
            (error = ext2fs_direnter(tdp, tnm, sip->i_number,
            ext2fs_ftype(ITOV(sip)->v_type))) != 0)
                goto out;
        if (isdir && sdp != tdp) {
                if ((error = ext2fs_setdotdot(sip, tdp->i_number)) != 0)
                        goto out;
                sdp->i_din.e2di_nlink--;
                tdp->i_din.e2di_nlink++;
        }
        sip->i_din.e2di_ctime = ext2fs_now();
        (void)ext2fs_iupdate(sip);
        (void)ext2fs_iupdate(sdp);
        if (tdp != sdp)
                (void)ext2fs_iupdate(tdp);
out:
        if (tip != NULL)
                ext2fs_rele_locked(tip);
        if (sip != NULL)
                ext2fs_rele_locked(sip);
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_mkdir(struct vnode *dvp, const char *nm, struct vattr *va,
    struct vnode **vpp, struct cred *cr __unused)
{
        struct ext2inode *dp = VTOI(dvp), *ip;
        struct ext2mount *em = dp->i_mnt;
        int error;

        if (em->em_ronly)
                return EROFS;
        if (is_dot(nm))
                return EEXIST;
        mutex_lock(&em->em_lock);
        if ((error = ext2fs_lookup_locked(dp, nm, &ip)) == 0) {
                ext2fs_rele_locked(ip);
                error = EEXIST;
                goto out;
        }
        if (error != ENOENT)
                goto out;
        if (dp->i_din.e2di_nlink == 0) {
                error = ENOENT;
                goto out;
        }
        if (dp->i_din.e2di_nlink >= EXT2_LINK_MAX) {
                error = EMLINK;
                goto out;
        }
        if ((error = ext2fs_newinode(em, dp, (uint16_t)(S_IFDIR |
            (va->va_mode & 07777)), va, &ip)) != 0)
                goto out;
        if ((error = ext2fs_dirinit(ip, dp->i_number)) != 0 ||
            (error = ext2fs_iupdate(ip)) != 0 ||
            (error = ext2fs_direnter(dp, nm, ip->i_number,
            EXT2_FT_DIR)) != 0) {
                ip->i_din.e2di_nlink = 0;
                ext2fs_rele_locked(ip);
                goto out;
        }
        dp->i_din.e2di_nlink++;
        (void)ext2fs_iupdate(dp);
        *vpp = ITOV(ip);
out:
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_rmdir(struct vnode *dvp, const char *nm, struct cred *cr __unused)
{
        struct ext2inode *dp = VTOI(dvp), *ip;
        struct ext2mount *em = dp->i_mnt;
        int error, empty;

        if (em->em_ronly)
                return EROFS;
        if (is_dot(nm))
                return EINVAL;
        mutex_lock(&em->em_lock);
        if ((error = ext2fs_lookup_locked(dp, nm, &ip)) != 0)
                goto out;
        if (ITOV(ip)->v_type != VDIR) {
                error = ENOTDIR;
        } else if ((empty = ext2fs_dirempty(ip)) <= 0) {
                error = empty < 0 ? -empty : ENOTEMPTY;
        } else if ((error = ext2fs_dirremove(dp, nm)) == 0) {
                dp->i_din.e2di_nlink--;
                (void)ext2fs_iupdate(dp);
                ip->i_din.e2di_nlink = 0;
                ip->i_din.e2di_ctime = ext2fs_now();
                (void)ext2fs_iupdate(ip);
        }
        ext2fs_rele_locked(ip);
out:
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_readdir(struct vnode *vp, struct uio *uio, int *eofp,
    struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        struct ext2mount *em = ip->i_mnt;
        int error;

        if (vp->v_type != VDIR)
                return ENOTDIR;
        mutex_lock(&em->em_lock);
        error = ext2fs_readdir_uio(ip, uio, eofp);
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_symlink(struct vnode *dvp, const char *nm, struct vattr *va,
    const char *target, struct cred *cr __unused)
{
        struct ext2inode *dp = VTOI(dvp), *ip;
        struct ext2mount *em = dp->i_mnt;
        size_t len = strlen(target);
        int error;

        if (em->em_ronly)
                return EROFS;
        if (is_dot(nm))
                return EEXIST;
        if (len >= em->em_bsize)
                return ENAMETOOLONG;
        mutex_lock(&em->em_lock);
        if ((error = ext2fs_lookup_locked(dp, nm, &ip)) == 0) {
                ext2fs_rele_locked(ip);
                error = EEXIST;
                goto out;
        }
        if (error != ENOENT)
                goto out;
        if ((error = ext2fs_newinode(em, dp, S_IFLNK | 0777, va, &ip)) != 0)
                goto out;
        if (len < EXT2_MAXSYMLINKLEN) {
                /* A fast link: the text lives in the block pointers. */
                memcpy(ip->i_din.e2di_blocks, target, len);
        } else {
                struct buf *bp;
                uint32_t pbn;
                int isnew;

                if ((error = ext2fs_bmap(ip, 0, &pbn, 1, &isnew)) != 0) {
                        ip->i_din.e2di_nlink = 0;
                        ext2fs_rele_locked(ip);
                        goto out;
                }
                bp = ext2fs_getblk(em, pbn);
                bclrbuf(bp);
                memcpy(bp->b_data, target, len);
                bdwrite(bp);
        }
        ext2fs_setsize(ip, len);
        if ((error = ext2fs_iupdate(ip)) != 0 ||
            (error = ext2fs_direnter(dp, nm, ip->i_number,
            EXT2_FT_SYMLINK)) != 0)
                ip->i_din.e2di_nlink = 0;
        ext2fs_rele_locked(ip);
out:
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_readlink(struct vnode *vp, struct uio *uio, struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        struct ext2mount *em = ip->i_mnt;
        uint32_t facl_sects = ip->i_din.e2di_facl != 0 ? em->em_bsects : 0;
        size_t len = (size_t)ip->i_size;
        struct buf *bp;
        uint32_t pbn;
        int error;

        if (vp->v_type != VLNK)
                return EINVAL;
        if (len < EXT2_MAXSYMLINKLEN && ip->i_din.e2di_nblock == facl_sects)
                return uiomove(ip->i_din.e2di_blocks, len, UIO_READ, uio);
        if (len >= em->em_bsize)
                return EIO;
        mutex_lock(&em->em_lock);
        error = ext2fs_bmap(ip, 0, &pbn, 0, NULL);
        if (error == 0 && pbn == 0)
                error = EIO;
        if (error == 0 && (error = ext2fs_bread(em, pbn, &bp)) == 0) {
                error = uiomove(bp->b_data, len, UIO_READ, uio);
                brelse(bp);
        }
        mutex_unlock(&em->em_lock);
        return error;
}

static int
ext2fs_read(struct vnode *vp, struct uio *uio, int ioflag __unused,
    struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        int error = 0;

        if (vp->v_type == VDIR)
                return EISDIR;
        if (vp->v_type != VREG)
                return EINVAL;
        if (uio->uio_offset < 0)
                return EINVAL;
        mraccess(&ip->i_rwlock);
        while (uio->uio_resid > 0 && (uint64_t)uio->uio_offset < ip->i_size) {
                uint64_t idx = (uint64_t)uio->uio_offset / PAGESIZE;
                size_t off = (size_t)((uint64_t)uio->uio_offset % PAGESIZE);
                size_t n = PAGESIZE - off;
                struct page *pp;

                if ((uint64_t)n > ip->i_size - (uint64_t)uio->uio_offset)
                        n = (size_t)(ip->i_size - (uint64_t)uio->uio_offset);
                if ((ssize_t)n > uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if ((error = ext2fs_getpage(ip, idx, 0, &pp)) != 0)
                        break;
                error = uiomove((char *)page_to_kva(pp) + off, n, UIO_READ,
                    uio);
                page_rele(pp);
                if (error != 0)
                        break;
        }
        mrunlock(&ip->i_rwlock);
        return error;
}

/*
 * Write through the page cache, a block at a time, allocating blocks as
 * needed. The page is filled before a block is allocated under it, so a
 * new block reads as the hole it was. Running out of space part way is
 * a short write; it is an error only if nothing was written. With
 * IO_SYNC the pages are written back before returning.
 */
static int
ext2fs_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr)
{
        struct ext2inode *ip = VTOI(vp);
        struct ext2mount *em = ip->i_mnt;
        ssize_t start = uio->uio_resid;
        int error = 0;

        if (vp->v_type != VREG)
                return EINVAL;
        if (em->em_ronly)
                return EROFS;
        if (uio->uio_offset < 0)
                return EINVAL;
        mrupdate(&ip->i_rwlock);
        mutex_lock(&em->em_lock);
        if ((ioflag & IO_APPEND) != 0)
                uio->uio_offset = (off_t)ip->i_size;
        while (uio->uio_resid > 0) {
                uint64_t off = (uint64_t)uio->uio_offset;
                uint64_t lbn = off / em->em_bsize;
                size_t boff = (size_t)(off % em->em_bsize);
                size_t n = em->em_bsize - boff;
                struct page *pp;
                uint32_t pbn;
                int isnew, whole;

                if ((ssize_t)n > uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if ((off_t)off >= uio->uio_limit) {
                        error = EFBIG;
                        break;
                }
                if (n > (uint64_t)uio->uio_limit - off)
                        n = (size_t)((uint64_t)uio->uio_limit - off);
                if (off + n > EXT2_MAXSMALLFILE && !em->em_largefile) {
                        error = EFBIG;
                        break;
                }
                whole = em->em_bsize == PAGESIZE && n == PAGESIZE;
                if ((error = ext2fs_getpage(ip, off / PAGESIZE, whole,
                    &pp)) != 0)
                        break;
                if ((error = ext2fs_bmap(ip, lbn, &pbn, 1, &isnew)) != 0) {
                        page_rele(pp);
                        break;
                }
                if (isnew)
                        binvalblk(em->em_dev, (daddr_t)pbn * em->em_bsects);
                error = uiomove((char *)page_to_kva(pp) + off % PAGESIZE, n,
                    UIO_WRITE, uio);
                page_setdirty(pp);
                page_rele(pp);
                if (error != 0)
                        break;
                if ((uint64_t)uio->uio_offset > ip->i_size)
                        ext2fs_setsize(ip, (uint64_t)uio->uio_offset);
        }
        if (uio->uio_resid != start) {
                ip->i_din.e2di_mtime = ip->i_din.e2di_ctime = ext2fs_now();
                /* A write by anyone else takes the set-ID bits. */
                if (!CRED_ISROOT(cr))
                        ip->i_din.e2di_mode &= (uint16_t)~(S_ISUID | S_ISGID);
                if (error == ENOSPC || error == EFBIG)
                        error = 0;
        }
        if (ext2fs_iupdate(ip) != 0 && error == 0)
                error = EIO;
        mutex_unlock(&em->em_lock);
        if ((ioflag & IO_SYNC) != 0 && error == 0)
                error = ext2fs_flushpages(ip);
        mrunlock(&ip->i_rwlock);
        return error;
}

static int
ext2fs_fsync(struct vnode *vp, struct cred *cr)
{
        int error = VOP_PUTPAGE(vp, 0, cr);
        int e = VFS_SYNC(vp->v_vfsp, cr);

        return error != 0 ? error : e;
}

static int
ext2fs_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        struct ext2mount *em = ip->i_mnt;
        int r;

        mutex_lock(&em->em_lock);
        r = ext2fs_inactive_locked(ip);
        mutex_unlock(&em->em_lock);
        return r;
}

/*
 * A page of the file for a mapping, held. Past the end of the file is
 * an error: the mapping has nothing there.
 */
static int
ext2fs_vop_getpage(struct vnode *vp, off_t off, struct page **ppp,
    struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        int error;

        if (vp->v_type != VREG)
                return ENODEV;
        mraccess(&ip->i_rwlock);
        if (off < 0 || (uint64_t)off >= ip->i_size)
                error = EFAULT;
        else
                error = ext2fs_getpage(ip, (uint64_t)off / PAGESIZE, 0, ppp);
        mrunlock(&ip->i_rwlock);
        return error;
}

/* Write back every dirty page of vp. */
static int
ext2fs_putpage(struct vnode *vp, int flags __unused, struct cred *cr __unused)
{
        struct ext2inode *ip = VTOI(vp);
        int error;

        if (vp->v_type != VREG)
                return 0;
        mraccess(&ip->i_rwlock);
        error = ext2fs_flushpages(ip);
        mrunlock(&ip->i_rwlock);
        return error;
}

const struct vnodeops ext2fs_vnodeops = {
        ext2fs_open,
        ext2fs_close,
        ext2fs_read,
        ext2fs_write,
        ext2fs_getattr,
        ext2fs_setattr,
        ext2fs_access,
        ext2fs_lookup,
        ext2fs_create,
        ext2fs_remove,
        ext2fs_link,
        ext2fs_rename,
        ext2fs_mkdir,
        ext2fs_rmdir,
        ext2fs_readdir,
        ext2fs_symlink,
        ext2fs_readlink,
        ext2fs_fsync,
        ext2fs_inactive,
        ext2fs_putpage,
        ext2fs_reclaim,
        ext2fs_vop_getpage,
        fs_poll
};
