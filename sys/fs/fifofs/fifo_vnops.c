/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-10
 * SPDX-License-Identifier: MIT
 */

/*
 * Pipes: pipe() (42) and the FIFO vnodes behind them.
 *
 * A pipe is one private vnode of type VFIFO, outside any file system,
 * opened twice: the first descriptor for reading, the second for
 * writing (POSIX pipes, not System V's two-way STREAMS ones). Its data
 * is a ring buffer of PIPE_SIZE bytes under fn_lock; readers wait on
 * fn_rsv for data and writers on fn_wsv for room. The ends are counted
 * as files close: reading with no writers left is end of file, and
 * writing with no readers left fails with EPIPE after SIGPIPE to the
 * writing LWP. A write of PIPE_BUF bytes or fewer goes in whole or
 * waits; a longer one may be split among other writers' data.
 *
 * O_NONBLOCK makes a read or write that would wait fail with EAGAIN,
 * O_NDELAY return 0 instead, as in System V. Named FIFOs are to use the
 * same node.
 *
 * Data moves with fn_lock held, so a fault on the user's buffer is
 * taken under it: fn_lock ranks above the address space's locks.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define PIPE_SIZE       16384   /* bytes a pipe holds */
#define PIPE_BUF        4096    /* writes up to this size are atomic */

struct fifonode {
        struct vnode    fn_vnode;
        struct mutex    fn_lock;
        struct sv       fn_rsv;         /* readers wait for data */
        struct sv       fn_wsv;         /* writers wait for room */
        int             fn_readers;     /* open read ends, fn_lock */
        int             fn_writers;     /* open write ends, fn_lock */
        size_t          fn_head;        /* first byte of data, fn_lock */
        size_t          fn_count;       /* bytes held, fn_lock */
        char            *fn_buf;
        uid_t           fn_uid;
        gid_t           fn_gid;
        struct timespec fn_atime;
        struct timespec fn_mtime;
        struct timespec fn_ctime;
};

#define VTOFN(vp)       ((struct fifonode *)(vp)->v_data)

/*
 * Wait on sv, dropping fn_lock; it is held again on return. Returns 0
 * after a wakeup (which may be spurious), EINTR when a handler is due,
 * or ERESTART when the LWP must stop or exit and reissue the call.
 */
static int
fifo_sleep(struct fifonode *fn, struct sv *sv)
{
        int r;

        if (sv_wait_mutex_sig(sv, &fn->fn_lock) != 0 &&
            (r = sig_sleep_check()) != 0) {
                mutex_lock(&fn->fn_lock);
                return r == 1 ? EINTR : ERESTART;
        }
        mutex_lock(&fn->fn_lock);
        return 0;
}

/* Writing to a pipe with no reader: SIGPIPE to the writing LWP. */
static int
fifo_epipe(void)
{
        struct proc *p = curproc;
        siginfo_t si;
        int s;

        memset(&si, 0, sizeof(si));
        si.si_signo = SIGPIPE;
        si.si_code = SI_USER;
        si.si_pid = p->p_pid;
        s = spin_lock(&p->p_siglock);
        sigtolwp_locked(p, curlwp, SIGPIPE, &si);
        spin_unlock(&p->p_siglock, s);
        return EPIPE;
}

static int
fifo_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

/* The last close of a file: one end fewer. */
static int
fifo_close(struct vnode *vp, int flags, struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp);

        mutex_lock(&fn->fn_lock);
        if ((flags & FREAD) != 0)
                fn->fn_readers--;
        if ((flags & FWRITE) != 0)
                fn->fn_writers--;
        mutex_unlock(&fn->fn_lock);
        sv_broadcast(&fn->fn_rsv);
        sv_broadcast(&fn->fn_wsv);
        return 0;
}

static int
fifo_read(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp);
        size_t moved = 0;
        int error = 0;

        mutex_lock(&fn->fn_lock);
        while (fn->fn_count == 0) {
                if (fn->fn_writers == 0 || (ioflag & IO_NDELAY) != 0)
                        goto out;
                if ((ioflag & IO_NONBLOCK) != 0) {
                        error = EAGAIN;
                        goto out;
                }
                if ((error = fifo_sleep(fn, &fn->fn_rsv)) != 0)
                        goto out;
        }
        while (uio->uio_resid > 0 && fn->fn_count > 0) {
                size_t n = fn->fn_count;

                if (n > PIPE_SIZE - fn->fn_head)
                        n = PIPE_SIZE - fn->fn_head;
                if (n > (size_t)uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if ((error = uiomove(fn->fn_buf + fn->fn_head, n, UIO_READ,
                    uio)) != 0)
                        break;
                fn->fn_head = (fn->fn_head + n) % PIPE_SIZE;
                fn->fn_count -= n;
                moved += n;
        }
        if (fn->fn_count == 0)
                fn->fn_head = 0;
        if (moved != 0)
                gethrestime(&fn->fn_atime);
out:
        mutex_unlock(&fn->fn_lock);
        if (moved != 0)
                sv_broadcast(&fn->fn_wsv);
        return error;
}

static int
fifo_write(struct vnode *vp, struct uio *uio, int ioflag,
    struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp);
        int atomic = uio->uio_resid <= PIPE_BUF;
        size_t moved = 0;
        int error = 0;

        mutex_lock(&fn->fn_lock);
        while (uio->uio_resid > 0) {
                size_t room, tail, n;

                if (fn->fn_readers == 0) {
                        error = fifo_epipe();
                        break;
                }
                room = PIPE_SIZE - fn->fn_count;
                if (room == 0 || (atomic && room < (size_t)uio->uio_resid)) {
                        if (moved != 0 && !atomic &&
                            (ioflag & (IO_NONBLOCK | IO_NDELAY)) != 0)
                                break;
                        if ((ioflag & IO_NDELAY) != 0)
                                break;
                        if ((ioflag & IO_NONBLOCK) != 0) {
                                error = EAGAIN;
                                break;
                        }
                        mutex_unlock(&fn->fn_lock);
                        sv_broadcast(&fn->fn_rsv);
                        mutex_lock(&fn->fn_lock);
                        if ((error = fifo_sleep(fn, &fn->fn_wsv)) != 0)
                                break;
                        continue;
                }
                tail = (fn->fn_head + fn->fn_count) % PIPE_SIZE;
                n = room;
                if (n > PIPE_SIZE - tail)
                        n = PIPE_SIZE - tail;
                if (n > (size_t)uio->uio_resid)
                        n = (size_t)uio->uio_resid;
                if ((error = uiomove(fn->fn_buf + tail, n, UIO_WRITE,
                    uio)) != 0)
                        break;
                fn->fn_count += n;
                moved += n;
        }
        if (moved != 0) {
                gethrestime(&fn->fn_mtime);
                fn->fn_ctime = fn->fn_mtime;
        }
        mutex_unlock(&fn->fn_lock);
        if (moved != 0)
                sv_broadcast(&fn->fn_rsv);
        return error;
}

static int
fifo_getattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp);

        memset(va, 0, sizeof(*va));
        mutex_lock(&fn->fn_lock);
        va->va_type = VFIFO;
        va->va_mode = 0600;
        va->va_uid = fn->fn_uid;
        va->va_gid = fn->fn_gid;
        va->va_nodeid = vp->v_number;
        va->va_nlink = 1;
        va->va_size = (off_t)fn->fn_count;
        va->va_atime = fn->fn_atime;
        va->va_mtime = fn->fn_mtime;
        va->va_ctime = fn->fn_ctime;
        va->va_blksize = PIPE_BUF;
        mutex_unlock(&fn->fn_lock);
        return 0;
}

static int
fifo_setattr(struct vnode *vp __unused, struct vattr *va __unused,
    struct cred *cr __unused)
{
        return EINVAL;
}

static int
fifo_access(struct vnode *vp __unused, int mode __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
fifo_lookup(struct vnode *dvp __unused, const char *nm __unused,
    struct vnode **vpp __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_create(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, int excl __unused, struct vnode **vpp __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_name1(struct vnode *dvp __unused, const char *nm __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_link(struct vnode *tdvp __unused, struct vnode *vp __unused,
    const char *nm __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_rename(struct vnode *sdvp __unused, const char *snm __unused,
    struct vnode *tdvp __unused, const char *tnm __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_mkdir(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, struct vnode **vpp __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_readdir(struct vnode *vp __unused, struct uio *uio __unused,
    int *eofp __unused, struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_symlink(struct vnode *dvp __unused, const char *nm __unused,
    struct vattr *va __unused, const char *target __unused,
    struct cred *cr __unused)
{
        return ENOTDIR;
}

static int
fifo_readlink(struct vnode *vp __unused, struct uio *uio __unused,
    struct cred *cr __unused)
{
        return EINVAL;
}

static int
fifo_fsync(struct vnode *vp __unused, struct cred *cr __unused)
{
        return EINVAL;
}

/* The last reference: both ends are closed, so the pipe goes. */
static int
fifo_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp);

        vn_destroy(vp);
        kmem_free(fn->fn_buf, PIPE_SIZE);
        kmem_free(fn, sizeof(*fn));
        return VN_INACTIVE_NOCACHE;
}

static int
fifo_putpage(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
fifo_reclaim(struct vnode *vp __unused, int flags __unused)
{
        return EBUSY;
}

static int
fifo_getpage(struct vnode *vp __unused, off_t off __unused,
    struct page **ppp __unused, struct cred *cr __unused)
{
        return ENODEV;
}

static const struct vnodeops fifo_vnodeops = {
        fifo_open, fifo_close, fifo_read, fifo_write, fifo_getattr,
        fifo_setattr, fifo_access, fifo_lookup, fifo_create, fifo_name1,
        fifo_link, fifo_rename, fifo_mkdir, fifo_name1, fifo_readdir,
        fifo_symlink, fifo_readlink, fifo_fsync, fifo_inactive,
        fifo_putpage, fifo_reclaim, fifo_getpage
};

/*
 * pipe(): two descriptors, returned in %rax and %rdx as in System V,
 * the first open for reading the pipe and the second for writing it.
 */
int
sys_pipe(const uint64_t *args __unused, uint64_t *rv)
{
        struct proc *p = curproc;
        struct fifonode *fn = kmem_zalloc(sizeof(*fn), KM_SLEEP);
        struct vnode *vp = &fn->fn_vnode;
        struct file *rfp, *wfp;
        int rfd, wfd, error;

        fn->fn_buf = kmem_alloc(PIPE_SIZE, KM_SLEEP);
        mutex_init(&fn->fn_lock, "pipe");
        sv_init(&fn->fn_rsv, "piperd");
        sv_init(&fn->fn_wsv, "pipewr");
        fn->fn_readers = 1;
        fn->fn_writers = 1;
        fn->fn_uid = p->p_cred->cr_uid;
        fn->fn_gid = p->p_cred->cr_gid;
        gethrestime(&fn->fn_ctime);
        fn->fn_atime = fn->fn_mtime = fn->fn_ctime;
        vn_init(vp, NULL, VFIFO, &fifo_vnodeops, fn);
        VN_HOLD(vp);                    /* the read end's */
        VN_HOLD(vp);                    /* the write end's */
        rfp = falloc(vp, FREAD);
        wfp = falloc(vp, FWRITE);
        if ((error = fd_alloc(p, rfp, 0, &rfd)) != 0) {
                frele(rfp);
                frele(wfp);
                return error;
        }
        if ((error = fd_alloc(p, wfp, 0, &wfd)) != 0) {
                (void)fd_close(p, rfd);
                frele(wfp);
                return error;
        }
        rv[0] = (uint64_t)rfd;
        rv[1] = (uint64_t)wfd;
        return 0;
}
