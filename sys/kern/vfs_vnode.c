/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Vnode references, uiomove(), the kernel credential, and file times.
 * See <sys/vnode.h>.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

static struct cred kernel_cred;         /* uid 0, gid 0 */
struct cred *kcred = &kernel_cred;
struct vnode *rootdir;

void
vn_init(struct vnode *vp, struct vfs *vfsp, enum vtype type,
    const struct vnodeops *ops, void *data)
{
        spinlock_init(&vp->v_lock, "vnode");
        vp->v_count = 0;
        vp->v_flag = 0;
        vp->v_type = type;
        vp->v_vfsp = vfsp;
        vp->v_vfsmountedhere = NULL;
        vp->v_op = ops;
        vp->v_data = data;
        vp->v_rdev = 0;
}

void
vn_hold(struct vnode *vp)
{
        int s = spin_lock(&vp->v_lock);

        vp->v_count++;
        spin_unlock(&vp->v_lock, s);
}

/*
 * Drop a reference; the last one calls VOP_INACTIVE, which may sleep.
 * A file system that can hand the vnode out again re-checks v_count
 * under its own lock there.
 */
void
vn_rele(struct vnode *vp)
{
        int s = spin_lock(&vp->v_lock);

        if (vp->v_count == 0)
                panic("vn_rele: vnode %p released too often", (void *)vp);
        if (--vp->v_count != 0) {
                spin_unlock(&vp->v_lock, s);
                return;
        }
        vp->v_flag |= VINACT;
        spin_unlock(&vp->v_lock, s);
        VOP_INACTIVE(vp, kcred);
}

/* The file system kept the vnode after VOP_INACTIVE. */
void
vn_inactive_done(struct vnode *vp)
{
        int s = spin_lock(&vp->v_lock);

        vp->v_flag &= ~(uint32_t)VINACT;
        spin_unlock(&vp->v_lock, s);
}

int
uiomove(void *cp, size_t n, enum uio_rw rw, struct uio *uio)
{
        char *p = cp;

        if (uio->uio_segflg != UIO_SYSSPACE)
                return EFAULT;
        while (n > 0 && uio->uio_resid > 0) {
                struct iovec *iov = uio->uio_iov;
                size_t cnt;

                if (iov->iov_len == 0) {
                        uio->uio_iov++;
                        uio->uio_iovcnt--;
                        if (uio->uio_iovcnt <= 0)
                                break;
                        continue;
                }
                cnt = iov->iov_len < n ? iov->iov_len : n;
                if (rw == UIO_READ)
                        memcpy(iov->iov_base, p, cnt);
                else
                        memcpy(p, iov->iov_base, cnt);
                iov->iov_base = (char *)iov->iov_base + cnt;
                iov->iov_len -= cnt;
                uio->uio_resid -= (ssize_t)cnt;
                uio->uio_offset += (off_t)cnt;
                p += cnt;
                n -= cnt;
        }
        return 0;
}

void
gethrestime(struct timespec *ts)
{
        uint64_t t = lbolt;

        ts->tv_sec = (int64_t)(t / HZ);
        ts->tv_nsec = (long)((t % HZ) * (1000000000 / HZ));
}
