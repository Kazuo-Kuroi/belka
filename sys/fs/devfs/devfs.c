/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * devfs: the device names, mounted on /dev, until there are device
 * special files on disk. A fixed directory:
 *
 *   console    the console terminal (kern_cons.c)
 *   null       reads end of file, takes every write
 *   zero       reads zeros, takes every write
 *   tty        opens the caller's controlling terminal (ENXIO if none)
 *   ptmx       opens the master of a new pseudo-terminal (kern/tty_pty.c)
 *   pts/       pts/N: each pseudo-terminal slave whose master is open
 *   mnttab     the mount table, as text (vfs_mnttab); /etc/mnttab links
 *              here, as Solaris makes the table the kernel's
 *   vd0, ...   each disk (blkdev), a block device that mount(2) is given;
 *              it cannot be read or written through yet
 *
 * Its vnodes are private and static but for the console's and the
 * slaves', which belong to their drivers, so it is mounted once.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/cons.h>
#include <sys/cred.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/mkdev.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/pty.h>
#include <sys/random.h>
#include <sys/specvn.h>
#include <sys/strsubr.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

enum { DV_ROOT, DV_PTS, DV_NULL, DV_ZERO, DV_TTY, DV_PTMX, DV_MNTTAB,
    DV_RANDOM, DV_URANDOM, DV_N };

static struct specattr dv_attr[DV_N];
static struct vnode dv_vn[DV_N];
/* The disks' nodes, made when devfs is mounted, after the disks are found. */
static struct specattr dv_blkattr[BLKDEV_MAX];
static struct vnode dv_blkvn[BLKDEV_MAX];
static int dv_nblk;

/* A clone node for each STREAMS device (strdevsw), made at mount. */
static struct specattr dv_strattr[STRDEV_MAX];
static struct vnode dv_strvn[STRDEV_MAX];
static int dv_nstr;
static struct vfs *devfs_vfs;

static const struct vnodeops devfs_dirops, devfs_memops, devfs_cloneops,
    devfs_mntops, devfs_blkops;

/* The root's names, in order; NULL: the console's own vnode. */
static const struct {
        const char      *name;
        int             node;           /* -1: the console */
} devfs_names[] = {
        { "console", -1 },
        { "null", DV_NULL },
        { "zero", DV_ZERO },
        { "random", DV_RANDOM },
        { "urandom", DV_URANDOM },
        { "tty", DV_TTY },
        { "ptmx", DV_PTMX },
        { "mnttab", DV_MNTTAB },
        { "pts", DV_PTS },
};
#define NNAMES  (sizeof(devfs_names) / sizeof(devfs_names[0]))

static int
devfs_mount(struct vfs *vfsp, const void *data __unused,
    struct cred *cr __unused)
{
        static const struct {
                enum vtype      type;
                mode_t          mode;
                dev_t           rdev;
                const struct vnodeops *ops;
        } setup[DV_N] = {
                [DV_ROOT] = { VDIR, 0755, 0, &devfs_dirops },
                [DV_PTS] = { VDIR, 0755, 0, &devfs_dirops },
                [DV_NULL] = { VCHR, 0666, makedev(DEV_MEM_MAJOR, 0),
                    &devfs_memops },
                [DV_ZERO] = { VCHR, 0666, makedev(DEV_MEM_MAJOR, 1),
                    &devfs_memops },
                [DV_RANDOM] = { VCHR, 0666, makedev(DEV_MEM_MAJOR, 8),
                    &devfs_memops },
                [DV_URANDOM] = { VCHR, 0666, makedev(DEV_MEM_MAJOR, 9),
                    &devfs_memops },
                [DV_TTY] = { VCHR, 0666, makedev(DEV_TTY_MAJOR, 0),
                    &devfs_cloneops },
                [DV_PTMX] = { VCHR, 0666, makedev(DEV_PTM_MAJOR, 0xffff),
                    &devfs_cloneops },
                [DV_MNTTAB] = { VREG, 0444, 0, &devfs_mntops },
        };

        if (devfs_vfs != NULL)
                return EBUSY;
        devfs_vfs = vfsp;
        for (int i = 0; i < DV_N; i++) {
                specattr_init(&dv_attr[i], setup[i].mode, 0, 0,
                    setup[i].rdev);
                vn_init(&dv_vn[i], vfsp, setup[i].type, setup[i].ops,
                    &dv_attr[i]);
                VN_HOLD(&dv_vn[i]);     /* never released */
        }
        for (dv_nblk = 0; dv_nblk < BLKDEV_MAX; dv_nblk++) {
                struct blkdev *bd = blkdev_index(dv_nblk);

                if (bd == NULL)
                        break;
                specattr_init(&dv_blkattr[dv_nblk], 0600, 0, 0,
                    makedev(DEV_BLK_MAJOR, (uint32_t)dv_nblk));
                vn_init(&dv_blkvn[dv_nblk], vfsp, VBLK, &devfs_blkops,
                    &dv_blkattr[dv_nblk]);
                VN_HOLD(&dv_blkvn[dv_nblk]);   /* never released */
        }
        for (dv_nstr = 0; dv_nstr < nstrdev; dv_nstr++) {
                struct strdev *sd = &strdevsw[dv_nstr];

                specattr_init(&dv_strattr[dv_nstr], sd->d_mode, 0, 0,
                    makedev(sd->d_major, sd->d_unit << 24));
                vn_init(&dv_strvn[dv_nstr], vfsp, VCHR, &devfs_cloneops,
                    &dv_strattr[dv_nstr]);
                VN_HOLD(&dv_strvn[dv_nstr]);   /* never released */
        }
        dv_vn[DV_ROOT].v_flag |= VROOT;
        pty_setvfs(vfsp);
        vfsp->vfs_bsize = PAGESIZE;
        return 0;
}

static int
devfs_unmount(struct vfs *vfsp __unused, struct cred *cr __unused)
{
        return EBUSY;
}

static int
devfs_root(struct vfs *vfsp __unused, struct vnode **vpp)
{
        *vpp = &dv_vn[DV_ROOT];
        VN_HOLD(*vpp);
        return 0;
}

static int
devfs_statvfs(struct vfs *vfsp, struct statvfs *sp)
{
        memset(sp, 0, sizeof(*sp));
        sp->f_bsize = sp->f_frsize = PAGESIZE;
        sp->f_fsid = vfsp->vfs_fsid;
        memcpy(sp->f_basetype, "devfs", 6);
        sp->f_namemax = MAXNAMELEN - 1;
        return 0;
}

static int
devfs_sync(struct vfs *vfsp __unused, struct cred *cr __unused)
{
        return 0;
}

const struct vfsops devfs_vfsops = {
        devfs_mount, devfs_unmount, devfs_root, devfs_statvfs, devfs_sync
};

/* --- The directories. --- */

static int
devfs_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
devfs_close(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
devfs_dirrw(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return EISDIR;
}

static int
devfs_lookup(struct vnode *dvp, const char *nm, struct vnode **vpp,
    struct cred *cr __unused)
{
        if (strcmp(nm, ".") == 0) {
                *vpp = dvp;
                VN_HOLD(dvp);
                return 0;
        }
        if (dvp == &dv_vn[DV_PTS]) {
                unsigned int n = 0;

                if (strcmp(nm, "..") == 0) {
                        *vpp = &dv_vn[DV_ROOT];
                        VN_HOLD(*vpp);
                        return 0;
                }
                for (const char *c = nm; *c != '\0'; c++) {
                        if (*c < '0' || *c > '9' || n > PTY_MAX ||
                            (c == nm && *c == '0' && c[1] != '\0'))
                                return ENOENT;
                        n = n * 10 + (unsigned int)(*c - '0');
                }
                return pty_lookup(n, vpp);
        }
        for (size_t i = 0; i < NNAMES; i++) {
                if (strcmp(nm, devfs_names[i].name) != 0)
                        continue;
                *vpp = devfs_names[i].node < 0 ? cons_vnode() :
                    &dv_vn[devfs_names[i].node];
                if (devfs_names[i].node >= 0)
                        VN_HOLD(*vpp);
                return 0;
        }
        for (int i = 0; i < dv_nblk; i++) {
                if (strcmp(nm, blkdev_index(i)->bd_name) == 0) {
                        *vpp = &dv_blkvn[i];
                        VN_HOLD(*vpp);
                        return 0;
                }
        }
        for (int i = 0; i < dv_nstr; i++) {
                if (strcmp(nm, strdevsw[i].d_name) == 0) {
                        *vpp = &dv_strvn[i];
                        VN_HOLD(*vpp);
                        return 0;
                }
        }
        return ENOENT;
}

/* Put one record at position idx; 0, or -1 if it does not fit. */
static int
devfs_dirent(struct uio *uio, off_t idx, ino_t ino, const char *name,
    int *errorp)
{
        char rec[DIRENT_RECLEN(MAXNAMELEN)];
        struct dirent *d = (struct dirent *)(void *)rec;
        size_t len = strlen(name), reclen = DIRENT_RECLEN(len);

        if ((ssize_t)reclen > uio->uio_resid)
                return -1;
        memset(rec, 0, reclen);
        d->d_ino = ino;
        d->d_off = idx + 1;
        d->d_reclen = (uint16_t)reclen;
        memcpy(d->d_name, name, len + 1);
        if ((*errorp = uiomove(rec, reclen, UIO_READ, uio)) != 0)
                return -1;
        uio->uio_offset = idx + 1;
        return 0;
}

/*
 * Positions: 0 ".", 1 "..", then the root's names, or in pts the slave
 * of index position - 2 if its master is open.
 */
static int
devfs_readdir(struct vnode *vp, struct uio *uio, int *eofp,
    struct cred *cr __unused)
{
        const struct specattr *sa = vp->v_data;
        int error = 0;

        *eofp = 0;
        for (off_t idx = uio->uio_offset; error == 0; idx++) {
                char name[16];
                ino_t ino;
                unsigned int n;

                if (idx == 0) {
                        snprintf(name, sizeof(name), ".");
                        ino = sa->sa_ino;
                } else if (idx == 1) {
                        snprintf(name, sizeof(name), "..");
                        ino = dv_attr[DV_ROOT].sa_ino;
                } else if (vp == &dv_vn[DV_ROOT]) {
                        size_t k = (size_t)(idx - 2);

                        if (k >= NNAMES + (size_t)dv_nblk + (size_t)dv_nstr) {
                                *eofp = 1;
                                break;
                        }
                        if (k >= NNAMES + (size_t)dv_nblk) {
                                k -= NNAMES + (size_t)dv_nblk;
                                snprintf(name, sizeof(name), "%s",
                                    strdevsw[k].d_name);
                                ino = dv_strattr[k].sa_ino;
                        } else if (k >= NNAMES) {
                                k -= NNAMES;
                                snprintf(name, sizeof(name), "%s",
                                    blkdev_index((int)k)->bd_name);
                                ino = dv_blkattr[k].sa_ino;
                        } else {
                                snprintf(name, sizeof(name), "%s",
                                    devfs_names[k].name);
                                ino = devfs_names[k].node < 0 ? 1 :
                                    dv_attr[devfs_names[k].node].sa_ino;
                        }
                } else {
                        if (pty_next((unsigned int)(idx - 2), &n) != 0) {
                                *eofp = 1;
                                break;
                        }
                        idx = (off_t)n + 2;
                        snprintf(name, sizeof(name), "%u", n);
                        ino = 1000 + n;
                }
                if (devfs_dirent(uio, idx, ino, name, &error) != 0)
                        break;
        }
        return error;
}

static int
devfs_inactive(struct vnode *vp, struct cred *cr __unused)
{
        vn_inactive_done(vp);
        return VN_INACTIVE_CACHE;
}

static const struct vnodeops devfs_dirops = {
        devfs_open, devfs_close, devfs_dirrw, devfs_dirrw, spec_getattr,
        spec_setattr, spec_access, devfs_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, devfs_readdir,
        spec_symlink, spec_readlink, spec_fsync, devfs_inactive,
        spec_putpage, spec_reclaim, spec_getpage, fs_poll, NULL
};

/* --- null, zero, random and urandom (kern/kern_random.c). --- */

static int
mem_read(struct vnode *vp, struct uio *uio, int ioflag __unused,
    struct cred *cr __unused)
{
        static const char zeros[512];
        int error = 0;

        if (vp == &dv_vn[DV_NULL])
                return 0;
        if (vp == &dv_vn[DV_RANDOM] || vp == &dv_vn[DV_URANDOM])
                return rnd_read(uio);
        while (uio->uio_resid > 0 && error == 0) {
                size_t n = uio->uio_resid < (ssize_t)sizeof(zeros) ?
                    (size_t)uio->uio_resid : sizeof(zeros);

                error = uiomove((void *)(uintptr_t)zeros, n, UIO_READ, uio);
        }
        return error;
}

static int
mem_write(struct vnode *vp, struct uio *uio, int ioflag __unused,
    struct cred *cr __unused)
{
        if (vp == &dv_vn[DV_RANDOM] || vp == &dv_vn[DV_URANDOM])
                return rnd_write(uio);
        uio->uio_offset += uio->uio_resid;
        while (uio->uio_iovcnt > 0) {
                uio->uio_iov->iov_base = (char *)uio->uio_iov->iov_base +
                    uio->uio_iov->iov_len;
                uio->uio_iov->iov_len = 0;
                uio->uio_iov++;
                uio->uio_iovcnt--;
        }
        uio->uio_resid = 0;
        return 0;
}

static const struct vnodeops devfs_memops = {
        devfs_open, devfs_close, mem_read, mem_write, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, devfs_inactive,
        spec_putpage, spec_reclaim, spec_getpage, fs_poll, NULL
};

/* --- tty and ptmx: an open makes the file another vnode's. --- */

static int
clone_open(struct vnode **vpp, int flags, struct cred *cr)
{
        struct vnode *vp;

        if (*vpp == &dv_vn[DV_PTMX])
                return ptm_clone(vpp, flags, cr);
        if (*vpp >= &dv_strvn[0] && *vpp < &dv_strvn[dv_nstr])
                return str_clone(&strdevsw[*vpp - &dv_strvn[0]], vpp, flags,
                    cr);
        /* /dev/tty: the session's terminal, opened in its own right. */
        if ((vp = tty_ctty_vnode(curproc->p_sid)) == NULL)
                return ENXIO;
        VN_RELE(*vpp);
        *vpp = vp;
        return VOP_OPEN(vpp, flags, cr);
}

static int
clone_rw(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return ENXIO;           /* never open as itself */
}

static const struct vnodeops devfs_cloneops = {
        clone_open, devfs_close, clone_rw, clone_rw, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, devfs_inactive,
        spec_putpage, spec_reclaim, spec_getpage, fs_poll, NULL
};

/* --- mnttab: the mount table, made afresh for each read. --- */

static int
mnt_read(struct vnode *vp __unused, struct uio *uio, int ioflag __unused,
    struct cred *cr __unused)
{
        size_t len = vfs_mnttab(NULL, 0), room = len + 1;
        char *buf;
        int error = 0;

        if (uio->uio_offset < 0)
                return EINVAL;
        buf = kmem_alloc(room, KM_SLEEP);
        /* A mount since the first call may make it longer: cut it there. */
        len = vfs_mnttab(buf, room);
        if (len > room)
                len = room;
        if ((uint64_t)uio->uio_offset < len) {
                size_t n = len - (size_t)uio->uio_offset;

                if ((ssize_t)n > uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                error = uiomove(buf + uio->uio_offset, n, UIO_READ, uio);
        }
        kmem_free(buf, room);
        return error;
}

static int
mnt_write(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return EROFS;
}

static const struct vnodeops devfs_mntops = {
        devfs_open, devfs_close, mnt_read, mnt_write, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, devfs_inactive,
        spec_putpage, spec_reclaim, spec_getpage, fs_poll, NULL
};

/* --- the disks. --- */

/* Not yet: a disk is only named, for mount(2). */
static int
blk_rw(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return ENODEV;
}

static const struct vnodeops devfs_blkops = {
        devfs_open, devfs_close, blk_rw, blk_rw, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, devfs_inactive,
        spec_putpage, spec_reclaim, spec_getpage, fs_poll, NULL
};

/* The disk vp is devfs's node for, or NULL (mount(2), umount(2)). */
struct blkdev *
devfs_blkdev(struct vnode *vp)
{
        for (int i = 0; i < dv_nblk; i++)
                if (vp == &dv_blkvn[i])
                        return blkdev_index(i);
        return NULL;
}
