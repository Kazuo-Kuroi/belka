/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * procfs: /proc as IRIX has it, System V Release 4.0's flat file system
 * (see <sys/procfs.h>):
 *
 *   /proc/NNNNN        a process, by id; its real user's, mode 0600
 *   /proc/pinfo/NNNNN  the same process, for anyone, mode 0444
 *
 * Both answer PIOCPSINFO. Reading a process's address space through its
 * file, and the debugging ioctls, are not here yet: read() gives EIO.
 *
 * The two directories are static vnodes; a process's file is a private
 * vnode made at each lookup and freed when its last reference goes,
 * leaving the name cache first. It names the process by id, and asks
 * after it under proctree_lock each time, so a file whose process has
 * gone answers ENOENT.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dirent.h>
#include <sys/dnlc.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/procfs.h>
#include <sys/schedclass.h>
#include <sys/specvn.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <vm/as.h>

enum { PR_ROOT, PR_PINFODIR, PR_NDIRS };

struct prnode {
        struct vnode    pn_vn;          /* first: the vnode is the node */
        pid_t           pn_pid;
        int             pn_pinfo;       /* under /proc/pinfo */
};

#define VTOPN(vp)       ((struct prnode *)(void *)(vp))

static struct specattr pr_dattr[PR_NDIRS];
static struct vnode pr_dvn[PR_NDIRS];
static struct vfs *procfs_vfs;

static const struct vnodeops procfs_dirops, procfs_fileops;

static int
procfs_mount(struct vfs *vfsp, const void *data __unused,
    struct cred *cr __unused)
{
        if (procfs_vfs != NULL)
                return EBUSY;
        procfs_vfs = vfsp;
        for (int i = 0; i < PR_NDIRS; i++) {
                specattr_init(&pr_dattr[i], 0555, 0, 0, 0);
                vn_init(&pr_dvn[i], vfsp, VDIR, &procfs_dirops,
                    &pr_dattr[i]);
                VN_HOLD(&pr_dvn[i]);    /* never released */
        }
        pr_dvn[PR_ROOT].v_flag |= VROOT;
        vfsp->vfs_bsize = PAGESIZE;
        return 0;
}

static int
procfs_unmount(struct vfs *vfsp __unused, struct cred *cr __unused)
{
        return EBUSY;
}

static int
procfs_root(struct vfs *vfsp __unused, struct vnode **vpp)
{
        *vpp = &pr_dvn[PR_ROOT];
        VN_HOLD(*vpp);
        return 0;
}

static int
procfs_statvfs(struct vfs *vfsp, struct statvfs *sp)
{
        memset(sp, 0, sizeof(*sp));
        sp->f_bsize = sp->f_frsize = PAGESIZE;
        sp->f_fsid = vfsp->vfs_fsid;
        memcpy(sp->f_basetype, "proc", 5);
        sp->f_namemax = 16;
        return 0;
}

static int
procfs_sync(struct vfs *vfsp __unused, struct cred *cr __unused)
{
        return 0;
}

const struct vfsops procfs_vfsops = {
        procfs_mount, procfs_unmount, procfs_root, procfs_statvfs,
        procfs_sync
};

/* --- What is known of a process. --- */

/*
 * Fill *ps for process pid. ENOENT if there is none (or it is being
 * made). The terminal is looked up after proctree_lock is dropped.
 */
static int
pr_psinfo(pid_t pid, struct prpsinfo *ps)
{
        struct vnode *tvp;
        struct proc *p;
        struct cred *cr;
        uint64_t all, user;
        pid_t sid;
        int s;

        memset(ps, 0, sizeof(*ps));
        mutex_lock(&proctree_lock);
        if ((p = proc_find_locked(pid)) == NULL || p->p_stat == SIDL) {
                mutex_unlock(&proctree_lock);
                return ENOENT;
        }
        cr = p->p_cred;
        ps->pr_uid = cr->cr_ruid;
        ps->pr_gid = cr->cr_rgid;
        ps->pr_euid = cr->cr_uid;
        ps->pr_egid = cr->cr_gid;
        ps->pr_pid = p->p_pid;
        ps->pr_ppid = p->p_parent != NULL ? p->p_parent->p_pid : 0;
        ps->pr_pgrp = p->p_pgid;
        ps->pr_sid = sid = p->p_sid;
        ps->pr_nice = (char)p->p_nice;
        ps->pr_flag = (p->p_flags & P_SYSTEM) != 0 ? PR_ISSYS : 0;
        ps->pr_start[0] = p->p_start[0];
        ps->pr_start[1] = p->p_start[1];
        ps->pr_ctime[0] = (int64_t)(p->p_cticks / HZ);
        ps->pr_ctime[1] = (int64_t)(p->p_cticks % HZ) * (NANOSEC / HZ);
        memcpy(ps->pr_fname, p->p_comm, sizeof(ps->pr_fname) - 1);
        memcpy(ps->pr_psargs, p->p_psargs, sizeof(ps->pr_psargs) - 1);
        if (ps->pr_psargs[0] == '\0')
                memcpy(ps->pr_psargs, p->p_comm, sizeof(ps->pr_fname) - 1);
        /* p_as is replaced under proctree_lock (exec, exit). */
        if (p->p_as != NULL)
                ps->pr_size = as_size(p->p_as) / PAGESIZE;

        if (p->p_stat == SZOMB) {
                ps->pr_zomb = 1;
                ps->pr_sname = 'Z';
                ps->pr_state = KT_ZOMB + 1;
        } else {
                struct kthread *kt;

                all = proc_ticks(p, &user);
                ps->pr_time[0] = (int64_t)(all / HZ);
                ps->pr_time[1] = (int64_t)(all % HZ) * (NANOSEC / HZ);
                s = spin_lock(&p->p_siglock);
                ps->pr_nlwp = p->p_nlwp;
                kt = p->p_lwps != NULL ? p->p_lwps->l_kthread : NULL;
                if (kt != NULL) {
                        int st = kt->kt_state;

                        ps->pr_state = (char)(st + 1);
                        ps->pr_sname = p->p_stopped ? 'T' :
                            st == KT_ONPROC ? 'O' : st == KT_SLEEP ? 'S' :
                            'R';
                        ps->pr_pri = kt->kt_pri;
                        if (kt->kt_class != NULL)
                                snprintf(ps->pr_clname, PRCLSZ, "%s",
                                    kt->kt_class->cl_name);
                } else {
                        ps->pr_sname = 'S';
                        ps->pr_state = KT_SLEEP + 1;
                }
                spin_unlock(&p->p_siglock, s);
        }
        mutex_unlock(&proctree_lock);

        ps->pr_ttydev = PRNODEV;
        if ((tvp = tty_ctty_vnode(sid)) != NULL) {
                struct vattr va;

                va.va_mask = AT_ALL;
                if (VOP_GETATTR(tvp, &va, kcred) == 0)
                        ps->pr_ttydev = va.va_rdev;
                VN_RELE(tvp);
        }
        return 0;
}

/* The owner of process pid's file, for its attributes; ENOENT if gone. */
static int
pr_owner(pid_t pid, uid_t *uidp, gid_t *gidp, int64_t start[2])
{
        struct proc *p;

        mutex_lock(&proctree_lock);
        if ((p = proc_find_locked(pid)) == NULL || p->p_stat == SIDL) {
                mutex_unlock(&proctree_lock);
                return ENOENT;
        }
        *uidp = p->p_cred->cr_ruid;
        *gidp = p->p_cred->cr_rgid;
        start[0] = p->p_start[0];
        start[1] = p->p_start[1];
        mutex_unlock(&proctree_lock);
        return 0;
}

/* --- The directories. --- */

static int
pr_open(struct vnode **vpp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
pr_close(struct vnode *vp __unused, int flags __unused,
    struct cred *cr __unused)
{
        return 0;
}

static int
pr_dirrw(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return EISDIR;
}

static struct vnode *
pr_mknode(pid_t pid, int pinfo)
{
        struct prnode *pn = kmem_zalloc(sizeof(*pn), KM_SLEEP);

        pn->pn_pid = pid;
        pn->pn_pinfo = pinfo;
        vn_init(&pn->pn_vn, procfs_vfs, VREG, &procfs_fileops, pn);
        VN_HOLD(&pn->pn_vn);            /* the caller's */
        return &pn->pn_vn;
}

static int
pr_lookup(struct vnode *dvp, const char *nm, struct vnode **vpp,
    struct cred *cr __unused)
{
        uid_t uid;
        gid_t gid;
        int64_t start[2];
        long pid = 0;

        if (strcmp(nm, ".") == 0 || (strcmp(nm, "..") == 0 &&
            dvp == &pr_dvn[PR_ROOT])) {
                *vpp = dvp;
                VN_HOLD(dvp);
                return 0;
        }
        if (strcmp(nm, "..") == 0) {
                *vpp = &pr_dvn[PR_ROOT];
                VN_HOLD(*vpp);
                return 0;
        }
        if (dvp == &pr_dvn[PR_ROOT] && strcmp(nm, "pinfo") == 0) {
                *vpp = &pr_dvn[PR_PINFODIR];
                VN_HOLD(*vpp);
                return 0;
        }
        if (nm[0] == '\0')
                return ENOENT;
        for (const char *c = nm; *c != '\0'; c++) {
                if (*c < '0' || *c > '9' || pid > PID_MAX)
                        return ENOENT;
                pid = pid * 10 + (*c - '0');
        }
        if (pid > PID_MAX || pr_owner((pid_t)pid, &uid, &gid, start) != 0)
                return ENOENT;
        *vpp = pr_mknode((pid_t)pid, dvp == &pr_dvn[PR_PINFODIR]);
        return 0;
}

/* Put one record at position idx; 0, or -1 if it does not fit. */
static int
pr_dirent(struct uio *uio, off_t idx, ino_t ino, const char *name,
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

struct nextpid {
        pid_t   np_from;
        pid_t   np_found;       /* the least id >= np_from, or 0 */
};

static void
nextpid_one(struct proc *p, void *arg)
{
        struct nextpid *np = arg;

        if (p->p_stat != SIDL && p->p_pid >= np->np_from &&
            (np->np_found == 0 || p->p_pid < np->np_found))
                np->np_found = p->p_pid;
}

/*
 * The least process id at or above from, or -1. Process 0 is not listed:
 * it is the kernel, and not in the pid hash.
 */
static pid_t
pr_nextpid(pid_t from)
{
        struct nextpid np = { from > 0 ? from : 1, 0 };

        mutex_lock(&proctree_lock);
        proc_iterate_locked(nextpid_one, &np);
        mutex_unlock(&proctree_lock);
        return np.np_found != 0 ? np.np_found : -1;
}

/*
 * Positions: 0 ".", 1 "..", 2 "pinfo" (in /proc only), then from
 * PR_FIRSTPID each process, at PR_FIRSTPID plus its id, in id order.
 */
#define PR_FIRSTPID     3

static int
pr_readdir(struct vnode *vp, struct uio *uio, int *eofp,
    struct cred *cr __unused)
{
        int root = vp == &pr_dvn[PR_ROOT], error = 0;

        *eofp = 0;
        for (off_t idx = uio->uio_offset; error == 0; idx++) {
                char name[16];
                ino_t ino;

                if (idx == 0) {
                        snprintf(name, sizeof(name), ".");
                        ino = root ? 2 : 3;
                } else if (idx == 1) {
                        snprintf(name, sizeof(name), "..");
                        ino = 2;
                } else if (idx == 2) {
                        if (!root)
                                continue;
                        snprintf(name, sizeof(name), "pinfo");
                        ino = 3;
                } else {
                        pid_t pid = pr_nextpid((pid_t)(idx - PR_FIRSTPID));

                        if (pid < 0) {
                                *eofp = 1;
                                break;
                        }
                        idx = PR_FIRSTPID + pid;
                        snprintf(name, sizeof(name), "%05d", (int)pid);
                        ino = 16 + (ino_t)pid * 2 + (root ? 0 : 1);
                }
                if (pr_dirent(uio, idx, ino, name, &error) != 0)
                        break;
        }
        return error;
}

static int
pr_dinactive(struct vnode *vp, struct cred *cr __unused)
{
        vn_inactive_done(vp);
        return VN_INACTIVE_CACHE;
}

static const struct vnodeops procfs_dirops = {
        pr_open, pr_close, pr_dirrw, pr_dirrw, spec_getattr,
        spec_setattr, spec_access, pr_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, pr_readdir,
        spec_symlink, spec_readlink, spec_fsync, pr_dinactive,
        spec_putpage, spec_reclaim, spec_getpage, fs_poll, NULL
};

/* --- A process's file. --- */

static int
pr_getattr(struct vnode *vp, struct vattr *va, struct cred *cr __unused)
{
        struct prnode *pn = VTOPN(vp);
        uid_t uid;
        gid_t gid;
        int64_t start[2];
        int error;

        if ((error = pr_owner(pn->pn_pid, &uid, &gid, start)) != 0)
                return error;
        memset(va, 0, sizeof(*va));
        va->va_type = VREG;
        va->va_mode = pn->pn_pinfo ? 0444 : 0600;
        va->va_uid = pn->pn_pinfo ? 0 : uid;
        va->va_gid = pn->pn_pinfo ? 0 : gid;
        va->va_fsid = procfs_vfs->vfs_fsid;
        va->va_nodeid = 16 + (uint64_t)pn->pn_pid * 2 + (pn->pn_pinfo ? 1 : 0);
        va->va_nlink = 1;
        va->va_atime.tv_sec = va->va_mtime.tv_sec = va->va_ctime.tv_sec =
            start[0];
        va->va_atime.tv_nsec = va->va_mtime.tv_nsec = va->va_ctime.tv_nsec =
            start[1];
        va->va_blksize = PAGESIZE;
        return 0;
}

static int
pr_setattr(struct vnode *vp __unused, struct vattr *va __unused,
    struct cred *cr __unused)
{
        return EPERM;
}

static int
pr_access(struct vnode *vp, int mode, struct cred *cr)
{
        struct prnode *pn = VTOPN(vp);
        uid_t uid;
        gid_t gid;
        int64_t start[2];
        int error;

        if ((error = pr_owner(pn->pn_pid, &uid, &gid, start)) != 0)
                return error;
        if (pn->pn_pinfo)
                return vaccess(VREG, 0444, 0, 0, mode, cr);
        return vaccess(VREG, 0600, uid, gid, mode, cr);
}

static int
pr_read(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return EIO;             /* no address-space reads yet */
}

static int
pr_write(struct vnode *vp __unused, struct uio *uio __unused,
    int ioflag __unused, struct cred *cr __unused)
{
        return EIO;
}

static int
pr_ioctl(struct vnode *vp, unsigned long cmd, uint64_t arg,
    int flags __unused, struct cred *cr __unused)
{
        struct prnode *pn = VTOPN(vp);
        struct prpsinfo ps;
        int error;

        switch (cmd) {
        case PIOCPSINFO:
                if ((error = pr_psinfo(pn->pn_pid, &ps)) != 0)
                        return error;
                return copyout(&ps, (void *)(uintptr_t)arg, sizeof(ps));
        default:
                return EINVAL;
        }
}

/* The last reference: out of the name cache, and freed. */
static int
pr_finactive(struct vnode *vp, struct cred *cr __unused)
{
        dnlc_purge_vp(vp);
        vn_destroy(vp);
        kmem_free(VTOPN(vp), sizeof(struct prnode));
        return VN_INACTIVE_NOCACHE;
}

static const struct vnodeops procfs_fileops = {
        pr_open, pr_close, pr_read, pr_write, pr_getattr,
        pr_setattr, pr_access, spec_lookup, spec_create, spec_name1,
        spec_link, spec_rename, spec_mkdir, spec_name1, spec_readdir,
        spec_symlink, spec_readlink, spec_fsync, pr_finactive,
        spec_putpage, spec_reclaim, spec_getpage, fs_poll, pr_ioctl
};
