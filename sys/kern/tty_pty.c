/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Pseudo-terminals, after System V's ptm and pts (see <sys/pty.h>).
 *
 * A pair is made by opening /dev/ptmx (ptm_clone), which opens its
 * master; the slave, /dev/pts/N, is a terminal (a struct tty, with the
 * line discipline of kern/tty.c), and may be opened once unlockpt()
 * (UNLKPT) has been called on the master. What the slave's discipline
 * writes goes into a buffer the master reads; what the master writes is
 * the slave's input. Closing the last master file hangs the slave up
 * (tty_hangup: SIGHUP, end of file, EIO); with no slave open, a read of
 * the master sees end of file. The slave belongs to the real user and
 * group that opened the master, mode 0620, so grantpt() has nothing to
 * do. The master's device number has the pair's index as its minor,
 * from which the C library makes ptsname().
 *
 * pty_lock protects the table and every pair's open counts and
 * activity; a pair is freed once neither of its vnodes is referenced.
 * The master's buffer is under the slave terminal's t_lock, which its
 * discipline already holds when it writes.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dnlc.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/mkdev.h>
#include <sys/proc.h>
#include <sys/pty.h>
#include <sys/specvn.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define PTY_BUF         4096

struct pty {
        unsigned int    pt_index;
        struct specattr pt_mattr;       /* the master's v_data */
        struct specattr pt_sattr;       /* the slave's */
        struct vnode    pt_mvn;
        struct vnode    pt_svn;
        struct tty      pt_tty;         /* the slave terminal */

        /* Under pt_tty.t_lock. */
        char            pt_buf[PTY_BUF];
        size_t          pt_head;
        size_t          pt_count;
        struct sv       pt_msv;         /* the master can read */
        struct pollhead pt_mph;

        /* Under pty_lock (the counts also under t_lock where read). */
        int             pt_mopen;       /* master file opens */
        int             pt_sopen;       /* slave file opens */
        int             pt_sopened;     /* the slave was ever opened */
        int             pt_locked;      /* until UNLKPT */
        int             pt_mactive;     /* the vnodes are referenced */
        int             pt_sactive;
};

#define MVTOPT(vp)      ((struct pty *)(void *)((char *)(vp)->v_data - \
                            offsetof(struct pty, pt_mattr)))
#define SVTOPT(vp)      ((struct pty *)(void *)((char *)(vp)->v_data - \
                            offsetof(struct pty, pt_sattr)))

static struct mutex pty_lock;
static int pty_ready;
static struct pty *ptys[PTY_MAX];
static struct vfs *pty_vfs;

static const struct vnodeops ptm_vnodeops, pts_vnodeops;

/* devfs gives the file system slave vnodes belong to, when mounted. */
void
pty_setvfs(struct vfs *vfsp)
{
        if (!pty_ready) {
                mutex_init(&pty_lock, "pty");
                pty_ready = 1;
        }
        pty_vfs = vfsp;
}

static void
pty_free(struct pty *pt)
{
        ptys[pt->pt_index] = NULL;
        tty_destroy(&pt->pt_tty);
        dnlc_purge_vp(&pt->pt_svn);
        vn_destroy(&pt->pt_mvn);
        vn_destroy(&pt->pt_svn);
        kmem_free(pt, sizeof(*pt));
}

/* The slave discipline's output, into the master's buffer. */
static void
pty_output(struct tty *tp, const char *buf, size_t n)
{
        struct pty *pt = tp->t_softc;

        for (size_t i = 0; i < n && pt->pt_count < PTY_BUF; i++) {
                pt->pt_buf[(pt->pt_head + pt->pt_count) % PTY_BUF] = buf[i];
                pt->pt_count++;
        }
        sv_broadcast(&pt->pt_msv);
        pollwakeup(&pt->pt_mph);
}

static size_t
pty_outspace(struct tty *tp)
{
        struct pty *pt = tp->t_softc;

        return PTY_BUF - pt->pt_count;
}

/*
 * Open /dev/ptmx: make a pair, and replace *vpp (ptmx, held) by its
 * master, held and open.
 */
int
ptm_clone(struct vnode **vpp, int flags __unused, struct cred *cr)
{
        struct pty *pt;
        unsigned int i;

        if (!pty_ready)
                return ENXIO;
        pt = kmem_zalloc(sizeof(*pt), KM_SLEEP);
        mutex_lock(&pty_lock);
        for (i = 0; i < PTY_MAX && ptys[i] != NULL; i++)
                continue;
        if (i == PTY_MAX) {
                mutex_unlock(&pty_lock);
                kmem_free(pt, sizeof(*pt));
                return EAGAIN;
        }
        pt->pt_index = i;
        tty_init(&pt->pt_tty);
        pt->pt_tty.t_output = pty_output;
        pt->pt_tty.t_outspace = pty_outspace;
        pt->pt_tty.t_softc = pt;
        pt->pt_tty.t_vnode = &pt->pt_svn;
        sv_init(&pt->pt_msv, "ptm");
        pollhead_init(&pt->pt_mph);
        specattr_init(&pt->pt_mattr, 0620, cr->cr_ruid, cr->cr_rgid,
            makedev(DEV_PTM_MAJOR, i));
        specattr_init(&pt->pt_sattr, 0620, cr->cr_ruid, cr->cr_rgid,
            makedev(DEV_PTS_MAJOR, i));
        vn_init(&pt->pt_mvn, pty_vfs, VCHR, &ptm_vnodeops, &pt->pt_mattr);
        vn_init(&pt->pt_svn, pty_vfs, VCHR, &pts_vnodeops, &pt->pt_sattr);
        VN_HOLD(&pt->pt_mvn);
        pt->pt_mactive = 1;
        pt->pt_mopen = 1;
        pt->pt_locked = 1;
        ptys[i] = pt;
        mutex_unlock(&pty_lock);
        VN_RELE(*vpp);
        *vpp = &pt->pt_mvn;
        return 0;
}

/* Slave index, held, for /dev/pts; ENOENT once its master is closed. */
int
pty_lookup(unsigned int index, struct vnode **vpp)
{
        struct pty *pt;
        int error = ENOENT;

        if (!pty_ready || index >= PTY_MAX)
                return ENOENT;
        mutex_lock(&pty_lock);
        pt = ptys[index];
        if (pt != NULL && pt->pt_mopen != 0 &&
            vn_get(&pt->pt_svn, pt->pt_svn.v_number)) {
                pt->pt_sactive = 1;
                *vpp = &pt->pt_svn;
                error = 0;
        }
        mutex_unlock(&pty_lock);
        return error;
}

/* The first pair at or after from whose master is open: 0, or ENOENT. */
int
pty_next(unsigned int from, unsigned int *index)
{
        int error = ENOENT;

        if (!pty_ready)
                return ENOENT;
        mutex_lock(&pty_lock);
        for (unsigned int i = from; i < PTY_MAX; i++) {
                if (ptys[i] != NULL && ptys[i]->pt_mopen != 0) {
                        *index = i;
                        error = 0;
                        break;
                }
        }
        mutex_unlock(&pty_lock);
        return error;
}

/*
 * Sleep on sv, which t_lock is held for and is again on return: 0, or
 * EINTR or ERESTART for a signal.
 */
static int
pty_sleep(struct pty *pt, struct sv *sv)
{
        int r = 0;

        if (sv_wait_mutex_sig(sv, &pt->pt_tty.t_lock) != 0 &&
            (r = sig_sleep_check()) != 0)
                r = r == 1 ? EINTR : ERESTART;
        mutex_lock(&pt->pt_tty.t_lock);
        return r;
}

/* --- The master. --- */

static int
ptm_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

/* The last master file closed: the slave hangs up. */
static int
ptm_close(struct vnode *vp, int flags __unused, struct cred *cr __unused)
{
        struct pty *pt = MVTOPT(vp);
        int last;

        mutex_lock(&pty_lock);
        last = --pt->pt_mopen == 0;
        mutex_unlock(&pty_lock);
        if (last)
                tty_hangup(&pt->pt_tty);
        return 0;
}

static int
ptm_read(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct pty *pt = MVTOPT(vp);
        struct tty *tp = &pt->pt_tty;
        int error = 0;

        mutex_lock(&tp->t_lock);
        while (pt->pt_count == 0) {
                /* Every slave file closed again: end of file. */
                if (pt->pt_sopened && pt->pt_sopen == 0)
                        goto out;
                if ((ioflag & (IO_NONBLOCK | IO_NDELAY)) != 0) {
                        error = (ioflag & IO_NONBLOCK) != 0 ? EAGAIN : 0;
                        goto out;
                }
                if ((error = pty_sleep(pt, &pt->pt_msv)) != 0)
                        goto out;
        }
        while (pt->pt_count != 0 && uio->uio_resid > 0 && error == 0) {
                size_t n = PTY_BUF - pt->pt_head;

                if (n > pt->pt_count)
                        n = pt->pt_count;
                if ((ssize_t)n > uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                error = uiomove(pt->pt_buf + pt->pt_head, n, UIO_READ, uio);
                pt->pt_head = (pt->pt_head + n) % PTY_BUF;
                pt->pt_count -= n;
        }
        sv_broadcast(&tp->t_outsv);     /* the slave has room */
out:
        mutex_unlock(&tp->t_lock);
        return error;
}

/* Each character the master writes is a character the slave receives. */
static int
ptm_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct pty *pt = MVTOPT(vp);
        int error = 0;

        while (uio->uio_resid > 0) {
                char c;

                if ((error = tty_wait_room(&pt->pt_tty,
                    (ioflag & (IO_NONBLOCK | IO_NDELAY)) != 0)) != 0 ||
                    (error = uiomove(&c, 1, UIO_WRITE, uio)) != 0)
                        break;
                tty_input(&pt->pt_tty, (unsigned char)c);
        }
        return error;
}

/*
 * ISPTM and UNLKPT, and FIONREAD for the master's own buffer; the rest
 * are the slave terminal's (its modes, its window size, TIOCSTI).
 */
static int
ptm_ioctl(struct vnode *vp, unsigned long cmd, uint64_t arg, int flag,
    struct cred *cr)
{
        struct pty *pt = MVTOPT(vp);
        int32_t n;

        switch (cmd) {
        case ISPTM:
                return 0;
        case UNLKPT:
                mutex_lock(&pty_lock);
                pt->pt_locked = 0;
                mutex_unlock(&pty_lock);
                return 0;
        case FIONREAD:
                mutex_lock(&pt->pt_tty.t_lock);
                n = (int32_t)pt->pt_count;
                mutex_unlock(&pt->pt_tty.t_lock);
                return copyout(&n, (void *)(uintptr_t)arg, sizeof(n));
        default:
                return tty_ioctl(&pt->pt_tty, cmd, arg, flag, cr);
        }
}

static int
ptm_poll(struct vnode *vp, short events __unused, short *reventsp,
    struct pollhead **phpp, struct cred *cr __unused)
{
        struct pty *pt = MVTOPT(vp);
        struct tty *tp = &pt->pt_tty;
        short rev = 0;

        mutex_lock(&tp->t_lock);
        if (pt->pt_count != 0)
                rev |= POLLIN | POLLRDNORM;
        else if (pt->pt_sopened && pt->pt_sopen == 0)
                rev |= POLLHUP;
        if (tp->t_incount + tp->t_linelen + 1 < TTY_INQ)
                rev |= POLLOUT;
        mutex_unlock(&tp->t_lock);
        *reventsp = rev;
        *phpp = &pt->pt_mph;
        return 0;
}

static int
ptm_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct pty *pt = MVTOPT(vp);

        mutex_lock(&pty_lock);
        pt->pt_mactive = 0;
        if (!pt->pt_sactive) {
                pty_free(pt);
                mutex_unlock(&pty_lock);
                return VN_INACTIVE_NOCACHE;
        }
        mutex_unlock(&pty_lock);
        return VN_INACTIVE_NOCACHE;
}

/* --- The slave. --- */

/* Only once unlocked, and while the master is open. */
static int
pts_open(struct vnode **vpp, int flags, struct cred *cr __unused)
{
        struct pty *pt = SVTOPT(*vpp);
        int error = 0;

        mutex_lock(&pty_lock);
        if (pt->pt_locked || pt->pt_mopen == 0) {
                error = EIO;
        } else {
                mutex_lock(&pt->pt_tty.t_lock);
                pt->pt_sopen++;
                pt->pt_sopened = 1;
                mutex_unlock(&pt->pt_tty.t_lock);
        }
        mutex_unlock(&pty_lock);
        if (error == 0)
                tty_open(&pt->pt_tty, flags);
        return error;
}

static int
pts_close(struct vnode *vp, int flags __unused, struct cred *cr __unused)
{
        struct pty *pt = SVTOPT(vp);

        mutex_lock(&pty_lock);
        mutex_lock(&pt->pt_tty.t_lock);
        pt->pt_sopen--;
        sv_broadcast(&pt->pt_msv);
        mutex_unlock(&pt->pt_tty.t_lock);
        mutex_unlock(&pty_lock);
        pollwakeup(&pt->pt_mph);
        return 0;
}

static int
pts_read(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        return tty_read(&SVTOPT(vp)->pt_tty, uio, ioflag);
}

static int
pts_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        return tty_write(&SVTOPT(vp)->pt_tty, uio, ioflag);
}

static int
pts_ioctl(struct vnode *vp, unsigned long cmd, uint64_t arg, int flag,
    struct cred *cr)
{
        return tty_ioctl(&SVTOPT(vp)->pt_tty, cmd, arg, flag, cr);
}

static int
pts_poll(struct vnode *vp, short events, short *reventsp,
    struct pollhead **phpp, struct cred *cr __unused)
{
        return tty_poll(&SVTOPT(vp)->pt_tty, events, reventsp, phpp);
}

static int
pts_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct pty *pt = SVTOPT(vp);

        mutex_lock(&pty_lock);
        pt->pt_sactive = 0;
        if (!pt->pt_mactive) {
                pty_free(pt);
                mutex_unlock(&pty_lock);
                return VN_INACTIVE_NOCACHE;
        }
        mutex_unlock(&pty_lock);
        vn_inactive_done(vp);
        return VN_INACTIVE_CACHE;
}

static const struct vnodeops ptm_vnodeops = {
        ptm_open, ptm_close, ptm_read, ptm_write, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, ptm_inactive,
        spec_putpage, spec_reclaim, spec_getpage, ptm_poll, ptm_ioctl
};

static const struct vnodeops pts_vnodeops = {
        pts_open, pts_close, pts_read, pts_write, spec_getattr,
        spec_setattr, spec_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, pts_inactive,
        spec_putpage, spec_reclaim, spec_getpage, pts_poll, pts_ioctl
};
