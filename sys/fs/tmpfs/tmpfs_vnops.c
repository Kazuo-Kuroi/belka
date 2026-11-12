/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * tmpfs vnode operations. See tmpfs.h for the locking.
 *
 * Directory offsets for VOP_READDIR are entry positions: 0 is ".", 1 is
 * "..", then the entries in creation order. Removing an entry shifts
 * the positions of later ones, so a directory read that races with
 * removals may skip a name; per-entry cookies would fix that.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/pathname.h>
#include <sys/stat.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/param.h>

#include <vm/page.h>
#include <vm/pvn.h>

#include "fs/tmpfs/tmpfs.h"

#define TMPFS_MAXFILEPAGES      ((size_t)1 << 24)       /* 64 GB files */

static const char zero_page[PAGESIZE];

static struct tdirent *tdir_find(struct tmpnode *, const char *,
    struct tdirent ***);
static int tdir_add(struct tmpnode *, const char *, struct tmpnode *);
static void tdir_unlink(struct tmpnode *, struct tdirent **);
static void tn_touch(struct tmpnode *, int);
static int tn_truncate(struct tmpnode *, off_t);
static int is_dot(const char *);

#define TOUCH_M         0x1
#define TOUCH_C         0x2

static int
is_dot(const char *nm)
{
        return strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0;
}

static void
tn_touch(struct tmpnode *tn, int what)
{
        struct timespec now;

        gethrestime(&now);
        if ((what & TOUCH_M) != 0)
                tn->tn_mtime = now;
        if ((what & TOUCH_C) != 0)
                tn->tn_ctime = now;
}

/* Find name in dir; *linkp gets the link to it, for unlinking. */
static struct tdirent *
tdir_find(struct tmpnode *dir, const char *nm, struct tdirent ***linkp)
{
        size_t len = strlen(nm);

        for (struct tdirent **l = &dir->tn_dirents; *l != NULL;
            l = &(*l)->td_next) {
                if ((*l)->td_namelen == len &&
                    memcmp((*l)->td_name, nm, len) == 0) {
                        if (linkp != NULL)
                                *linkp = l;
                        return *l;
                }
        }
        return NULL;
}

/* Append an entry; entries stay in creation order. */
static int
tdir_add(struct tmpnode *dir, const char *nm, struct tmpnode *tn)
{
        size_t len = strlen(nm);
        struct tdirent *de, **l;

        if (len == 0)
                return ENOENT;
        if (len >= MAXNAMELEN)
                return ENAMETOOLONG;
        de = kmem_alloc(offsetof(struct tdirent, td_name) + len + 1,
            KM_SLEEP);
        de->td_next = NULL;
        de->td_node = tn;
        de->td_namelen = (uint16_t)len;
        memcpy(de->td_name, nm, len + 1);
        for (l = &dir->tn_dirents; *l != NULL; l = &(*l)->td_next)
                continue;
        *l = de;
        dir->tn_dirlen++;
        tn_touch(dir, TOUCH_M | TOUCH_C);
        return 0;
}

static void
tdir_unlink(struct tmpnode *dir, struct tdirent **link)
{
        struct tdirent *de = *link;

        *link = de->td_next;
        dir->tn_dirlen--;
        tn_touch(dir, TOUCH_M | TOUCH_C);
        kmem_free(de, offsetof(struct tdirent, td_name) + de->td_namelen + 1);
}

/*
 * Set the size, freeing pages wholly past the end and zeroing the tail
 * of a partial last page. tn_rwlock is held for update.
 */
static int
tn_truncate(struct tmpnode *tn, off_t size)
{
        struct tmount *tm = tn->tn_mount;
        uint64_t keep = (uint64_t)(size + (off_t)PAGEOFFSET) / PAGESIZE;
        uint64_t n = pvn_trunc(TNTOV(tn), keep);
        struct page *pp;

        if (n != 0)
                atomic_add_64(&tm->tm_pages, -(int64_t)n);
        if ((size & (off_t)PAGEOFFSET) != 0 && keep > 0 &&
            (pp = page_lookup(TNTOV(tn), keep - 1)) != NULL) {
                size_t off = (size_t)(size & (off_t)PAGEOFFSET);

                memset((char *)page_to_kva(pp) + off, 0, PAGESIZE - off);
                page_rele(pp);
        }
        tn->tn_size = size;
        tn_touch(tn, TOUCH_M | TOUCH_C);
        return 0;
}

static int
tmpfs_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
tmpfs_close(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
tmpfs_access(struct vnode *vp, int mode, struct cred *cr)
{
        struct tmpnode *tn = VTOTN(vp);
        mode_t m = tn->tn_mode;

        if (CRED_ISROOT(cr)) {
                /* The superuser may execute only what is executable. */
                if ((mode & VEXEC) != 0 && tn->tn_type != VDIR &&
                    (m & 0111) == 0)
                        return EACCES;
                return 0;
        }
        if (cr->cr_uid == tn->tn_uid)
                m >>= 6;
        else if (cr->cr_gid == tn->tn_gid)
                m >>= 3;
        return ((m & 7) & mode) == (mode & 7) ? 0 : EACCES;
}

static int
tmpfs_getattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        struct tmpnode *tn = VTOTN(vp);
        struct tmount *tm = tn->tn_mount;

        mutex_lock(&tm->tm_lock);
        va->va_type = tn->tn_type;
        va->va_mode = tn->tn_mode;
        va->va_uid = tn->tn_uid;
        va->va_gid = tn->tn_gid;
        va->va_fsid = vp->v_vfsp->vfs_fsid;
        va->va_nodeid = tn->tn_ino;
        va->va_nlink = tn->tn_nlink;
        va->va_size = tn->tn_type == VDIR ? (off_t)tn->tn_dirlen + 2 :
            tn->tn_size;
        va->va_atime = tn->tn_atime;
        va->va_mtime = tn->tn_mtime;
        va->va_ctime = tn->tn_ctime;
        va->va_rdev = 0;
        va->va_blksize = PAGESIZE;
        va->va_nblocks = (uint64_t)pvn_npages(vp) * (PAGESIZE / 512);
        mutex_unlock(&tm->tm_lock);
        return 0;
}

static int
tmpfs_setattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        struct tmpnode *tn = VTOTN(vp);
        struct tmount *tm = tn->tn_mount;
        int error = 0;

        if ((va->va_mask & AT_SIZE) != 0) {
                if (tn->tn_type == VDIR)
                        return EISDIR;
                if (tn->tn_type != VREG)
                        return EINVAL;
                if (va->va_size < 0)
                        return EINVAL;
                /* Mapped pages are held; they cannot be cut away. */
                if (vp->v_nmap != 0)
                        return ETXTBSY;
        }
        mutex_lock(&tm->tm_lock);
        if ((va->va_mask & AT_MODE) != 0)
                tn->tn_mode = va->va_mode & 07777;
        if ((va->va_mask & AT_UID) != 0)
                tn->tn_uid = va->va_uid;
        if ((va->va_mask & AT_GID) != 0)
                tn->tn_gid = va->va_gid;
        if ((va->va_mask & AT_ATIME) != 0)
                tn->tn_atime = va->va_atime;
        if ((va->va_mask & AT_MTIME) != 0)
                tn->tn_mtime = va->va_mtime;
        if ((va->va_mask & AT_SIZE) != 0) {
                mrupdate(&tn->tn_rwlock);
                error = tn_truncate(tn, va->va_size);
                mrunlock(&tn->tn_rwlock);
        }
        tn_touch(tn, TOUCH_C);
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_lookup(struct vnode *dvp, const char *nm, struct vnode **vpp,
    struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(dvp);
        struct tmount *tm = dir->tn_mount;
        struct tdirent *de;
        int error = 0;

        if (dir->tn_type != VDIR)
                return ENOTDIR;
        mutex_lock(&tm->tm_lock);
        if (dir->tn_nlink == 0) {
                error = ENOENT;         /* removed directory */
        } else if (strcmp(nm, ".") == 0 || nm[0] == '\0') {
                *vpp = dvp;
                VN_HOLD(dvp);
        } else if (strcmp(nm, "..") == 0) {
                *vpp = TNTOV(dir->tn_parent);
                VN_HOLD(*vpp);
        } else if ((de = tdir_find(dir, nm, NULL)) != NULL) {
                *vpp = TNTOV(de->td_node);
                VN_HOLD(*vpp);
        } else {
                error = ENOENT;
        }
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_create(struct vnode *dvp, const char *nm, struct vattr *va, int excl,
    struct vnode **vpp, struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(dvp);
        struct tmount *tm = dir->tn_mount;
        struct tdirent *de;
        struct tmpnode *tn;
        int error;

        if (va->va_type != VREG && va->va_type != VFIFO)
                return EINVAL;
        if (is_dot(nm))
                return EEXIST;
        mutex_lock(&tm->tm_lock);
        if (dir->tn_nlink == 0) {
                mutex_unlock(&tm->tm_lock);
                return ENOENT;
        }
        if ((de = tdir_find(dir, nm, NULL)) != NULL) {
                tn = de->td_node;
                if (excl == EXCL)
                        error = EEXIST;
                else if (tn->tn_type == VDIR)
                        error = EISDIR;
                else {
                        error = 0;
                        *vpp = TNTOV(tn);
                        VN_HOLD(*vpp);
                }
                mutex_unlock(&tm->tm_lock);
                return error;
        }
        tn = tmpnode_alloc(tm, va->va_type, va->va_mode);
        if ((error = tdir_add(dir, nm, tn)) != 0) {
                tmpnode_free(tm, tn);
                mutex_unlock(&tm->tm_lock);
                return error;
        }
        tn->tn_nlink = 1;
        *vpp = TNTOV(tn);
        VN_HOLD(*vpp);
        mutex_unlock(&tm->tm_lock);
        return 0;
}

static int
tmpfs_remove(struct vnode *dvp, const char *nm, struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(dvp);
        struct tmount *tm = dir->tn_mount;
        struct tdirent *de, **link;
        struct tmpnode *tn;

        if (is_dot(nm))
                return EINVAL;
        mutex_lock(&tm->tm_lock);
        if ((de = tdir_find(dir, nm, &link)) == NULL) {
                mutex_unlock(&tm->tm_lock);
                return ENOENT;
        }
        tn = de->td_node;
        if (tn->tn_type == VDIR) {
                mutex_unlock(&tm->tm_lock);
                return EPERM;
        }
        tdir_unlink(dir, link);
        tn->tn_nlink--;
        tn_touch(tn, TOUCH_C);
        if (tn->tn_nlink == 0 && TNTOV(tn)->v_count == 0)
                tmpnode_free(tm, tn);
        mutex_unlock(&tm->tm_lock);
        return 0;
}

static int
tmpfs_link(struct vnode *tdvp, struct vnode *svp, const char *nm,
    struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(tdvp), *tn = VTOTN(svp);
        struct tmount *tm = dir->tn_mount;
        int error;

        if (tn->tn_type == VDIR)
                return EPERM;
        if (is_dot(nm))
                return EEXIST;
        mutex_lock(&tm->tm_lock);
        if (dir->tn_nlink == 0 || tn->tn_nlink == 0)
                error = ENOENT;
        else if (tdir_find(dir, nm, NULL) != NULL)
                error = EEXIST;
        else if ((error = tdir_add(dir, nm, tn)) == 0) {
                tn->tn_nlink++;
                tn_touch(tn, TOUCH_C);
        }
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_rename(struct vnode *sdvp, const char *snm, struct vnode *tdvp,
    const char *tnm, struct cred *cr __unused)
{
        struct tmpnode *sdir = VTOTN(sdvp), *tdir = VTOTN(tdvp);
        struct tmount *tm = sdir->tn_mount;
        struct tdirent *sde, *tde, **slink, **tlink;
        struct tmpnode *src, *dst = NULL;
        int error = 0;

        if (is_dot(snm) || is_dot(tnm))
                return EINVAL;
        mutex_lock(&tm->tm_lock);
        if ((sde = tdir_find(sdir, snm, &slink)) == NULL) {
                error = ENOENT;
                goto out;
        }
        src = sde->td_node;
        if (tdir->tn_nlink == 0) {
                error = ENOENT;
                goto out;
        }
        if ((tde = tdir_find(tdir, tnm, &tlink)) != NULL) {
                dst = tde->td_node;
                if (dst == src)
                        goto out;               /* the same file: nothing */
                if (src->tn_type == VDIR && dst->tn_type != VDIR) {
                        error = ENOTDIR;
                        goto out;
                }
                if (src->tn_type != VDIR && dst->tn_type == VDIR) {
                        error = EISDIR;
                        goto out;
                }
                if (dst->tn_type == VDIR && dst->tn_dirlen != 0) {
                        error = ENOTEMPTY;
                        goto out;
                }
                if (TNTOV(dst)->v_vfsmountedhere != NULL) {
                        error = EBUSY;
                        goto out;
                }
        }
        /* A directory may not move into its own subtree. */
        if (src->tn_type == VDIR) {
                for (struct tmpnode *p = tdir; ; p = p->tn_parent) {
                        if (p == src) {
                                error = EINVAL;
                                goto out;
                        }
                        if (p == tm->tm_root)
                                break;
                }
        }

        if (dst != NULL) {
                tdir_unlink(tdir, tlink);
                if (dst->tn_type == VDIR) {
                        dst->tn_nlink = 0;
                        tdir->tn_nlink--;
                } else {
                        dst->tn_nlink--;
                }
                tn_touch(dst, TOUCH_C);
        }
        /* tdir_find again: unlinking dst may have moved slink. */
        (void)tdir_find(sdir, snm, &slink);
        tdir_unlink(sdir, slink);
        if ((error = tdir_add(tdir, tnm, src)) != 0)
                panic("tmpfs_rename: cannot re-add %s", tnm);
        if (src->tn_type == VDIR && sdir != tdir) {
                src->tn_parent = tdir;
                sdir->tn_nlink--;
                tdir->tn_nlink++;
        }
        tn_touch(src, TOUCH_C);
        if (dst != NULL && dst->tn_nlink == 0 && TNTOV(dst)->v_count == 0)
                tmpnode_free(tm, dst);
out:
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_mkdir(struct vnode *dvp, const char *nm, struct vattr *va,
    struct vnode **vpp, struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(dvp);
        struct tmount *tm = dir->tn_mount;
        struct tmpnode *tn;
        int error;

        if (is_dot(nm))
                return EEXIST;
        mutex_lock(&tm->tm_lock);
        if (dir->tn_nlink == 0) {
                error = ENOENT;
        } else if (tdir_find(dir, nm, NULL) != NULL) {
                error = EEXIST;
        } else {
                tn = tmpnode_alloc(tm, VDIR, va->va_mode);
                if ((error = tdir_add(dir, nm, tn)) != 0) {
                        tmpnode_free(tm, tn);
                } else {
                        tn->tn_parent = dir;
                        tn->tn_nlink = 2;
                        dir->tn_nlink++;
                        *vpp = TNTOV(tn);
                        VN_HOLD(*vpp);
                }
        }
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_rmdir(struct vnode *dvp, const char *nm, struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(dvp);
        struct tmount *tm = dir->tn_mount;
        struct tdirent *de, **link;
        struct tmpnode *tn;
        int error = 0;

        if (is_dot(nm))
                return EINVAL;
        mutex_lock(&tm->tm_lock);
        if ((de = tdir_find(dir, nm, &link)) == NULL) {
                error = ENOENT;
        } else if ((tn = de->td_node)->tn_type != VDIR) {
                error = ENOTDIR;
        } else if (tn->tn_dirlen != 0) {
                error = ENOTEMPTY;
        } else {
                tdir_unlink(dir, link);
                dir->tn_nlink--;
                tn->tn_nlink = 0;
                tn_touch(tn, TOUCH_C);
                if (TNTOV(tn)->v_count == 0)
                        tmpnode_free(tm, tn);
        }
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_readdir(struct vnode *vp, struct uio *uio, int *eofp,
    struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(vp);
        struct tmount *tm = dir->tn_mount;
        char rec[DIRENT_RECLEN(MAXNAMELEN)];
        struct tdirent *de;
        off_t idx = uio->uio_offset, pos = 2;
        int error = 0;

        if (dir->tn_type != VDIR)
                return ENOTDIR;
        mutex_lock(&tm->tm_lock);
        de = dir->tn_dirents;
        while (pos < idx && de != NULL) {
                de = de->td_next;
                pos++;
        }
        *eofp = 0;
        for (;;) {
                struct dirent *d = (struct dirent *)(void *)rec;
                const char *name;
                size_t len, reclen;
                ino_t ino;

                if (idx == 0) {
                        name = ".";
                        ino = dir->tn_ino;
                } else if (idx == 1) {
                        name = "..";
                        ino = dir->tn_parent->tn_ino;
                } else if (de != NULL) {
                        name = de->td_name;
                        ino = de->td_node->tn_ino;
                } else {
                        *eofp = 1;
                        break;
                }
                len = strlen(name);
                reclen = DIRENT_RECLEN(len);
                if ((ssize_t)reclen > uio->uio_resid)
                        break;
                memset(rec, 0, reclen);
                d->d_ino = ino;
                d->d_off = idx + 1;
                d->d_reclen = (uint16_t)reclen;
                memcpy(d->d_name, name, len + 1);
                if ((error = uiomove(rec, reclen, UIO_READ, uio)) != 0)
                        break;
                /* The uio offset is the position, not a byte count. */
                uio->uio_offset = idx + 1;
                if (idx >= 2)
                        de = de->td_next;
                idx++;
        }
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_symlink(struct vnode *dvp, const char *nm, struct vattr *va __unused,
    const char *target, struct cred *cr __unused)
{
        struct tmpnode *dir = VTOTN(dvp);
        struct tmount *tm = dir->tn_mount;
        size_t len = strlen(target);
        struct tmpnode *tn;
        int error;

        if (is_dot(nm))
                return EEXIST;
        mutex_lock(&tm->tm_lock);
        if (dir->tn_nlink == 0) {
                error = ENOENT;
        } else if (tdir_find(dir, nm, NULL) != NULL) {
                error = EEXIST;
        } else {
                tn = tmpnode_alloc(tm, VLNK, 0777);
                tn->tn_link = kmem_alloc(len + 1, KM_SLEEP);
                memcpy(tn->tn_link, target, len + 1);
                tn->tn_linklen = len;
                tn->tn_size = (off_t)len;
                if ((error = tdir_add(dir, nm, tn)) != 0)
                        tmpnode_free(tm, tn);
                else
                        tn->tn_nlink = 1;
        }
        mutex_unlock(&tm->tm_lock);
        return error;
}

static int
tmpfs_readlink(struct vnode *vp, struct uio *uio, struct cred *cr __unused)
{
        struct tmpnode *tn = VTOTN(vp);

        if (tn->tn_type != VLNK)
                return EINVAL;
        return uiomove(tn->tn_link, tn->tn_linklen, UIO_READ, uio);
}

static int
tmpfs_read(struct vnode *vp, struct uio *uio, int ioflag __unused,
    struct cred *cr __unused)
{
        struct tmpnode *tn = VTOTN(vp);
        int error = 0;

        if (tn->tn_type == VDIR)
                return EISDIR;
        if (tn->tn_type != VREG)
                return EINVAL;
        if (uio->uio_offset < 0)
                return EINVAL;
        mraccess(&tn->tn_rwlock);
        while (uio->uio_resid > 0 && uio->uio_offset < tn->tn_size) {
                size_t pg = (size_t)(uio->uio_offset / (off_t)PAGESIZE);
                size_t off = (size_t)(uio->uio_offset & (off_t)PAGEOFFSET);
                size_t n = PAGESIZE - off;
                const char *src = zero_page;    /* for a hole */
                struct page *pp;

                if ((off_t)n > tn->tn_size - uio->uio_offset)
                        n = (size_t)(tn->tn_size - uio->uio_offset);
                if ((ssize_t)n > uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if ((pp = page_lookup(vp, pg)) != NULL)
                        src = (const char *)page_to_kva(pp) + off;
                error = uiomove((void *)(uintptr_t)src, n, UIO_READ, uio);
                if (pp != NULL)
                        page_rele(pp);
                if (error != 0)
                        break;
        }
        mrunlock(&tn->tn_rwlock);
        return error;
}

/*
 * Write, allocating pages as needed. Running out of space part way is a
 * short write; it is an error only if nothing was written.
 */
static int
tmpfs_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct tmpnode *tn = VTOTN(vp);
        struct tmount *tm = tn->tn_mount;
        ssize_t start = uio->uio_resid;
        int error = 0;

        if (tn->tn_type != VREG)
                return EINVAL;
        if (uio->uio_offset < 0)
                return EINVAL;
        mrupdate(&tn->tn_rwlock);
        if ((ioflag & IO_APPEND) != 0)
                uio->uio_offset = tn->tn_size;
        while (uio->uio_resid > 0) {
                size_t pg = (size_t)(uio->uio_offset / (off_t)PAGESIZE);
                size_t off = (size_t)(uio->uio_offset & (off_t)PAGEOFFSET);
                size_t n = PAGESIZE - off;
                struct page *pp;
                int isnew;

                if ((ssize_t)n > uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if (uio->uio_offset >= uio->uio_limit) {
                        error = EFBIG;
                        break;
                }
                if ((off_t)n > uio->uio_limit - uio->uio_offset)
                        n = (size_t)(uio->uio_limit - uio->uio_offset);
                if (pg >= TMPFS_MAXFILEPAGES) {
                        error = EFBIG;
                        break;
                }
                if ((pp = page_lookup_create(vp, pg, &isnew)) == NULL) {
                        error = ENOSPC;
                        break;
                }
                if (isnew) {
                        if (atomic_inc_64_nv(&tm->tm_pages) >
                            tm->tm_maxpages) {
                                atomic_add_64(&tm->tm_pages, -1);
                                page_abort(pp);
                                error = ENOSPC;
                                break;
                        }
                        memset(page_to_kva(pp), 0, PAGESIZE);
                        page_unbusy(pp);
                }
                error = uiomove((char *)page_to_kva(pp) + off, n, UIO_WRITE,
                    uio);
                page_rele(pp);
                if (error != 0)
                        break;
                if (uio->uio_offset > tn->tn_size)
                        tn->tn_size = uio->uio_offset;
        }
        if (uio->uio_resid != start) {
                tn_touch(tn, TOUCH_M | TOUCH_C);
                if (error == ENOSPC || error == EFBIG)
                        error = 0;
        }
        mrunlock(&tn->tn_rwlock);
        return error;
}

static int
tmpfs_fsync(struct vnode *vp __unused, struct cred *cr __unused)
{
        return 0;
}

/* The last reference went: free the node if it has no names left. */
static int
tmpfs_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct tmpnode *tn = VTOTN(vp);
        struct tmount *tm = tn->tn_mount;

        mutex_lock(&tm->tm_lock);
        if (vp->v_count == 0 && tn->tn_nlink == 0)
                tmpnode_free(tm, tn);
        else
                vn_inactive_done(vp);
        mutex_unlock(&tm->tm_lock);
        return VN_INACTIVE_NOCACHE;     /* ignored: the vnode is private */
}

/* Pages in memory are all there is: nothing to write back. */
static int
tmpfs_putpage(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

/*
 * A page of the file for a mapping, held. A hole gets a page of zeroes,
 * counted against the mount like a written one.
 */
static int
tmpfs_getpage(struct vnode *vp, off_t off, struct page **ppp,
    struct cred *cr __unused)
{
        struct tmpnode *tn = VTOTN(vp);
        struct tmount *tm = tn->tn_mount;
        struct page *pp;
        int isnew, error = 0;

        if (tn->tn_type != VREG)
                return ENODEV;
        mraccess(&tn->tn_rwlock);
        if (off < 0 || off >= tn->tn_size) {
                error = EFAULT;
        } else if ((pp = page_lookup_create(vp, (uint64_t)off / PAGESIZE,
            &isnew)) == NULL) {
                error = ENOMEM;
        } else {
                if (isnew) {
                        if (atomic_inc_64_nv(&tm->tm_pages) >
                            tm->tm_maxpages) {
                                atomic_add_64(&tm->tm_pages, -1);
                                page_abort(pp);
                                mrunlock(&tn->tn_rwlock);
                                return ENOSPC;
                        }
                        memset(page_to_kva(pp), 0, PAGESIZE);
                        page_unbusy(pp);
                }
                *ppp = pp;
        }
        mrunlock(&tn->tn_rwlock);
        return error;
}

/* Private vnodes are never reclaimed. */
static int
tmpfs_reclaim(struct vnode *vp __unused, int flags __unused)
{
        return EBUSY;
}

const struct vnodeops tmpfs_vnodeops = {
        tmpfs_open,
        tmpfs_close,
        tmpfs_read,
        tmpfs_write,
        tmpfs_getattr,
        tmpfs_setattr,
        tmpfs_access,
        tmpfs_lookup,
        tmpfs_create,
        tmpfs_remove,
        tmpfs_link,
        tmpfs_rename,
        tmpfs_mkdir,
        tmpfs_rmdir,
        tmpfs_readdir,
        tmpfs_symlink,
        tmpfs_readlink,
        tmpfs_fsync,
        tmpfs_inactive,
        tmpfs_putpage,
        tmpfs_reclaim,
        tmpfs_getpage,
        fs_poll
};
