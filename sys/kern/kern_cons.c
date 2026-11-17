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
 * until there are device special files. It is a terminal (cons_tty, see
 * kern/tty.c): the line discipline processes what is written and goes
 * out through cnwrite(), under printf's lock, so that a write does not
 * interleave with kernel messages within a chunk; the device driver
 * passes what it receives to cons_input().
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cons.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/mkdev.h>
#include <sys/specvn.h>
#include <sys/stat.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/vnode.h>

static struct consdev *cn_tab;
static struct tty cons_tty;
static int cons_tty_ready;
static unsigned int cons_opens;         /* protected by cons_tty.t_lock */

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

/* Write one character as it is (cnwrite(), under printf's lock). */
void
cnputc_raw(int c)
{
        if (cn_tab != NULL)
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
cons_open(struct vnode **vpp __unused, int flags, struct cred *cr __unused)
{
        mutex_lock(&cons_tty.t_lock);
        if (cons_opens++ == 0)
                cons_tty.t_hungup = 0;
        mutex_unlock(&cons_tty.t_lock);
        tty_open(&cons_tty, flags);
        return 0;
}

static int
cons_close(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        mutex_lock(&cons_tty.t_lock);
        cons_opens--;
        mutex_unlock(&cons_tty.t_lock);
        return 0;
}

static int
cons_read(struct vnode *vp __unused, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        return tty_read(&cons_tty, uio, ioflag);
}

static int
cons_write(struct vnode *vp __unused, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        return tty_write(&cons_tty, uio, ioflag);
}

static int
cons_ioctl(struct vnode *vp __unused, unsigned long cmd, uint64_t arg,
    int flag, struct cred *cr)
{
        return tty_ioctl(&cons_tty, cmd, arg, flag, cr);
}

static int
cons_poll(struct vnode *vp __unused, short events, short *reventsp,
    struct pollhead **phpp, struct cred *cr __unused)
{
        return tty_poll(&cons_tty, events, reventsp, phpp);
}

/* The terminal's output: as it is, the discipline has processed it. */
static void
cons_output(struct tty *tp __unused, const char *buf, size_t n)
{
        cnwrite(buf, n);
}

/* A character the console device received. */
void
cons_input(int c)
{
        if (cons_tty_ready)
                tty_input(&cons_tty, c);
}

void
cons_disconnect(void)
{
        if (cons_tty_ready)
                tty_modem_disconnect(&cons_tty);
}

/*
 * The console's owner, group and mode (cons_attr, the vnode's v_data):
 * root's, 0620, until login gives it to whoever logs in on it. They are
 * kept and checked as a device node's are (kern/spec_vnops.c).
 */
static struct specattr cons_attr;

static int
cons_getattr(struct vnode *vp, struct vattr *va, struct cred *cr)
{
        int error;

        if ((error = spec_getattr(vp, va, cr)) == 0)
                va->va_blksize = CONS_CHUNK;
        return error;
}

static int
cons_setattr(struct vnode *vp, struct vattr *va, struct cred *cr)
{
        return spec_setattr(vp, va, cr);
}

static int
cons_access(struct vnode *vp, int mode, struct cred *cr)
{
        return spec_access(vp, mode, cr);
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
        cons_putpage, cons_reclaim, cons_getpage, cons_poll, cons_ioctl
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
                tty_init(&cons_tty);
                cons_tty.t_termios.c_cflag |= CLOCAL;
                cons_tty.t_output = cons_output;
                cons_tty.t_vnode = &cons_vn;
                cons_tty_ready = 1;
                specattr_init(&cons_attr, 0620, 0, 0,
                    makedev(DEV_CONS_MAJOR, 0));
                cons_attr.sa_ino = 1;
                vn_init(&cons_vn, NULL, VCHR, &cons_vnodeops, &cons_attr);
                VN_HOLD(&cons_vn);      /* never released */
                cons_vn_ready = 1;
        }
        VN_HOLD(&cons_vn);
        return &cons_vn;
}

/* An open file on the console for a kernel-started process. */
struct vnode *
cons_open_vnode(void)
{
        struct vnode *vp = cons_vnode();

        (void)cons_open(&vp, FNOCTTY, NULL);
        return vp;
}
