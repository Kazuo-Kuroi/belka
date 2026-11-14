/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Vnodes: the file system independent face of a file, after System V,
 * with IRIX's concurrency contract.
 *
 * - The generic layer holds a reference (VN_HOLD) across every vnode
 *   operation and takes no vnode lock around it. Each file system
 *   serialises its own operations.
 * - When the last reference goes (VN_RELE), VOP_INACTIVE runs, with no
 *   locks held and VINACT set.
 *
 * Vnodes come in two kinds:
 *
 * - Shared vnodes (VSHARE) belong to the global vnode cache (IRIX), and
 *   come from vn_alloc(). VOP_INACTIVE returns VN_INACTIVE_CACHE to keep
 *   the vnode's identity, and the vnode then waits on a free list,
 *   pages and all, for its file system to find it again; or it returns
 *   VN_INACTIVE_NOCACHE after tearing its node down. vn_alloc() reuses
 *   free vnodes, least recently freed first, through VOP_RECLAIM, once
 *   there are at least as many free vnodes as vnodes in use. Their
 *   memory is never freed, so a pointer to one always points at a
 *   vnode; v_number tells whether it is still the same one.
 * - Private vnodes are embedded in the file system's own node and set up
 *   with vn_init() (tmpfs). The file system frees them with the node,
 *   calling vn_destroy() first; after VOP_INACTIVE the generic layer
 *   does not touch them. Their pages never go on the cache lists.
 *
 * A file system that finds a cached vnode through its own index takes a
 * reference with VN_HOLD while holding the lock that its VOP_INACTIVE
 * and VOP_RECLAIM take, so it can never meet one that is half torn down.
 * The name cache keeps soft references (pointer and v_number) and takes
 * real ones with vn_get(), which fails on a vnode being inactivated or
 * reclaimed.
 *
 * Operations return 0 or an errno value.
 */

#ifndef _SYS_VNODE_H_
#define _SYS_VNODE_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/pathname.h>
#include <sys/spinlock.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <vm/pvn.h>

struct cred;
struct page;
struct pollhead;
struct uio;
struct vfs;
struct vnode;

enum vtype { VNON, VREG, VDIR, VBLK, VCHR, VLNK, VFIFO, VSOCK };

/* v_flag, under v_lock */
#define VROOT           0x0001  /* root of its file system */
#define VINACT          0x0002  /* VOP_INACTIVE in progress */
#define VRECLM          0x0004  /* being reclaimed */
#define VFREE           0x0008  /* on a free list */
#define VREINACT        0x0010  /* released again during VOP_INACTIVE */
#define VSHARE          0x0020  /* in the global vnode cache */

struct vnode {
        struct spinlock v_lock;
        volatile uint32_t v_count;
        uint32_t        v_flag;
        enum vtype      v_type;
        struct vfs      *v_vfsp;        /* file system holding it */
        struct vfs      *v_vfsmountedhere; /* file system mounted on it */
        const struct vnodeops *v_op;
        void            *v_data;        /* the file system's node */
        dev_t           v_rdev;
        uint64_t        v_number;       /* identity, new on each reuse */
        volatile uint32_t v_nmap;       /* segments mapping it */

        /* The global vnode cache. */
        struct vnode    *v_fnext;       /* free list, under its lock */
        struct vnode    *v_fprev;
        int             v_listid;       /* which free list */
        struct vnode    *v_allnext;     /* every shared vnode */
        struct vnode    *v_dnext;       /* dirty vnodes, under their lock */
        struct vnode    *v_dprev;
        int             v_ondirty;
        uint32_t        v_flushgen;     /* last flush pass that saw it */

        struct vpcache  v_pc;           /* its pages */
};

/* VOP_INACTIVE results, for shared vnodes. */
#define VN_INACTIVE_CACHE       0       /* keep the identity cached */
#define VN_INACTIVE_NOCACHE     1       /* the node is gone */

/* VOP_RECLAIM flags. */
#define VRC_NOWAIT      0x0001  /* EBUSY rather than wait for a lock */

/* Attributes; va_mask says which are meant. */
struct vattr {
        uint32_t        va_mask;
        enum vtype      va_type;
        mode_t          va_mode;        /* permission bits */
        uid_t           va_uid;
        gid_t           va_gid;
        dev_t           va_fsid;
        ino_t           va_nodeid;
        nlink_t         va_nlink;
        off_t           va_size;
        struct timespec va_atime;
        struct timespec va_mtime;
        struct timespec va_ctime;
        dev_t           va_rdev;
        uint32_t        va_blksize;
        uint64_t        va_nblocks;     /* 512-byte blocks */
};

#define AT_TYPE         0x0001
#define AT_MODE         0x0002
#define AT_UID          0x0004
#define AT_GID          0x0008
#define AT_SIZE         0x0010
#define AT_ATIME        0x0020
#define AT_MTIME        0x0040
#define AT_CTIME        0x0080
#define AT_ALL          0xffff

/* VOP_ACCESS modes. */
#define VREAD           04
#define VWRITE          02
#define VEXEC           01

/* VOP_CREATE exclusive argument. */
#define NONEXCL         0
#define EXCL            1

struct vnodeops {
        int     (*vop_open)(struct vnode **, int, struct cred *);
        int     (*vop_close)(struct vnode *, int, struct cred *);
        int     (*vop_read)(struct vnode *, struct uio *, int, struct cred *);
        int     (*vop_write)(struct vnode *, struct uio *, int, struct cred *);
        int     (*vop_getattr)(struct vnode *, struct vattr *, struct cred *);
        int     (*vop_setattr)(struct vnode *, struct vattr *, struct cred *);
        int     (*vop_access)(struct vnode *, int, struct cred *);
        int     (*vop_lookup)(struct vnode *, const char *, struct vnode **,
                    struct cred *);
        int     (*vop_create)(struct vnode *, const char *, struct vattr *,
                    int, struct vnode **, struct cred *);
        int     (*vop_remove)(struct vnode *, const char *, struct cred *);
        int     (*vop_link)(struct vnode *, struct vnode *, const char *,
                    struct cred *);
        int     (*vop_rename)(struct vnode *, const char *, struct vnode *,
                    const char *, struct cred *);
        int     (*vop_mkdir)(struct vnode *, const char *, struct vattr *,
                    struct vnode **, struct cred *);
        int     (*vop_rmdir)(struct vnode *, const char *, struct cred *);
        int     (*vop_readdir)(struct vnode *, struct uio *, int *,
                    struct cred *);
        int     (*vop_symlink)(struct vnode *, const char *, struct vattr *,
                    const char *, struct cred *);
        int     (*vop_readlink)(struct vnode *, struct uio *, struct cred *);
        int     (*vop_fsync)(struct vnode *, struct cred *);
        int     (*vop_inactive)(struct vnode *, struct cred *);
        int     (*vop_putpage)(struct vnode *, int, struct cred *);
        int     (*vop_reclaim)(struct vnode *, int);
        int     (*vop_getpage)(struct vnode *, off_t, struct page **,
                    struct cred *);
        int     (*vop_poll)(struct vnode *, short, short *,
                    struct pollhead **, struct cred *);
        /* Device control; NULL (as for files) is ENOTTY. */
        int     (*vop_ioctl)(struct vnode *, unsigned long, uint64_t, int,
                    struct cred *);
};

#define VOP_OPEN(vpp, f, cr)            (*(vpp))->v_op->vop_open(vpp, f, cr)
#define VOP_CLOSE(vp, f, cr)            (vp)->v_op->vop_close(vp, f, cr)
#define VOP_READ(vp, uio, f, cr)        (vp)->v_op->vop_read(vp, uio, f, cr)
#define VOP_WRITE(vp, uio, f, cr)       (vp)->v_op->vop_write(vp, uio, f, cr)
#define VOP_GETATTR(vp, va, cr)         (vp)->v_op->vop_getattr(vp, va, cr)
#define VOP_SETATTR(vp, va, cr)         (vp)->v_op->vop_setattr(vp, va, cr)
#define VOP_ACCESS(vp, m, cr)           (vp)->v_op->vop_access(vp, m, cr)
#define VOP_LOOKUP(dvp, nm, vpp, cr)    (dvp)->v_op->vop_lookup(dvp, nm, vpp, cr)
#define VOP_CREATE(dvp, nm, va, ex, vpp, cr) \
        (dvp)->v_op->vop_create(dvp, nm, va, ex, vpp, cr)
#define VOP_REMOVE(dvp, nm, cr)         (dvp)->v_op->vop_remove(dvp, nm, cr)
#define VOP_LINK(tdvp, vp, nm, cr)      (tdvp)->v_op->vop_link(tdvp, vp, nm, cr)
#define VOP_RENAME(sdvp, snm, tdvp, tnm, cr) \
        (sdvp)->v_op->vop_rename(sdvp, snm, tdvp, tnm, cr)
#define VOP_MKDIR(dvp, nm, va, vpp, cr) (dvp)->v_op->vop_mkdir(dvp, nm, va, vpp, cr)
#define VOP_RMDIR(dvp, nm, cr)          (dvp)->v_op->vop_rmdir(dvp, nm, cr)
#define VOP_READDIR(vp, uio, eof, cr)   (vp)->v_op->vop_readdir(vp, uio, eof, cr)
#define VOP_SYMLINK(dvp, nm, va, tg, cr) \
        (dvp)->v_op->vop_symlink(dvp, nm, va, tg, cr)
#define VOP_READLINK(vp, uio, cr)       (vp)->v_op->vop_readlink(vp, uio, cr)
#define VOP_FSYNC(vp, cr)               (vp)->v_op->vop_fsync(vp, cr)
#define VOP_INACTIVE(vp, cr)            (vp)->v_op->vop_inactive(vp, cr)
#define VOP_PUTPAGE(vp, fl, cr)         (vp)->v_op->vop_putpage(vp, fl, cr)
#define VOP_RECLAIM(vp, fl)             (vp)->v_op->vop_reclaim(vp, fl)
#define VOP_GETPAGE(vp, off, ppp, cr)   (vp)->v_op->vop_getpage(vp, off, ppp, cr)
#define VOP_POLL(vp, ev, rev, php, cr) \
        (vp)->v_op->vop_poll(vp, ev, rev, php, cr)
#define VOP_IOCTL(vp, cmd, arg, f, cr) \
        ((vp)->v_op->vop_ioctl != NULL ? \
        (vp)->v_op->vop_ioctl(vp, cmd, arg, f, cr) : ENOTTY)

/* Statistics of the global vnode cache. */
struct vn_stats {
        uint32_t        vs_nvnodes;     /* shared vnodes in existence */
        uint32_t        vs_nfree;       /* on the free lists */
        uint64_t        vs_allocs;      /* vn_alloc calls */
        uint64_t        vs_built;       /* of them, new vnodes */
        uint64_t        vs_reclaims;    /* identities reclaimed */
        uint64_t        vs_reclaim_busy; /* reclaims a file system refused */
};

/* Shared vnodes are built rather than reused below this many. */
extern uint32_t vn_minvn;

void vn_sysinit(void);
struct vnode *vn_alloc(struct vfs *, enum vtype, const struct vnodeops *,
    void *);
void vn_init(struct vnode *, struct vfs *, enum vtype,
    const struct vnodeops *, void *);
void vn_destroy(struct vnode *);
void vn_hold(struct vnode *);
void vn_rele(struct vnode *);
void vn_rele_inactive(struct vnode *, int (*)(struct vnode *));
int vn_get(struct vnode *, uint64_t);
void vn_inactive_done(struct vnode *);
void vn_gone(struct vnode *);
void vn_flush_vfs(struct vfs *);
void vn_flush_aged(uint64_t);
void vn_purge_vfs(struct vfs *);
void vn_dirty_add(struct vnode *);
void vn_dirty_remove(struct vnode *);
void vn_get_stats(struct vn_stats *);

struct stat;
void vattr_to_stat(const struct vattr *, struct stat *);

#define VN_HOLD(vp)     vn_hold(vp)
#define VN_RELE(vp)     vn_rele(vp)

/* The system root directory; held for the life of the system. */
extern struct vnode *rootdir;

/*
 * File operations by path (vfs_vnops.c). Each returns 0 or an errno
 * value. A relative path starts at the directory given to the *at form
 * (the *at system calls pass a directory descriptor's vnode), or at the
 * current directory when that is NULL, as it is for the plain forms.
 */
int vn_openat(struct vnode *, const char *, int, mode_t, struct vnode **);
int vn_close(struct vnode *, int);
int vn_rdwr(enum uio_rw, struct vnode *, void *, size_t, off_t, int,
    size_t *);
int vn_create(const char *, mode_t, int, struct vnode **);
int vn_removeat(struct vnode *, const char *);
int vn_linkat(struct vnode *, const char *, struct vnode *, const char *,
    int);
int vn_renameat(struct vnode *, const char *, struct vnode *, const char *);
int vn_mkdirat(struct vnode *, const char *, mode_t);
int vn_rmdirat(struct vnode *, const char *);
int vn_symlinkat(const char *, struct vnode *, const char *);
int vn_readlinkat(struct vnode *, const char *, char *, size_t, size_t *);
int vn_statat(struct vnode *, const char *, int, struct vattr *);
int vn_truncate(const char *, off_t);
int vn_mknodat(struct vnode *, const char *, enum vtype, mode_t);
int vn_chown(struct vnode *, uid_t, gid_t);
int vn_chmod(struct vnode *, mode_t);
int vaccess(enum vtype, mode_t, uid_t, gid_t, int, struct cred *);

static inline int
vn_open(const char *path, int flags, mode_t mode, struct vnode **vpp)
{
        return vn_openat(NULL, path, flags, mode, vpp);
}

static inline int
vn_remove(const char *path)
{
        return vn_removeat(NULL, path);
}

static inline int
vn_link(const char *frompath, const char *topath)
{
        return vn_linkat(NULL, frompath, NULL, topath, NO_FOLLOW);
}

static inline int
vn_rename(const char *frompath, const char *topath)
{
        return vn_renameat(NULL, frompath, NULL, topath);
}

static inline int
vn_mkdir(const char *path, mode_t mode)
{
        return vn_mkdirat(NULL, path, mode);
}

static inline int
vn_rmdir(const char *path)
{
        return vn_rmdirat(NULL, path);
}

static inline int
vn_symlink(const char *target, const char *path)
{
        return vn_symlinkat(target, NULL, path);
}

static inline int
vn_readlink(const char *path, char *buf, size_t size, size_t *lenp)
{
        return vn_readlinkat(NULL, path, buf, size, lenp);
}

static inline int
vn_stat(const char *path, int follow, struct vattr *va)
{
        return vn_statat(NULL, path, follow, va);
}

static inline int
vn_mknod(const char *path, enum vtype type, mode_t mode)
{
        return vn_mknodat(NULL, path, type, mode);
}

/* VOP_POLL for objects that are always ready (sys_poll.c). */
int fs_poll(struct vnode *, short, short *, struct pollhead **, struct cred *);

/* fifofs: the FIFO node standing for a file system's VFIFO vnode. */
int fifo_vp(struct vnode *, struct vnode **);

#endif /* _SYS_VNODE_H_ */
