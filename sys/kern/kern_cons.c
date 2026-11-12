/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Console output switch. Output before cninit() is discarded.
 *
 * There is no locking yet: only the bootstrap processor runs. A console
 * spinlock is added together with the synchronisation primitives.
 *
 * Processes reach the console through cons_vnode(), a private vnode
 * outside any file system that proc_spawn() opens on descriptors 0 to 2
 * until there are device special files. Writes go out through printf,
 * whose lock keeps them from interleaving with kernel messages within a
 * chunk; reads see end of file, since there is no console input yet.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cons.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

static struct consdev *cn_tab;

void
cninit(struct consdev *cp)
{
        cn_tab = cp;
}

/* Write one character, expanding a newline to carriage return and newline. */
void
cnputc(int c)
{
        if (cn_tab == NULL)
                return;
        if (c == '\n')
                cn_tab->cn_putc(cn_tab, '\r');
        cn_tab->cn_putc(cn_tab, c);
}

/* Wait until everything written to the console has left the device. */
void
cnflush(void)
{
        if (cn_tab != NULL && cn_tab->cn_flush != NULL)
                cn_tab->cn_flush(cn_tab);
}

/* --- The console as a vnode. --- */

#define CONS_CHUNK      128

static int
cons_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
cons_close(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
cons_read(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return 0;
}

static int
cons_write(struct vnode *vp __unused, struct uio *uio, int ioflag __unused,
    struct cred *cr __unused)
{
        char buf[CONS_CHUNK];

        while (uio->uio_resid > 0) {
                size_t n = uio->uio_resid < CONS_CHUNK ?
                    (size_t)uio->uio_resid : CONS_CHUNK;
                int error = uiomove(buf, n, UIO_WRITE, uio);

                if (error != 0)
                        return error;
                printf("%.*s", (int)n, buf);
        }
        return 0;
}

static int
cons_getattr(struct vnode *vp __unused, struct vattr *va,
    struct cred *cr __unused)
{
        memset(va, 0, sizeof(*va));
        va->va_type = VCHR;
        va->va_mode = 0620;
        va->va_nlink = 1;
        va->va_blksize = CONS_CHUNK;
        return 0;
}

static int
cons_setattr(struct vnode *vp __unused, struct vattr *va __unused,
    struct cred *cr __unused)
{
        return EPERM;
}

static int
cons_access(struct vnode *vp __unused, int mode __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
cons_lookup(struct vnode *dvp __unused, const char *nm __unused,
    struct vnode **vpp __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_create(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, int excl __unused, struct vnode **vpp __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_name1(struct vnode *dvp __unused, const char *nm __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_link(struct vnode *tdvp __unused, struct vnode *vp __unused,
    const char *nm __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_rename(struct vnode *sdvp __unused, const char *snm __unused,
    struct vnode *tdvp __unused, const char *tnm __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_mkdir(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, struct vnode **vpp __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_readdir(struct vnode *vp __unused, struct uio *uio __unused,
    int *eofp __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_symlink(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, const char *target __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
cons_readlink(struct vnode *vp __unused, struct uio *uio __unused,
    struct cred *cr __unused)
{
        return EINVAL;
}

static int
cons_fsync(struct vnode *vp __unused, struct cred *cr __unused)
{
        cnflush();
        return 0;
}

/* The console vnode is never freed: always held by cons_vnode(). */
static int
cons_inactive(struct vnode *vp, struct cred *cr __unused)
{
        vn_inactive_done(vp);
        return VN_INACTIVE_CACHE;
}

static int
cons_putpage(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
cons_reclaim(struct vnode *vp __unused, int flags __unused)
{
        return EBUSY;
}

static int
cons_getpage(struct vnode *vp __unused, off_t off __unused,
    struct page **ppp __unused, struct cred *cr __unused)
{
        return ENODEV;
}

static const struct vnodeops cons_vnodeops = {
        cons_open, cons_close, cons_read, cons_write, cons_getattr,
        cons_setattr, cons_access, cons_lookup, cons_create, cons_name1,
        cons_link, cons_rename, cons_mkdir, cons_name1, cons_readdir,
        cons_symlink, cons_readlink, cons_fsync, cons_inactive,
        cons_putpage, cons_reclaim, cons_getpage, fs_poll
};

static struct vnode cons_vn;
static int cons_vn_ready;

/*
 * The console vnode, held for the caller. proc_init() makes the first
 * call, before anything can race to set it up.
 */
struct vnode *
cons_vnode(void)
{
        if (!cons_vn_ready) {
                vn_init(&cons_vn, NULL, VCHR, &cons_vnodeops, NULL);
                VN_HOLD(&cons_vn);      /* never released */
                cons_vn_ready = 1;
        }
        VN_HOLD(&cons_vn);
        return &cons_vn;
}
