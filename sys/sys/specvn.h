/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Vnode operations shared by device vnodes (spec_vnops.c). A device
 * vnode's v_data points at a struct specattr, which holds its owner,
 * mode, and device number; spec_getattr, spec_setattr, and spec_access
 * use it. The others are what a node that is not a directory and has
 * no pages answers.
 */

#ifndef _SYS_SPECVN_H_
#define _SYS_SPECVN_H_

#include <sys/time.h>
#include <sys/types.h>
#include <sys/vnode.h>

struct specattr {
        mode_t          sa_mode;        /* permission bits */
        uid_t           sa_uid;
        gid_t           sa_gid;
        dev_t           sa_rdev;
        ino_t           sa_ino;
        struct timespec sa_time;        /* made */
};

void    specattr_init(struct specattr *, mode_t, uid_t, gid_t, dev_t);

int     spec_getattr(struct vnode *, struct vattr *, struct cred *);
int     spec_setattr(struct vnode *, struct vattr *, struct cred *);
int     spec_access(struct vnode *, int, struct cred *);
int     spec_lookup(struct vnode *, const char *, struct vnode **,
            struct cred *);
int     spec_create(struct vnode *, const char *, struct vattr *, int,
            struct vnode **, struct cred *);
int     spec_name1(struct vnode *, const char *, struct cred *);
int     spec_link(struct vnode *, struct vnode *, const char *,
            struct cred *);
int     spec_rename(struct vnode *, const char *, struct vnode *,
            const char *, struct cred *);
int     spec_mkdir(struct vnode *, const char *, struct vattr *,
            struct vnode **, struct cred *);
int     spec_readdir(struct vnode *, struct uio *, int *, struct cred *);
int     spec_symlink(struct vnode *, const char *, struct vattr *,
            const char *, struct cred *);
int     spec_readlink(struct vnode *, struct uio *, struct cred *);
int     spec_fsync(struct vnode *, struct cred *);
int     spec_putpage(struct vnode *, int, struct cred *);
int     spec_reclaim(struct vnode *, int);
int     spec_getpage(struct vnode *, off_t, struct page **, struct cred *);

#endif /* _SYS_SPECVN_H_ */
