/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Open files and descriptor tables. See <sys/file.h> and <sys/proc.h>.
 *
 * A process's table is an array of file pointers with a parallel array
 * of close-on-exec flags, grown by doubling up to NOFILE_MAX, under
 * p_lock. Each descriptor holds a reference on its file; fd_get() takes
 * another for the duration of a call, so a close in the middle of a read
 * cannot free the file under it.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/flock.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/stream.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#define NOFILE_INIT     32

static struct kmem_zone *file_zone;

/* A new open file for vp, which the caller has opened; holds vp's ref. */
struct file *
falloc(struct vnode *vp, int flag)
{
        struct file *fp;

        if (file_zone == NULL)
                file_zone = kmem_zone_init(sizeof(struct file), "file");
        fp = kmem_zone_alloc(file_zone, KM_SLEEP);
        fp->f_count = 1;
        fp->f_flag = flag;
        mutex_init(&fp->f_offlock, "file");
        fp->f_offset = 0;
        fp->f_vnode = vp;
        fp->f_cred = CRED();
        crhold(fp->f_cred);
        return fp;
}

void
fhold(struct file *fp)
{
        (void)atomic_inc_32_nv(&fp->f_count);
}

/* Drop a reference; the last drops its flock lock and closes the vnode. */
void
frele(struct file *fp)
{
        if (atomic_dec_32_nv(&fp->f_count) != 0)
                return;
        reclock_release(fp->f_vnode, fp);      /* its flock lock */
        (void)VOP_CLOSE(fp->f_vnode, fp->f_flag, fp->f_cred);
        VN_RELE(fp->f_vnode);
        crfree(fp->f_cred);
        kmem_zone_free(file_zone, fp);
}

/*
 * p gives up a descriptor's reference to fp: its record locks on the
 * file go with it (<sys/flock.h>), then the reference.
 */
static void
fd_release(struct proc *p, struct file *fp)
{
        reclock_release(fp->f_vnode, p);
        frele(fp);
}

/* The descriptors p may have: below RLIMIT_NOFILE. p_lock is held. */
static int
fd_limit(struct proc *p)
{
        rlim_t r = p->p_rlimit[RLIMIT_NOFILE].rlim_cur;

        return r < NOFILE_MAX ? (int)r : NOFILE_MAX;
}

/* Grow p's table to at least n slots. p_lock is held. */
static int
fd_grow(struct proc *p, int n)
{
        int size = p->p_nofile == 0 ? NOFILE_INIT : p->p_nofile;
        struct file **of;
        uint8_t *pof;

        if (n <= p->p_nofile)
                return 0;
        while (size < n)
                size *= 2;
        if (size > NOFILE_MAX)
                size = NOFILE_MAX;
        if (n > size)
                return EMFILE;
        of = kmem_zalloc((size_t)size * sizeof(*of), KM_SLEEP);
        pof = kmem_zalloc((size_t)size, KM_SLEEP);
        if (p->p_ofile != NULL) {
                memcpy(of, p->p_ofile, (size_t)p->p_nofile * sizeof(*of));
                memcpy(pof, p->p_pofile, (size_t)p->p_nofile);
                kmem_free(p->p_ofile, (size_t)p->p_nofile * sizeof(*of));
                kmem_free(p->p_pofile, (size_t)p->p_nofile);
        }
        p->p_ofile = of;
        p->p_pofile = pof;
        p->p_nofile = size;
        return 0;
}

/*
 * Give fp the lowest free descriptor at or above minfd. The table takes
 * over the caller's reference.
 */
int
fd_alloc_flags(struct proc *p, struct file *fp, int minfd, int flags, int *fdp)
{
        int fd, error;

        mutex_lock(&p->p_lock);
        if (minfd < 0 || minfd >= fd_limit(p)) {
                mutex_unlock(&p->p_lock);
                return EINVAL;
        }
        for (fd = minfd;; fd++) {
                if (fd >= fd_limit(p)) {
                        mutex_unlock(&p->p_lock);
                        return EMFILE;
                }
                if (fd >= p->p_nofile && (error = fd_grow(p, fd + 1)) != 0) {
                        mutex_unlock(&p->p_lock);
                        return error;
                }
                if (p->p_ofile[fd] == NULL)
                        break;
        }
        p->p_ofile[fd] = fp;
        p->p_pofile[fd] = (uint8_t)flags;
        mutex_unlock(&p->p_lock);
        *fdp = fd;
        return 0;
}

int
fd_alloc(struct proc *p, struct file *fp, int minfd, int *fdp)
{
        return fd_alloc_flags(p, fp, minfd, 0, fdp);
}

/* Publish both ends together, with their close-on-exec flags. */
int
fd_alloc_pair(struct proc *p, struct file *files[2], int flags, int fds[2])
{
        int found = 0, error = 0;

        mutex_lock(&p->p_lock);
        for (int fd = 0; fd < fd_limit(p); fd++) {
                if (fd >= p->p_nofile && (error = fd_grow(p, fd + 1)) != 0)
                        break;
                if (p->p_ofile[fd] == NULL) {
                        fds[found++] = fd;
                        if (found == 2)
                                break;
                }
        }
        if (found == 2) {
                for (int i = 0; i < 2; i++) {
                        p->p_ofile[fds[i]] = files[i];
                        p->p_pofile[fds[i]] = (uint8_t)flags;
                }
        } else if (error == 0) {
                error = EMFILE;
        }
        mutex_unlock(&p->p_lock);
        return error;
}

/*
 * Put fp at descriptor fd exactly, closing what was there (dup2). The
 * table takes over the caller's reference.
 */
int
fd_install(struct proc *p, struct file *fp, int fd)
{
        struct file *old;
        int error;

        mutex_lock(&p->p_lock);
        if (fd < 0 || fd >= fd_limit(p)) {
                mutex_unlock(&p->p_lock);
                return EBADF;
        }
        if (fd >= p->p_nofile && (error = fd_grow(p, fd + 1)) != 0) {
                mutex_unlock(&p->p_lock);
                return error;
        }
        old = p->p_ofile[fd];
        if (old != NULL && old->f_vnode->v_stream != NULL)
                strsig_close(old->f_vnode->v_stream, p);
        p->p_ofile[fd] = fp;
        p->p_pofile[fd] = 0;
        mutex_unlock(&p->p_lock);
        if (old != NULL)
                fd_release(p, old);
        return 0;
}

/* The file at fd, with a reference for the caller, or NULL. */
struct file *
fd_get(struct proc *p, int fd)
{
        struct file *fp = NULL;

        mutex_lock(&p->p_lock);
        if (fd >= 0 && fd < p->p_nofile && (fp = p->p_ofile[fd]) != NULL)
                fhold(fp);
        mutex_unlock(&p->p_lock);
        return fp;
}

int
fd_close(struct proc *p, int fd)
{
        struct file *fp = NULL;

        mutex_lock(&p->p_lock);
        if (fd >= 0 && fd < p->p_nofile) {
                fp = p->p_ofile[fd];
                if (fp != NULL && fp->f_vnode->v_stream != NULL)
                        strsig_close(fp->f_vnode->v_stream, p);
                p->p_ofile[fd] = NULL;
        }
        mutex_unlock(&p->p_lock);
        if (fp == NULL)
                return EBADF;
        fd_release(p, fp);
        return 0;
}

/* Close-on-exec flag of fd: get with set < 0, else set it. */
int
fd_cloexec(struct proc *p, int fd, int set)
{
        int r;

        mutex_lock(&p->p_lock);
        if (fd < 0 || fd >= p->p_nofile || p->p_ofile[fd] == NULL) {
                mutex_unlock(&p->p_lock);
                return -1;
        }
        if (set >= 0)
                p->p_pofile[fd] = set ? FD_CLOEXEC : 0;
        r = p->p_pofile[fd];
        mutex_unlock(&p->p_lock);
        return r;
}

/* fork: give the child the parent's descriptors, sharing the files. */
void
fd_dup_table(struct proc *parent, struct proc *child)
{
        mutex_lock(&parent->p_lock);
        if (parent->p_nofile != 0) {
                (void)fd_grow(child, parent->p_nofile);
                for (int fd = 0; fd < parent->p_nofile; fd++) {
                        struct file *fp = parent->p_ofile[fd];

                        if (fp != NULL) {
                                fhold(fp);
                                child->p_ofile[fd] = fp;
                                child->p_pofile[fd] = parent->p_pofile[fd];
                        }
                }
        }
        mutex_unlock(&parent->p_lock);
}

static void
fd_close_if(struct proc *p, int exec_only)
{
        for (int fd = 0; fd < p->p_nofile; fd++) {
                struct file *fp = NULL;

                mutex_lock(&p->p_lock);
                if (p->p_ofile[fd] != NULL &&
                    (!exec_only || (p->p_pofile[fd] & FD_CLOEXEC) != 0)) {
                        fp = p->p_ofile[fd];
                        if (fp->f_vnode->v_stream != NULL)
                                strsig_close(fp->f_vnode->v_stream, p);
                        p->p_ofile[fd] = NULL;
                }
                mutex_unlock(&p->p_lock);
                if (fp != NULL)
                        fd_release(p, fp);
        }
}

void
fd_close_all(struct proc *p)
{
        fd_close_if(p, 0);
}

void
fd_close_exec(struct proc *p)
{
        fd_close_if(p, 1);
}
