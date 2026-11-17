/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * System calls on file descriptors: read, write, pread, pwrite, lseek,
 * close, dup, fcntl, ioctl, getdents, fstat, and ftruncate.
 *
 * Reads and writes through one open file are serialised by its
 * f_offlock, so its offset advances atomically. A transfer that moves
 * some bytes before an error reports the bytes, as System V does.
 *
 * The user buffer is faulted in before the file system is called: the
 * copy inside then cannot fault into a file's VOP_GETPAGE while that
 * file system's locks are held (a write from a mapping of the file
 * being written, say). Mapped pages stay until unmapped, so nothing
 * undoes this in between.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/flock.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/spinlock.h>
#include <sys/stat.h>
#include <sys/synch.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/as.h>
#include <vm/seg.h>

/* Fault in a user buffer; errors are left for the copy to report. */
static void
uio_prefault(void *ubuf, size_t len, enum uio_rw rw)
{
        struct as *as = curproc->p_as;
        vaddr_t va = ptrunc((vaddr_t)(uintptr_t)ubuf);
        vaddr_t end = (vaddr_t)(uintptr_t)ubuf + len;

        if (as == NULL || len == 0 || end < va || end > VM_USER_MAX)
                return;
        for (; va < end; va += PAGESIZE) {
                if (as_fault(as, va, rw == UIO_READ ? S_WRITE : S_READ) !=
                    FC_OK)
                        return;
        }
}

/* A write met RLIMIT_FSIZE: SIGXFSZ to the writing LWP. */
static void
fsize_exceeded(void)
{
        struct proc *p = curproc;
        siginfo_t si;
        int s;

        memset(&si, 0, sizeof(si));
        si.si_signo = SIGXFSZ;
        si.si_code = SI_USER;
        si.si_pid = p->p_pid;
        s = spin_lock(&p->p_siglock);
        sigtolwp_locked(p, curlwp, SIGXFSZ, &si);
        spin_unlock(&p->p_siglock, s);
}

/*
 * Read or write through fd, into or from the user's iovcnt areas iov
 * (which the kernel holds), at the file's offset or, if positioned, at
 * off. rv[0] gets the bytes moved; a partial transfer is a success.
 */
static int
rdwrv(int fd, struct iovec *iov, int iovcnt, off_t off, int positioned,
    enum uio_rw rw, uint64_t *rv)
{
        struct proc *p = curproc;
        struct file *fp = fd_get(p, fd);
        struct vnode *vp;
        struct uio uio;
        uint64_t len = 0;
        int ioflag = 0, error;

        if (fp == NULL)
                return EBADF;
        if ((fp->f_flag & (rw == UIO_READ ? FREAD : FWRITE)) == 0) {
                frele(fp);
                return EBADF;
        }
        for (int i = 0; i < iovcnt; i++) {
                if ((int64_t)iov[i].iov_len < 0 ||
                    len + iov[i].iov_len > (uint64_t)INT64_MAX) {
                        frele(fp);
                        return EINVAL;
                }
                len += iov[i].iov_len;
        }
        vp = fp->f_vnode;
        if (positioned && (vp->v_type == VCHR || vp->v_type == VFIFO ||
            off < 0)) {
                frele(fp);
                return vp->v_type != VREG && vp->v_type != VDIR ? ESPIPE :
                    EINVAL;
        }
        if (rw == UIO_WRITE && (fp->f_flag & FAPPEND) != 0 && !positioned)
                ioflag |= IO_APPEND;
        if ((fp->f_flag & FSYNC) != 0)
                ioflag |= IO_SYNC;
        if ((fp->f_flag & FNONBLOCK) != 0)
                ioflag |= IO_NONBLOCK;
        else if ((fp->f_flag & FNDELAY) != 0)
                ioflag |= IO_NDELAY;
        uio.uio_iov = iov;
        uio.uio_iovcnt = iovcnt;
        uio.uio_resid = (ssize_t)len;
        uio.uio_segflg = UIO_USERSPACE;
        uio.uio_limit = UIO_NOLIMIT;
        if (rw == UIO_WRITE && vp->v_type == VREG) {
                rlim_t fsize = rlimit_cur(p, RLIMIT_FSIZE);

                if (fsize < (rlim_t)UIO_NOLIMIT)
                        uio.uio_limit = (off_t)fsize;
        }
        uio.uio_rw = rw;
        for (int i = 0; i < iovcnt; i++)
                uio_prefault(iov[i].iov_base, iov[i].iov_len, rw);
        if (!positioned)
                mutex_lock(&fp->f_offlock);
        uio.uio_offset = positioned ? off : fp->f_offset;
        if (rw == UIO_READ)
                error = VOP_READ(vp, &uio, ioflag, fp->f_cred);
        else
                error = VOP_WRITE(vp, &uio, ioflag, fp->f_cred);
        if (rw == UIO_WRITE && error == EFBIG && uio.uio_limit != UIO_NOLIMIT &&
            uio.uio_offset >= uio.uio_limit)
                fsize_exceeded();
        if (!positioned) {
                fp->f_offset = uio.uio_offset;
                mutex_unlock(&fp->f_offlock);
        }
        rv[0] = len - (uint64_t)uio.uio_resid;
        if (rv[0] != 0)
                error = 0;
        frele(fp);
        return error;
}

static int
rdwr(int fd, void *ubuf, uint64_t len, off_t off, int positioned,
    enum uio_rw rw, uint64_t *rv)
{
        struct iovec iov;

        iov.iov_base = ubuf;
        iov.iov_len = (size_t)len;
        return rdwrv(fd, &iov, 1, off, positioned, rw, rv);
}

#define IOV_MAX         1024    /* iovecs to readv() and writev() */

/* readv() and writev(): fd, struct iovec *, count. */
static int
rdwr_vec(const uint64_t *args, enum uio_rw rw, uint64_t *rv)
{
        int iovcnt = (int)args[2], error;
        struct iovec *iov;
        size_t size;

        if (iovcnt <= 0 || iovcnt > IOV_MAX)
                return EINVAL;
        size = (size_t)iovcnt * sizeof(*iov);
        iov = kmem_alloc(size, KM_SLEEP);
        if ((error = copyin((const void *)(uintptr_t)args[1], iov,
            size)) == 0)
                error = rdwrv((int)args[0], iov, iovcnt, 0, 0, rw, rv);
        kmem_free(iov, size);
        return error;
}

int
sys_readv(const uint64_t *args, uint64_t *rv)
{
        return rdwr_vec(args, UIO_READ, rv);
}

int
sys_writev(const uint64_t *args, uint64_t *rv)
{
        return rdwr_vec(args, UIO_WRITE, rv);
}

/* fsync(fd): write the file's data and attributes out. */
int
sys_fsync(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        int error;

        if (fp == NULL)
                return EBADF;
        error = VOP_FSYNC(fp->f_vnode, fp->f_cred);
        frele(fp);
        return error;
}

/* fchmod(fd, mode) */
int
sys_fchmod(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        int error;

        if (fp == NULL)
                return EBADF;
        error = vn_chmod(fp->f_vnode, (mode_t)args[1] & 07777);
        frele(fp);
        return error;
}

/* fchown(fd, uid, gid) */
int
sys_fchown(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        int error;

        if (fp == NULL)
                return EBADF;
        error = vn_chown(fp->f_vnode, (uid_t)args[1], (gid_t)args[2]);
        frele(fp);
        return error;
}

int
sys_read(const uint64_t *args, uint64_t *rv)
{
        return rdwr((int)args[0], (void *)(uintptr_t)args[1], args[2], 0, 0,
            UIO_READ, rv);
}

int
sys_write(const uint64_t *args, uint64_t *rv)
{
        return rdwr((int)args[0], (void *)(uintptr_t)args[1], args[2], 0, 0,
            UIO_WRITE, rv);
}

int
sys_pread(const uint64_t *args, uint64_t *rv)
{
        return rdwr((int)args[0], (void *)(uintptr_t)args[1], args[2],
            (off_t)args[3], 1, UIO_READ, rv);
}

int
sys_pwrite(const uint64_t *args, uint64_t *rv)
{
        return rdwr((int)args[0], (void *)(uintptr_t)args[1], args[2],
            (off_t)args[3], 1, UIO_WRITE, rv);
}

int
sys_lseek(const uint64_t *args, uint64_t *rv)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        off_t off = (off_t)args[1], base;
        struct vattr va;
        int error = 0;

        if (fp == NULL)
                return EBADF;
        if (fp->f_vnode->v_type == VFIFO) {
                frele(fp);
                return ESPIPE;
        }
        mutex_lock(&fp->f_offlock);
        switch ((int)args[2]) {
        case SEEK_SET:
                base = 0;
                break;
        case SEEK_CUR:
                base = fp->f_offset;
                break;
        case SEEK_END:
                va.va_mask = AT_SIZE;
                error = VOP_GETATTR(fp->f_vnode, &va, fp->f_cred);
                base = va.va_size;
                break;
        default:
                error = EINVAL;
                base = 0;
                break;
        }
        if (error == 0 && (base + off < 0 || (off > 0 && base + off < base)))
                error = EINVAL;
        if (error == 0) {
                fp->f_offset = base + off;
                rv[0] = (uint64_t)fp->f_offset;
        }
        mutex_unlock(&fp->f_offlock);
        frele(fp);
        return error;
}

int
sys_close(const uint64_t *args, uint64_t *rv __unused)
{
        return fd_close(curproc, (int)args[0]);
}

int
sys_dup(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        struct file *fp = fd_get(p, (int)args[0]);
        int fd, error;

        if (fp == NULL)
                return EBADF;
        if ((error = fd_alloc(p, fp, 0, &fd)) != 0) {
                frele(fp);
                return error;
        }
        rv[0] = (uint64_t)fd;
        return 0;
}

/* The open flags a file was opened with, as fcntl reports them. */
static int
file_oflags(int flag)
{
        return ((flag & (FREAD | FWRITE)) - 1) | (flag & ~(FREAD | FWRITE));
}

/*
 * F_GETLK, F_SETLK and F_SETLKW (vfs_reclock.c), with the struct flock
 * at uaddr. A lock set while another LWP closed fd would outlive the
 * close, so after setting one the descriptor is looked at again: if it
 * no longer names fp, the locks go, as the close would have taken them.
 */
static int
fcntl_lock(struct proc *p, int fd, struct file *fp, int cmd, uint64_t uaddr)
{
        struct flock fl;
        struct file *now;
        int error;

        if ((error = copyin((const void *)(uintptr_t)uaddr, &fl,
            sizeof(fl))) != 0)
                return error;
        if ((error = reclock_fcntl(fp, cmd, &fl)) != 0)
                return error;
        if (cmd == F_GETLK)
                return copyout(&fl, (void *)(uintptr_t)uaddr, sizeof(fl));
        if ((now = fd_get(p, fd)) != NULL)
                frele(now);
        if (now != fp)
                reclock_release(fp->f_vnode, p);
        return 0;
}

/* flock(fd, op): BSD's whole-file lock, owned by the open file. */
int
sys_flock(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        int error;

        if (fp == NULL)
                return EBADF;
        error = reclock_flock(fp, (int)args[1]);
        frele(fp);
        return error;
}

int
sys_fcntl(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        int fd = (int)args[0], cmd = (int)args[1], arg = (int)args[2];
        struct file *fp = fd_get(p, fd);
        int nfd, error = 0;

        if (fp == NULL)
                return EBADF;
        switch (cmd) {
        case F_DUPFD:
                if ((error = fd_alloc(p, fp, arg, &nfd)) == 0) {
                        rv[0] = (uint64_t)nfd;
                        return 0;
                }
                break;
        case F_DUP2FD:
                if (arg == fd) {
                        rv[0] = (uint64_t)fd;
                        break;
                }
                if ((error = fd_install(p, fp, arg)) == 0) {
                        rv[0] = (uint64_t)arg;
                        return 0;
                }
                break;
        case F_GETFD:
                rv[0] = (uint64_t)fd_cloexec(p, fd, -1);
                break;
        case F_SETFD:
                (void)fd_cloexec(p, fd, (arg & FD_CLOEXEC) != 0);
                break;
        case F_GETFL:
                rv[0] = (uint64_t)file_oflags(fp->f_flag);
                break;
        case F_SETFL:
                fp->f_flag = (fp->f_flag & ~FMASK_SETFL) |
                    (arg & FMASK_SETFL);
                break;
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
                error = fcntl_lock(p, fd, fp, cmd, args[2]);
                break;
        default:
                error = EINVAL;
                break;
        }
        frele(fp);
        return error;
}

/*
 * ioctl(fd, cmd, arg): FIONBIO, FIOCLEX, and FIONCLEX here, for any
 * file; everything else goes to the vnode (a terminal), with arg as
 * given and the caller's credentials. Files have no ioctls: ENOTTY.
 */
int
sys_ioctl(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        unsigned long cmd = (unsigned long)args[1];
        int32_t on;
        int error;

        if (fp == NULL)
                return EBADF;
        if (cmd == FIOCLEX || cmd == FIONCLEX) {
                error = 0;
                (void)fd_cloexec(curproc, (int)args[0], cmd == FIOCLEX);
        } else if (cmd == FIONBIO) {
                if ((error = copyin((const void *)(uintptr_t)args[2], &on,
                    sizeof(on))) == 0) {
                        if (on != 0)
                                fp->f_flag |= FNONBLOCK;
                        else
                                fp->f_flag &= ~FNONBLOCK;
                }
        } else {
                error = VOP_IOCTL(fp->f_vnode, cmd, args[2], fp->f_flag,
                    CRED());
        }
        frele(fp);
        return error;
}

/* getdents(fd, buf, nbytes): the native struct dirent records. */
int
sys_getdents(const uint64_t *args, uint64_t *rv)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        struct iovec iov;
        struct uio uio;
        int eof, error;

        if (fp == NULL)
                return EBADF;
        if (fp->f_vnode->v_type != VDIR) {
                frele(fp);
                return ENOTDIR;
        }
        iov.iov_base = (void *)(uintptr_t)args[1];
        iov.iov_len = (size_t)args[2];
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_resid = (ssize_t)args[2];
        uio.uio_segflg = UIO_USERSPACE;
        uio.uio_limit = UIO_NOLIMIT;
        uio.uio_rw = UIO_READ;
        mutex_lock(&fp->f_offlock);
        uio.uio_offset = fp->f_offset;
        error = VOP_READDIR(fp->f_vnode, &uio, &eof, fp->f_cred);
        fp->f_offset = uio.uio_offset;
        mutex_unlock(&fp->f_offlock);
        rv[0] = args[2] - (uint64_t)uio.uio_resid;
        frele(fp);
        return error;
}

/* Fill a native struct stat from attributes. */
void
vattr_to_stat(const struct vattr *va, struct stat *st)
{
        static const mode_t typebits[] = {
                [VREG] = S_IFREG, [VDIR] = S_IFDIR, [VBLK] = S_IFBLK,
                [VCHR] = S_IFCHR, [VLNK] = S_IFLNK, [VFIFO] = S_IFIFO,
                [VSOCK] = S_IFSOCK
        };

        memset(st, 0, sizeof(*st));
        st->st_dev = va->va_fsid;
        st->st_ino = va->va_nodeid;
        st->st_mode = (va->va_type <= VSOCK ? typebits[va->va_type] : 0) |
            (va->va_mode & 07777);
        st->st_nlink = va->va_nlink;
        st->st_uid = va->va_uid;
        st->st_gid = va->va_gid;
        st->st_rdev = va->va_rdev;
        st->st_size = va->va_size;
        st->st_atime_sec = va->va_atime.tv_sec;
        st->st_atime_nsec = va->va_atime.tv_nsec;
        st->st_mtime_sec = va->va_mtime.tv_sec;
        st->st_mtime_nsec = va->va_mtime.tv_nsec;
        st->st_ctime_sec = va->va_ctime.tv_sec;
        st->st_ctime_nsec = va->va_ctime.tv_nsec;
        st->st_blksize = va->va_blksize;
        st->st_blocks = (int64_t)va->va_nblocks;
}

int
sys_fstat(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        struct vattr va;
        struct stat st;
        int error;

        if (fp == NULL)
                return EBADF;
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_ALL;
        error = VOP_GETATTR(fp->f_vnode, &va, fp->f_cred);
        frele(fp);
        if (error != 0)
                return error;
        vattr_to_stat(&va, &st);
        return copyout(&st, (void *)(uintptr_t)args[1], sizeof(st));
}

int
sys_ftruncate(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        struct vattr va;
        int error;

        if (fp == NULL)
                return EBADF;
        if ((fp->f_flag & FWRITE) == 0 || (off_t)args[1] < 0) {
                frele(fp);
                return EINVAL;
        }
        if (args[1] > rlimit_cur(curproc, RLIMIT_FSIZE)) {
                frele(fp);
                fsize_exceeded();
                return EFBIG;
        }
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_SIZE;
        va.va_size = (off_t)args[1];
        error = VOP_SETATTR(fp->f_vnode, &va, fp->f_cred);
        frele(fp);
        return error;
}
