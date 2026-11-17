/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-10
 * SPDX-License-Identifier: MIT
 */

/*
 * Pipes, pipe() (42), and named FIFOs: the FIFO vnodes behind both.
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
 * O_NDELAY return 0 instead, as in System V.
 *
 * Named FIFOs use the same node. A file system keeps the FIFO's name
 * and attributes in a vnode of type VFIFO; vn_open() trades that vnode
 * for a fifonode with fifo_vp(), one per file system vnode while any
 * process has it open, found on fifo_list. The fifonode holds the file
 * system's vnode and passes attributes and access checks to it. Opening
 * follows POSIX: a reader waits for a writer and a writer for a reader,
 * unless O_NONBLOCK or O_NDELAY is given, when a reader goes ahead and
 * a writer with no reader fails with ENXIO. Opening for both never
 * waits. The data is discarded once neither end is open.
 *
 * Data moves with fn_lock held, so a fault on the user's buffer is
 * taken under it: fn_lock ranks above the address space's locks.
 * fifo_lock, a spin lock, guards only fifo_list.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/poll.h>
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
        struct pollhead fn_ph;          /* pollers */
        int             fn_readers;     /* open read ends, fn_lock */
        int             fn_writers;     /* open write ends, fn_lock */
        size_t          fn_head;        /* first byte of data, fn_lock */
        size_t          fn_count;       /* bytes held, fn_lock */
        char            *fn_buf;
        uint64_t        fn_ropens;      /* opens for reading ever, fn_lock */
        uint64_t        fn_wopens;      /* opens for writing ever, fn_lock */
        struct vnode    *fn_realvp;     /* a named FIFO's own vnode, held */
        struct fifonode *fn_next;       /* fifo_list, under fifo_lock */
        uid_t           fn_uid;
        gid_t           fn_gid;
        struct timespec fn_atime;
        struct timespec fn_mtime;
        struct timespec fn_ctime;
};

#define VTOFN(vp)       ((struct fifonode *)(vp)->v_data)

static const struct vnodeops fifo_vnodeops;

static void fifo_free(struct fifonode *);

/* The named FIFOs open now. */
static struct spinlock fifo_lock = SPINLOCK_INITIALIZER("fifolist");
static struct fifonode *fifo_list;

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

/* Take away the ends an open added; fn_lock is held. */
static void
fifo_drop_ends(struct fifonode *fn, int flags)
{
        if ((flags & FREAD) != 0)
                fn->fn_readers--;
        if ((flags & FWRITE) != 0)
                fn->fn_writers--;
        if (fn->fn_realvp != NULL && fn->fn_readers == 0 &&
            fn->fn_writers == 0) {
                fn->fn_head = 0;
                fn->fn_count = 0;
        }
}

/*
 * Open a named FIFO (pipes are born open): count the new ends, then
 * wait for the other side as POSIX asks.
 */
static int
fifo_open(struct vnode **vpp, int flags, struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(*vpp);
        int nowait = (flags & (FNONBLOCK | FNDELAY)) != 0;
        uint64_t gen;
        int error = 0;

        if (fn->fn_realvp == NULL)
                return 0;
        mutex_lock(&fn->fn_lock);
        if ((flags & (FREAD | FWRITE)) == FWRITE && nowait &&
            fn->fn_readers == 0) {
                mutex_unlock(&fn->fn_lock);
                return ENXIO;
        }
        if ((flags & FREAD) != 0) {
                fn->fn_readers++;
                fn->fn_ropens++;
        }
        if ((flags & FWRITE) != 0) {
                fn->fn_writers++;
                fn->fn_wopens++;
        }
        mutex_unlock(&fn->fn_lock);
        sv_broadcast(&fn->fn_rsv);
        sv_broadcast(&fn->fn_wsv);
        pollwakeup(&fn->fn_ph);
        if (nowait || (flags & (FREAD | FWRITE)) == (FREAD | FWRITE))
                return 0;

        /*
         * Wait until the other side is open, or has been opened since
         * (it may have closed again already). Readers wait on fn_rsv,
         * writers on fn_wsv, alongside reads and writes.
         */
        mutex_lock(&fn->fn_lock);
        if ((flags & FREAD) != 0) {
                gen = fn->fn_wopens;
                while (fn->fn_writers == 0 && fn->fn_wopens == gen &&
                    error == 0)
                        error = fifo_sleep(fn, &fn->fn_rsv);
        } else {
                gen = fn->fn_ropens;
                while (fn->fn_readers == 0 && fn->fn_ropens == gen &&
                    error == 0)
                        error = fifo_sleep(fn, &fn->fn_wsv);
        }
        if (error != 0)
                fifo_drop_ends(fn, flags);
        mutex_unlock(&fn->fn_lock);
        if (error != 0) {
                sv_broadcast(&fn->fn_rsv);
                sv_broadcast(&fn->fn_wsv);
                pollwakeup(&fn->fn_ph);
        }
        return error;
}

/* The last close of a file: one end fewer. */
static int
fifo_close(struct vnode *vp, int flags, struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp);

        mutex_lock(&fn->fn_lock);
        fifo_drop_ends(fn, flags);
        mutex_unlock(&fn->fn_lock);
        sv_broadcast(&fn->fn_rsv);
        sv_broadcast(&fn->fn_wsv);
        pollwakeup(&fn->fn_ph);
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
        if (moved != 0) {
                sv_broadcast(&fn->fn_wsv);
                pollwakeup(&fn->fn_ph);
        }
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
                        pollwakeup(&fn->fn_ph);
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
        if (moved != 0) {
                sv_broadcast(&fn->fn_rsv);
                pollwakeup(&fn->fn_ph);
        }
        return error;
}

static int
fifo_getattr(struct vnode *vp, struct vattr *va, struct cred *cr)
{
        struct fifonode *fn = VTOFN(vp);
        int error;

        if (fn->fn_realvp != NULL) {
                /* A named FIFO's attributes are its file's. */
                if ((error = VOP_GETATTR(fn->fn_realvp, va, cr)) != 0)
                        return error;
                mutex_lock(&fn->fn_lock);
                va->va_size = (off_t)fn->fn_count;
                mutex_unlock(&fn->fn_lock);
                return 0;
        }
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
fifo_setattr(struct vnode *vp, struct vattr *va, struct cred *cr)
{
        struct fifonode *fn = VTOFN(vp);

        if (fn->fn_realvp == NULL || (va->va_mask & AT_SIZE) != 0)
                return EINVAL;
        return VOP_SETATTR(fn->fn_realvp, va, cr);
}

static int
fifo_access(struct vnode *vp, int mode, struct cred *cr)
{
        struct fifonode *fn = VTOFN(vp);

        if (fn->fn_realvp == NULL)
                return 0;
        return VOP_ACCESS(fn->fn_realvp, mode, cr);
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
fifo_fsync(struct vnode *vp, struct cred *cr __unused)
{
        return VTOFN(vp)->fn_realvp != NULL ? 0 : EINVAL;
}

/*
 * The last reference: every end is closed, so the node goes. A named
 * FIFO's leaves fifo_list first; an open racing with this finds the
 * vnode inactive and makes a node of its own.
 */
static int
fifo_inactive(struct vnode *vp, struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp), **link;
        int s;

        if (fn->fn_realvp != NULL) {
                s = spin_lock(&fifo_lock);
                for (link = &fifo_list; *link != fn; link = &(*link)->fn_next)
                        ;
                *link = fn->fn_next;
                spin_unlock(&fifo_lock, s);
                VN_RELE(fn->fn_realvp);
        }
        fifo_free(fn);
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

/*
 * Readable with data held; hung up once no writer is left (for a named
 * FIFO, once some writer has come and gone); writable with room for an
 * atomic write; an error to write with no reader. poll() keeps only the
 * events of the ends a descriptor has open.
 */
static int
fifo_poll(struct vnode *vp, short events __unused, short *reventsp,
    struct pollhead **phpp, struct cred *cr __unused)
{
        struct fifonode *fn = VTOFN(vp);
        short rev = 0;

        mutex_lock(&fn->fn_lock);
        if (fn->fn_count != 0)
                rev |= POLLIN | POLLRDNORM;
        if (fn->fn_writers == 0 &&
            (fn->fn_realvp == NULL || fn->fn_wopens != 0))
                rev |= POLLHUP;
        if (PIPE_SIZE - fn->fn_count >= PIPE_BUF)
                rev |= POLLOUT;
        if (fn->fn_readers == 0)
                rev |= POLLERR;
        mutex_unlock(&fn->fn_lock);
        *reventsp = rev;
        *phpp = &fn->fn_ph;
        return 0;
}

static const struct vnodeops fifo_vnodeops = {
        fifo_open, fifo_close, fifo_read, fifo_write, fifo_getattr,
        fifo_setattr, fifo_access, fifo_lookup, fifo_create, fifo_name1,
        fifo_link, fifo_rename, fifo_mkdir, fifo_name1, fifo_readdir,
        fifo_symlink, fifo_readlink, fifo_fsync, fifo_inactive,
        fifo_putpage, fifo_reclaim, fifo_getpage, fifo_poll, NULL
};

/* A new fifonode with no ends open, its vnode unreferenced. */
static struct fifonode *
fifo_alloc(void)
{
        struct fifonode *fn = kmem_zalloc(sizeof(*fn), KM_SLEEP);

        fn->fn_buf = kmem_alloc(PIPE_SIZE, KM_SLEEP);
        mutex_init(&fn->fn_lock, "pipe");
        sv_init(&fn->fn_rsv, "piperd");
        sv_init(&fn->fn_wsv, "pipewr");
        pollhead_init(&fn->fn_ph);
        fn->fn_uid = CRED()->cr_uid;
        fn->fn_gid = CRED()->cr_gid;
        gethrestime(&fn->fn_ctime);
        fn->fn_atime = fn->fn_mtime = fn->fn_ctime;
        vn_init(&fn->fn_vnode, NULL, VFIFO, &fifo_vnodeops, fn);
        return fn;
}

static void
fifo_free(struct fifonode *fn)
{
        vn_destroy(&fn->fn_vnode);
        kmem_free(fn->fn_buf, PIPE_SIZE);
        kmem_free(fn, sizeof(*fn));
}

/* The open node for realvp on fifo_list, held, or NULL; fifo_lock held. */
static struct vnode *
fifo_find(struct vnode *realvp)
{
        struct fifonode *fn;

        for (fn = fifo_list; fn != NULL; fn = fn->fn_next)
                if (fn->fn_realvp == realvp &&
                    vn_get(&fn->fn_vnode, fn->fn_vnode.v_number))
                        return &fn->fn_vnode;
        return NULL;
}

/*
 * The FIFO node standing for realvp, a file system's VFIFO vnode, held,
 * in *vpp; it takes a reference of its own to realvp. vn_open() calls
 * this before VOP_OPEN.
 */
int
fifo_vp(struct vnode *realvp, struct vnode **vpp)
{
        struct fifonode *fn;
        struct vnode *vp;
        int s;

        s = spin_lock(&fifo_lock);
        vp = fifo_find(realvp);
        spin_unlock(&fifo_lock, s);
        if (vp != NULL) {
                *vpp = vp;
                return 0;
        }
        fn = fifo_alloc();
        s = spin_lock(&fifo_lock);
        if ((vp = fifo_find(realvp)) == NULL) {
                fn->fn_realvp = realvp;
                fn->fn_next = fifo_list;
                fifo_list = fn;
                vp = &fn->fn_vnode;
                VN_HOLD(realvp);
                VN_HOLD(vp);
                fn = NULL;
        }
        spin_unlock(&fifo_lock, s);
        if (fn != NULL)
                fifo_free(fn);
        *vpp = vp;
        return 0;
}

/*
 * pipe(): two descriptors, returned in %rax and %rdx as in System V,
 * the first open for reading the pipe and the second for writing it.
 */
static int
pipe_create(int flags, uint64_t *rv)
{
        struct proc *p = curproc;
        struct fifonode *fn = fifo_alloc();
        struct vnode *vp = &fn->fn_vnode;
        struct file *files[2];
        int fds[2], error, status = flags & O_NONBLOCK;

        fn->fn_readers = 1;
        fn->fn_writers = 1;
        VN_HOLD(vp);                    /* the read end's */
        VN_HOLD(vp);                    /* the write end's */
        files[0] = falloc(vp, FREAD | status);
        files[1] = falloc(vp, FWRITE | status);
        error = fd_alloc_pair(p, files, (flags & O_CLOEXEC) != 0 ?
            FD_CLOEXEC : 0, fds);
        if (error != 0) {
                frele(files[0]);
                frele(files[1]);
                return error;
        }
        rv[0] = (uint64_t)fds[0];
        rv[1] = (uint64_t)fds[1];
        return 0;
}

int
sys_pipe(const uint64_t *args __unused, uint64_t *rv)
{
        return pipe_create(0, rv);
}

int
sys_pipe2(const uint64_t *args, uint64_t *rv)
{
        int flags = (int)args[0];

        if ((flags & ~(O_NONBLOCK | O_CLOEXEC)) != 0)
                return EINVAL;
        return pipe_create(flags, rv);
}
