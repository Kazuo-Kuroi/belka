/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Pathname lookup, with the UnixWare rules:
 *
 * - An empty path is ENOENT. Repeated slashes are one; trailing slashes
 *   are ignored. Paths are at most MAXPATHLEN - 1 bytes and components
 *   MAXNAMELEN - 1, else ENAMETOOLONG.
 * - "/" alone is the root; asking for its parent is EINVAL, as is a
 *   last component that resolves to the parent itself ("." or ".." at
 *   the root).
 * - ".." at the system root stays there. ".." at the root of a mounted
 *   file system first moves to the directory it is mounted on.
 * - A component with a file system mounted on it is replaced by that
 *   file system's root.
 * - A symbolic link is followed if more path remains or the caller asked
 *   for it; its text replaces the component. An absolute link restarts
 *   at the root, and an empty one means ".". More than MAXSYMLINKS
 *   links is ELOOP.
 * - Every directory component must be a directory, else ENOTDIR.
 *
 * The name cache is consulted before VOP_LOOKUP and filled after it,
 * except for "." and "..".
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dnlc.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/pathname.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

static int lookup_component(struct vnode *, const char *, struct vnode **);
static int lookup_readlink(struct vnode *, char *, size_t *);

/*
 * Look name up in dvp, which the caller must be allowed to search; the
 * check comes before the name cache, which would otherwise bypass it.
 */
static int
lookup_component(struct vnode *dvp, const char *name, struct vnode **vpp)
{
        int dots = strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
        struct cred *cr = CRED();
        int error;

        if ((error = VOP_ACCESS(dvp, VEXEC, cr)) != 0)
                return error;
        if (!dots && (*vpp = dnlc_lookup(dvp, name)) != NULL)
                return 0;
        if ((error = VOP_LOOKUP(dvp, name, vpp, cr)) != 0)
                return error;
        if (!dots)
                dnlc_enter(dvp, name, *vpp);
        return 0;
}

/* Read link text into buf (MAXPATHLEN bytes); *lenp gets its length. */
static int
lookup_readlink(struct vnode *vp, char *buf, size_t *lenp)
{
        struct iovec iov;
        struct uio uio;
        int error;

        iov.iov_base = buf;
        iov.iov_len = MAXPATHLEN - 1;
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_offset = 0;
        uio.uio_resid = MAXPATHLEN - 1;
        uio.uio_segflg = UIO_SYSSPACE;
        uio.uio_limit = UIO_NOLIMIT;
        uio.uio_rw = UIO_READ;
        if ((error = VOP_READLINK(vp, &uio, CRED())) != 0)
                return error;
        *lenp = (size_t)(MAXPATHLEN - 1 - uio.uio_resid);
        buf[*lenp] = '\0';
        return 0;
}

int
lookuppn(const char *path, int follow, struct vnode *startdir,
    struct vnode **dvpp, char *lastname, struct vnode **vpp)
{
        char *pn = kmem_alloc(MAXPATHLEN, KM_SLEEP);
        char *link = kmem_alloc(MAXPATHLEN, KM_SLEEP);
        char comp[MAXNAMELEN];
        struct vnode *cur, *next;
        size_t len = strlen(path), pos = 0;
        int nlinks = 0, error = 0;

        if (dvpp != NULL)
                *dvpp = NULL;
        if (vpp != NULL)
                *vpp = NULL;
        if (len == 0) {
                error = ENOENT;
                goto out;
        }
        if (len >= MAXPATHLEN) {
                error = ENAMETOOLONG;
                goto out;
        }
        memcpy(pn, path, len + 1);

        /*
         * Relative names start at startdir, or the current directory,
         * which is read and held under p_lock: another LWP's chdir may
         * release the old one as soon as it is replaced.
         */
        if (pn[0] == '/') {
                cur = rootdir;
                VN_HOLD(cur);
        } else if (startdir != NULL) {
                cur = startdir;
                VN_HOLD(cur);
        } else {
                struct proc *p = curproc;

                mutex_lock(&p->p_lock);
                cur = p->p_cdir != NULL ? p->p_cdir : rootdir;
                VN_HOLD(cur);
                mutex_unlock(&p->p_lock);
        }

        for (;;) {
                size_t clen = 0;
                int last;

                while (pn[pos] == '/')
                        pos++;
                if (pn[pos] == '\0') {
                        /* Only slashes: the path names cur itself. */
                        if (dvpp != NULL) {
                                VN_RELE(cur);
                                error = EINVAL;
                        } else if (vpp != NULL) {
                                *vpp = cur;
                        } else {
                                VN_RELE(cur);
                        }
                        goto out;
                }
                while (pn[pos] != '/' && pn[pos] != '\0') {
                        if (clen == MAXNAMELEN - 1) {
                                VN_RELE(cur);
                                error = ENAMETOOLONG;
                                goto out;
                        }
                        comp[clen++] = pn[pos++];
                }
                comp[clen] = '\0';
                while (pn[pos] == '/')
                        pos++;
                last = pn[pos] == '\0';

                if (cur->v_type != VDIR) {
                        VN_RELE(cur);
                        error = ENOTDIR;
                        goto out;
                }

                if (strcmp(comp, "..") == 0) {
                        /* Up out of mounted file systems first. */
                        while (cur != rootdir && (cur->v_flag & VROOT) != 0 &&
                            cur->v_vfsp->vfs_vnodecovered != NULL) {
                                struct vnode *covered =
                                    cur->v_vfsp->vfs_vnodecovered;

                                VN_HOLD(covered);
                                VN_RELE(cur);
                                cur = covered;
                        }
                        if (cur == rootdir) {
                                next = cur;
                                VN_HOLD(next);
                                goto found;
                        }
                }

                error = lookup_component(cur, comp, &next);
                if (error == ENOENT && last && dvpp != NULL) {
                        *dvpp = cur;
                        memcpy(lastname, comp, clen + 1);
                        error = 0;
                        goto out;
                }
                if (error != 0) {
                        VN_RELE(cur);
                        goto out;
                }
                if ((error = vfs_traverse(&next)) != 0) {
                        VN_RELE(next);
                        VN_RELE(cur);
                        goto out;
                }
found:
                if (next->v_type == VLNK && (!last || follow)) {
                        size_t llen, rlen = strlen(pn + pos);

                        if (++nlinks > MAXSYMLINKS) {
                                error = ELOOP;
                        } else if ((error = lookup_readlink(next, link,
                            &llen)) == 0) {
                                if (llen == 0) {
                                        link[0] = '.';
                                        llen = 1;
                                }
                                if (llen + 1 + rlen >= MAXPATHLEN)
                                        error = ENAMETOOLONG;
                        }
                        VN_RELE(next);
                        if (error != 0) {
                                VN_RELE(cur);
                                goto out;
                        }
                        /* The rest of the path follows the link text. */
                        link[llen] = '/';
                        memcpy(link + llen + 1, pn + pos, rlen + 1);
                        memcpy(pn, link, llen + 1 + rlen + 1);
                        pos = 0;
                        if (pn[0] == '/') {
                                VN_RELE(cur);
                                cur = rootdir;
                                VN_HOLD(cur);
                        }
                        continue;
                }

                if (last) {
                        if (dvpp != NULL) {
                                if (next == cur) {
                                        VN_RELE(next);
                                        VN_RELE(cur);
                                        error = EINVAL;
                                        goto out;
                                }
                                *dvpp = cur;
                                memcpy(lastname, comp, clen + 1);
                        } else {
                                VN_RELE(cur);
                        }
                        if (vpp != NULL)
                                *vpp = next;
                        else
                                VN_RELE(next);
                        goto out;
                }
                VN_RELE(cur);
                cur = next;
        }
out:
        kmem_free(link, MAXPATHLEN);
        kmem_free(pn, MAXPATHLEN);
        return error;
}

int
lookupname(const char *path, int follow, struct vnode **vpp)
{
        return lookuppn(path, follow, NULL, NULL, NULL, vpp);
}
