/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * System calls on path names. Each copies its paths in and calls the
 * kernel file interface (vfs_vnops.c). Relative paths start at the
 * process's current directory, or for the *at calls (Belka's own) at
 * the directory open on a descriptor unless it is AT_FDCWD. There are
 * no permission checks beyond the file systems' own, since every
 * process runs with the kernel's credentials until security is in
 * scope.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/pathname.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

/*
 * A user path, copied in, and the directory a relative one starts at:
 * NULL for AT_FDCWD or an absolute path (the current directory or the
 * root), else the directory open on dirfd, held.
 */
struct upath {
        char            *up_path;
        struct vnode    *up_dir;
};

static int
upath_in(int dirfd, uint64_t uaddr, struct upath *up)
{
        char *kp = kmem_alloc(MAXPATHLEN, KM_SLEEP);
        struct file *fp;
        struct vnode *vp;
        int error;

        error = copyinstr((const char *)(uintptr_t)uaddr, kp, MAXPATHLEN,
            NULL);
        if (error != 0) {
                kmem_free(kp, MAXPATHLEN);
                return error;
        }
        up->up_path = kp;
        up->up_dir = NULL;
        if (dirfd == AT_FDCWD || kp[0] == '/')
                return 0;
        if ((fp = fd_get(curproc, dirfd)) == NULL) {
                error = EBADF;
        } else {
                vp = fp->f_vnode;
                VN_HOLD(vp);
                frele(fp);
                if (vp->v_type == VDIR) {
                        up->up_dir = vp;
                        return 0;
                }
                VN_RELE(vp);
                error = ENOTDIR;
        }
        kmem_free(kp, MAXPATHLEN);
        return error;
}

static void
upath_free(struct upath *up)
{
        if (up->up_dir != NULL)
                VN_RELE(up->up_dir);
        kmem_free(up->up_path, MAXPATHLEN);
}

/* Look up a user path relative to dirfd. */
static int
upath_lookup(int dirfd, uint64_t uaddr, int follow, struct vnode **vpp)
{
        struct upath up;
        int error;

        if ((error = upath_in(dirfd, uaddr, &up)) != 0)
                return error;
        error = lookuppn(up.up_path, follow, up.up_dir, NULL, NULL, vpp);
        upath_free(&up);
        return error;
}

static int
do_open(int dirfd, uint64_t uaddr, int oflag, mode_t mode, uint64_t *rv)
{
        struct proc *p = curproc;
        int fflag = ((oflag & O_ACCMODE) + 1) | (oflag & ~O_ACCMODE);
        struct upath up;
        struct vnode *vp;
        struct file *fp;
        int fd, error;

        if ((oflag & O_ACCMODE) == O_ACCMODE)
                return EINVAL;
        if ((error = upath_in(dirfd, uaddr, &up)) != 0)
                return error;
        error = vn_openat(up.up_dir, up.up_path, fflag,
            mode & ~p->p_cmask & 07777, &vp);
        upath_free(&up);
        if (error != 0)
                return error;
        fp = falloc(vp, fflag & (FREAD | FWRITE | FNDELAY | FAPPEND |
            FSYNC | FNONBLOCK));
        if ((error = fd_alloc(p, fp, 0, &fd)) != 0) {
                frele(fp);
                return error;
        }
        rv[0] = (uint64_t)fd;
        return 0;
}

int
sys_open(const uint64_t *args, uint64_t *rv)
{
        return do_open(AT_FDCWD, args[0], (int)args[1], (mode_t)args[2],
            rv);
}

int
sys_creat(const uint64_t *args, uint64_t *rv)
{
        return do_open(AT_FDCWD, args[0], O_WRONLY | O_CREAT | O_TRUNC,
            (mode_t)args[1], rv);
}

/* openat(dirfd, path, oflag, mode) */
int
sys_openat(const uint64_t *args, uint64_t *rv)
{
        return do_open((int)args[0], args[1], (int)args[2], (mode_t)args[3],
            rv);
}

/* unlink and rmdir, as unlinkat with flag 0 or AT_REMOVEDIR. */
static int
do_unlink(int dirfd, uint64_t uaddr, int flag)
{
        struct upath up;
        int error;

        if ((flag & ~AT_REMOVEDIR) != 0)
                return EINVAL;
        if ((error = upath_in(dirfd, uaddr, &up)) != 0)
                return error;
        if ((flag & AT_REMOVEDIR) != 0)
                error = vn_rmdirat(up.up_dir, up.up_path);
        else
                error = vn_removeat(up.up_dir, up.up_path);
        upath_free(&up);
        return error;
}

int
sys_unlink(const uint64_t *args, uint64_t *rv __unused)
{
        return do_unlink(AT_FDCWD, args[0], 0);
}

int
sys_rmdir(const uint64_t *args, uint64_t *rv __unused)
{
        return do_unlink(AT_FDCWD, args[0], AT_REMOVEDIR);
}

/* unlinkat(dirfd, path, flag) */
int
sys_unlinkat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_unlink((int)args[0], args[1], (int)args[2]);
}

/*
 * link and rename, between two paths with their own directories. link
 * does not follow a last symbolic link in the existing path unless
 * linkat is given AT_SYMLINK_FOLLOW.
 */
static int
do_link(int fromfd, uint64_t from, int tofd, uint64_t to, int flag,
    int rename)
{
        struct upath fp, tp;
        int error;

        if ((flag & ~(rename ? 0 : AT_SYMLINK_FOLLOW)) != 0)
                return EINVAL;
        if ((error = upath_in(fromfd, from, &fp)) != 0)
                return error;
        if ((error = upath_in(tofd, to, &tp)) != 0) {
                upath_free(&fp);
                return error;
        }
        if (rename)
                error = vn_renameat(fp.up_dir, fp.up_path, tp.up_dir,
                    tp.up_path);
        else
                error = vn_linkat(fp.up_dir, fp.up_path, tp.up_dir,
                    tp.up_path, (flag & AT_SYMLINK_FOLLOW) != 0 ? FOLLOW :
                    NO_FOLLOW);
        upath_free(&tp);
        upath_free(&fp);
        return error;
}

int
sys_link(const uint64_t *args, uint64_t *rv __unused)
{
        return do_link(AT_FDCWD, args[0], AT_FDCWD, args[1], 0, 0);
}

/* linkat(fromfd, from, tofd, to, flag) */
int
sys_linkat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_link((int)args[0], args[1], (int)args[2], args[3],
            (int)args[4], 0);
}

int
sys_rename(const uint64_t *args, uint64_t *rv __unused)
{
        return do_link(AT_FDCWD, args[0], AT_FDCWD, args[1], 0, 1);
}

/* renameat(fromfd, from, tofd, to) */
int
sys_renameat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_link((int)args[0], args[1], (int)args[2], args[3], 0, 1);
}

/* symlink(target, path) and symlinkat(target, dirfd, path). */
static int
do_symlink(uint64_t utarget, int dirfd, uint64_t uaddr)
{
        struct upath tp, up;
        int error;

        /* The target is only text; no directory goes with it. */
        if ((error = upath_in(AT_FDCWD, utarget, &tp)) != 0)
                return error;
        if ((error = upath_in(dirfd, uaddr, &up)) == 0) {
                error = vn_symlinkat(tp.up_path, up.up_dir, up.up_path);
                upath_free(&up);
        }
        upath_free(&tp);
        return error;
}

int
sys_symlink(const uint64_t *args, uint64_t *rv __unused)
{
        return do_symlink(args[0], AT_FDCWD, args[1]);
}

int
sys_symlinkat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_symlink(args[0], (int)args[1], args[2]);
}

static int
do_mkdir(int dirfd, uint64_t uaddr, mode_t mode)
{
        struct upath up;
        int error;

        if ((error = upath_in(dirfd, uaddr, &up)) != 0)
                return error;
        error = vn_mkdirat(up.up_dir, up.up_path,
            mode & ~curproc->p_cmask & 07777);
        upath_free(&up);
        return error;
}

int
sys_mkdir(const uint64_t *args, uint64_t *rv __unused)
{
        return do_mkdir(AT_FDCWD, args[0], (mode_t)args[1]);
}

/* mkdirat(dirfd, path, mode) */
int
sys_mkdirat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_mkdir((int)args[0], args[1], (mode_t)args[2]);
}

/*
 * mknod(path, mode, dev): mode's type bits say what to make. Only
 * FIFOs (S_IFIFO) can be made so far; dev is for device files.
 */
static int
do_mknod(int dirfd, uint64_t uaddr, mode_t mode)
{
        struct upath up;
        int error;

        if ((mode & S_IFMT) != S_IFIFO)
                return EINVAL;
        if ((error = upath_in(dirfd, uaddr, &up)) != 0)
                return error;
        error = vn_mknodat(up.up_dir, up.up_path, VFIFO,
            mode & ~curproc->p_cmask & 07777);
        upath_free(&up);
        return error;
}

int
sys_mknod(const uint64_t *args, uint64_t *rv __unused)
{
        return do_mknod(AT_FDCWD, args[0], (mode_t)args[1]);
}

/* mknodat(dirfd, path, mode, dev) */
int
sys_mknodat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_mknod((int)args[0], args[1], (mode_t)args[2]);
}

/* readlink(path, buf, size): the bytes of the link, not terminated. */
static int
do_readlink(int dirfd, uint64_t uaddr, uint64_t ubuf, size_t size,
    uint64_t *rv)
{
        struct upath up;
        char *buf;
        size_t len;
        int error;

        if (size == 0)
                return EINVAL;
        if ((error = upath_in(dirfd, uaddr, &up)) != 0)
                return error;
        buf = kmem_alloc(MAXPATHLEN, KM_SLEEP);
        error = vn_readlinkat(up.up_dir, up.up_path, buf, MAXPATHLEN, &len);
        upath_free(&up);
        if (error == 0) {
                if (len > size)
                        len = size;
                error = copyout(buf, (void *)(uintptr_t)ubuf, len);
                rv[0] = len;
        }
        kmem_free(buf, MAXPATHLEN);
        return error;
}

int
sys_readlink(const uint64_t *args, uint64_t *rv)
{
        return do_readlink(AT_FDCWD, args[0], args[1], (size_t)args[2], rv);
}

/* readlinkat(dirfd, path, buf, size) */
int
sys_readlinkat(const uint64_t *args, uint64_t *rv)
{
        return do_readlink((int)args[0], args[1], args[2], (size_t)args[3],
            rv);
}

static int
do_stat(int dirfd, uint64_t uaddr, uint64_t ubuf, int flag)
{
        struct upath up;
        struct vattr va;
        struct stat st;
        int error;

        if ((flag & ~AT_SYMLINK_NOFOLLOW) != 0)
                return EINVAL;
        if ((error = upath_in(dirfd, uaddr, &up)) != 0)
                return error;
        memset(&va, 0, sizeof(va));
        error = vn_statat(up.up_dir, up.up_path,
            (flag & AT_SYMLINK_NOFOLLOW) != 0 ? NO_FOLLOW : FOLLOW, &va);
        upath_free(&up);
        if (error != 0)
                return error;
        vattr_to_stat(&va, &st);
        return copyout(&st, (void *)(uintptr_t)ubuf, sizeof(st));
}

int
sys_stat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_stat(AT_FDCWD, args[0], args[1], 0);
}

int
sys_lstat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_stat(AT_FDCWD, args[0], args[1], AT_SYMLINK_NOFOLLOW);
}

/* fstatat(dirfd, path, buf, flag) */
int
sys_fstatat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_stat((int)args[0], args[1], args[2], (int)args[3]);
}

/*
 * access(path, mode): whether the file exists, and the access allowed.
 * With every process the superuser, the real and effective ids are the
 * same, so faccessat's AT_EACCESS changes nothing yet.
 */
static int
do_access(int dirfd, uint64_t uaddr, int mode, int flag)
{
        struct vnode *vp;
        int error;

        if ((mode & ~(VREAD | VWRITE | VEXEC)) != 0 ||
            (flag & ~AT_EACCESS) != 0)
                return EINVAL;
        if ((error = upath_lookup(dirfd, uaddr, FOLLOW, &vp)) != 0)
                return error;
        if (mode != 0)
                error = VOP_ACCESS(vp, mode, kcred);
        VN_RELE(vp);
        return error;
}

int
sys_access(const uint64_t *args, uint64_t *rv __unused)
{
        return do_access(AT_FDCWD, args[0], (int)args[1], 0);
}

/* faccessat(dirfd, path, mode, flag) */
int
sys_faccessat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_access((int)args[0], args[1], (int)args[2], (int)args[3]);
}

/*
 * chmod(path, mode). fchmodat with AT_SYMLINK_NOFOLLOW changes a file
 * that is not a link, and fails with EOPNOTSUPP on a link: links have
 * no mode of their own.
 */
static int
do_chmod(int dirfd, uint64_t uaddr, mode_t mode, int flag)
{
        struct vnode *vp;
        struct vattr va;
        int error;

        if ((flag & ~AT_SYMLINK_NOFOLLOW) != 0)
                return EINVAL;
        if ((error = upath_lookup(dirfd, uaddr,
            (flag & AT_SYMLINK_NOFOLLOW) != 0 ? NO_FOLLOW : FOLLOW, &vp)) !=
            0)
                return error;
        if (vp->v_type == VLNK) {
                error = EOPNOTSUPP;
        } else {
                memset(&va, 0, sizeof(va));
                va.va_mask = AT_MODE;
                va.va_mode = mode & 07777;
                error = VOP_SETATTR(vp, &va, kcred);
        }
        VN_RELE(vp);
        return error;
}

int
sys_chmod(const uint64_t *args, uint64_t *rv __unused)
{
        return do_chmod(AT_FDCWD, args[0], (mode_t)args[1], 0);
}

/* fchmodat(dirfd, path, mode, flag) */
int
sys_fchmodat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_chmod((int)args[0], args[1], (mode_t)args[2],
            (int)args[3]);
}

/* chown(path, uid, gid) and lchown, which does not follow a last link. */
static int
do_chown(int dirfd, uint64_t uaddr, uid_t uid, gid_t gid, int flag)
{
        struct vnode *vp;
        int error;

        if ((flag & ~AT_SYMLINK_NOFOLLOW) != 0)
                return EINVAL;
        if ((error = upath_lookup(dirfd, uaddr,
            (flag & AT_SYMLINK_NOFOLLOW) != 0 ? NO_FOLLOW : FOLLOW, &vp)) !=
            0)
                return error;
        error = vn_chown(vp, uid, gid);
        VN_RELE(vp);
        return error;
}

int
sys_chown(const uint64_t *args, uint64_t *rv __unused)
{
        return do_chown(AT_FDCWD, args[0], (uid_t)args[1], (gid_t)args[2],
            0);
}

int
sys_lchown(const uint64_t *args, uint64_t *rv __unused)
{
        return do_chown(AT_FDCWD, args[0], (uid_t)args[1], (gid_t)args[2],
            AT_SYMLINK_NOFOLLOW);
}

/* fchownat(dirfd, path, uid, gid, flag) */
int
sys_fchownat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_chown((int)args[0], args[1], (uid_t)args[2],
            (gid_t)args[3], (int)args[4]);
}

/* Make vp, which the caller holds, the current directory. */
static int
set_cdir(struct vnode *vp)
{
        struct proc *p = curproc;
        struct vnode *old;
        int error;

        if (vp->v_type != VDIR) {
                VN_RELE(vp);
                return ENOTDIR;
        }
        if ((error = VOP_ACCESS(vp, VEXEC, kcred)) != 0) {
                VN_RELE(vp);
                return error;
        }
        mutex_lock(&p->p_lock);
        old = p->p_cdir;
        p->p_cdir = vp;
        mutex_unlock(&p->p_lock);
        if (old != NULL)
                VN_RELE(old);
        return 0;
}

int
sys_chdir(const uint64_t *args, uint64_t *rv __unused)
{
        struct vnode *vp;
        int error;

        error = upath_lookup(AT_FDCWD, args[0], FOLLOW, &vp);
        return error != 0 ? error : set_cdir(vp);
}

int
sys_fchdir(const uint64_t *args, uint64_t *rv __unused)
{
        struct file *fp = fd_get(curproc, (int)args[0]);
        struct vnode *vp;

        if (fp == NULL)
                return EBADF;
        vp = fp->f_vnode;
        VN_HOLD(vp);
        frele(fp);
        return set_cdir(vp);
}
