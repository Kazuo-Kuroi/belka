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
 *   locks held and VINACT set. A file system that can be asked for the
 *   vnode again (by its own lookup) must re-check v_count under its own
 *   lock there; if it keeps the vnode it clears VINACT
 *   (vn_inactive_done). Unmount treats a vnode with VINACT as busy.
 * - The vnode belongs to the file system, which allocates and frees it
 *   with the node it stands for. IRIX's global vnode cache, with soft
 *   references from the name cache, comes with on-disk file systems.
 *
 * Operations return 0 or an errno value.
 */

#ifndef _SYS_VNODE_H_
#define _SYS_VNODE_H_

#include <stdint.h>

#include <sys/spinlock.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>

struct cred;
struct uio;
struct vfs;
struct vnode;

enum vtype { VNON, VREG, VDIR, VBLK, VCHR, VLNK, VFIFO, VSOCK };

/* v_flag */
#define VROOT           0x0001  /* root of its file system */
#define VINACT          0x0002  /* VOP_INACTIVE in progress; under v_lock */

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
};

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
        void    (*vop_inactive)(struct vnode *, struct cred *);
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

void vn_init(struct vnode *, struct vfs *, enum vtype,
    const struct vnodeops *, void *);
void vn_hold(struct vnode *);
void vn_rele(struct vnode *);
void vn_inactive_done(struct vnode *);

#define VN_HOLD(vp)     vn_hold(vp)
#define VN_RELE(vp)     vn_rele(vp)

/* The system root directory; held for the life of the system. */
extern struct vnode *rootdir;

/*
 * Kernel file operations by path (vfs_vnops.c), for the kernel itself
 * until there are system calls. Paths are absolute, or relative to the
 * root. Each returns 0 or an errno value.
 */
int vn_open(const char *, int, mode_t, struct vnode **);
int vn_close(struct vnode *, int);
int vn_rdwr(enum uio_rw, struct vnode *, void *, size_t, off_t, int,
    size_t *);
int vn_create(const char *, mode_t, int, struct vnode **);
int vn_remove(const char *);
int vn_link(const char *, const char *);
int vn_rename(const char *, const char *);
int vn_mkdir(const char *, mode_t);
int vn_rmdir(const char *);
int vn_symlink(const char *, const char *);
int vn_readlink(const char *, char *, size_t, size_t *);
int vn_stat(const char *, int, struct vattr *);
int vn_truncate(const char *, off_t);

#endif /* _SYS_VNODE_H_ */
