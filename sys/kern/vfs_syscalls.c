/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * System calls on path names. Each copies its paths in and calls the
 * kernel file interface (vfs_vnops.c); relative paths start at the
 * process's current directory (lookuppn). There are no permission
 * checks beyond the file systems' own, since every process runs with
 * the kernel's credentials until security is in scope.
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

/* Copy a user path into a new MAXPATHLEN buffer. */
static int
path_in(uint64_t upath, char **kpathp)
{
        char *kp = kmem_alloc(MAXPATHLEN, KM_SLEEP);
        int error = copyinstr((const char *)(uintptr_t)upath, kp,
            MAXPATHLEN, NULL);

        if (error != 0) {
                kmem_free(kp, MAXPATHLEN);
                return error;
        }
        *kpathp = kp;
        return 0;
}

static void
path_free(char *kp)
{
        kmem_free(kp, MAXPATHLEN);
}

static int
do_open(uint64_t upath, int oflag, mode_t mode, uint64_t *rv)
{
        struct proc *p = curproc;
        int fflag = ((oflag & O_ACCMODE) + 1) | (oflag & ~O_ACCMODE);
        struct vnode *vp;
        struct file *fp;
        char *path;
        int fd, error;

        if ((oflag & O_ACCMODE) == O_ACCMODE)
                return EINVAL;
        if ((error = path_in(upath, &path)) != 0)
                return error;
        error = vn_open(path, fflag, mode & ~p->p_cmask & 07777, &vp);
        path_free(path);
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
        return do_open(args[0], (int)args[1], (mode_t)args[2], rv);
}

int
sys_creat(const uint64_t *args, uint64_t *rv)
{
        return do_open(args[0], O_WRONLY | O_CREAT | O_TRUNC,
            (mode_t)args[1], rv);
}

/* Calls of one path. */
static int
path1(uint64_t upath, int (*fn)(const char *))
{
        char *path;
        int error;

        if ((error = path_in(upath, &path)) != 0)
                return error;
        error = fn(path);
        path_free(path);
        return error;
}

/* Calls of two paths. */
static int
path2(uint64_t a, uint64_t b, int (*fn)(const char *, const char *))
{
        char *pa, *pb;
        int error;

        if ((error = path_in(a, &pa)) != 0)
                return error;
        if ((error = path_in(b, &pb)) != 0) {
                path_free(pa);
                return error;
        }
        error = fn(pa, pb);
        path_free(pa);
        path_free(pb);
        return error;
}

int
sys_unlink(const uint64_t *args, uint64_t *rv __unused)
{
        return path1(args[0], vn_remove);
}

int
sys_rmdir(const uint64_t *args, uint64_t *rv __unused)
{
        return path1(args[0], vn_rmdir);
}

int
sys_link(const uint64_t *args, uint64_t *rv __unused)
{
        return path2(args[0], args[1], vn_link);
}

int
sys_rename(const uint64_t *args, uint64_t *rv __unused)
{
        return path2(args[0], args[1], vn_rename);
}

/* symlink(target, path) */
int
sys_symlink(const uint64_t *args, uint64_t *rv __unused)
{
        return path2(args[0], args[1], vn_symlink);
}

int
sys_mkdir(const uint64_t *args, uint64_t *rv __unused)
{
        char *path;
        int error;

        if ((error = path_in(args[0], &path)) != 0)
                return error;
        error = vn_mkdir(path, (mode_t)args[1] & ~curproc->p_cmask & 07777);
        path_free(path);
        return error;
}

/*
 * mknod(path, mode, dev): mode's type bits say what to make. Only
 * FIFOs (S_IFIFO) can be made so far; dev is for device files.
 */
int
sys_mknod(const uint64_t *args, uint64_t *rv __unused)
{
        mode_t mode = (mode_t)args[1];
        char *path;
        int error;

        if ((mode & S_IFMT) != S_IFIFO)
                return EINVAL;
        if ((error = path_in(args[0], &path)) != 0)
                return error;
        error = vn_mknod(path, VFIFO, mode & ~curproc->p_cmask & 07777);
        path_free(path);
        return error;
}

/* readlink(path, buf, size): the bytes of the link, not terminated. */
int
sys_readlink(const uint64_t *args, uint64_t *rv)
{
        char *path, *buf;
        size_t len, size = (size_t)args[2];
        int error;

        if (size == 0)
                return EINVAL;
        if ((error = path_in(args[0], &path)) != 0)
                return error;
        buf = kmem_alloc(MAXPATHLEN, KM_SLEEP);
        error = vn_readlink(path, buf, MAXPATHLEN, &len);
        path_free(path);
        if (error == 0) {
                if (len > size)
                        len = size;
                error = copyout(buf, (void *)(uintptr_t)args[1], len);
                rv[0] = len;
        }
        kmem_free(buf, MAXPATHLEN);
        return error;
}

static int
do_stat(uint64_t upath, uint64_t ubuf, int follow)
{
        struct vattr va;
        struct stat st;
        char *path;
        int error;

        if ((error = path_in(upath, &path)) != 0)
                return error;
        memset(&va, 0, sizeof(va));
        error = vn_stat(path, follow, &va);
        path_free(path);
        if (error != 0)
                return error;
        vattr_to_stat(&va, &st);
        return copyout(&st, (void *)(uintptr_t)ubuf, sizeof(st));
}

int
sys_stat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_stat(args[0], args[1], FOLLOW);
}

int
sys_lstat(const uint64_t *args, uint64_t *rv __unused)
{
        return do_stat(args[0], args[1], NO_FOLLOW);
}

/* access(path, mode): whether the file exists, and the access allowed. */
int
sys_access(const uint64_t *args, uint64_t *rv __unused)
{
        int mode = (int)args[1];
        struct vnode *vp;
        char *path;
        int error;

        if ((mode & ~(VREAD | VWRITE | VEXEC)) != 0)
                return EINVAL;
        if ((error = path_in(args[0], &path)) != 0)
                return error;
        error = lookupname(path, FOLLOW, &vp);
        path_free(path);
        if (error != 0)
                return error;
        if (mode != 0)
                error = VOP_ACCESS(vp, mode, kcred);
        VN_RELE(vp);
        return error;
}

int
sys_chmod(const uint64_t *args, uint64_t *rv __unused)
{
        struct vnode *vp;
        struct vattr va;
        char *path;
        int error;

        if ((error = path_in(args[0], &path)) != 0)
                return error;
        error = lookupname(path, FOLLOW, &vp);
        path_free(path);
        if (error != 0)
                return error;
        memset(&va, 0, sizeof(va));
        va.va_mask = AT_MODE;
        va.va_mode = (mode_t)args[1] & 07777;
        error = VOP_SETATTR(vp, &va, kcred);
        VN_RELE(vp);
        return error;
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
        char *path;
        int error;

        if ((error = path_in(args[0], &path)) != 0)
                return error;
        error = lookupname(path, FOLLOW, &vp);
        path_free(path);
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
