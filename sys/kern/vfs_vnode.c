/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Vnodes: references, the global vnode cache, the dirty vnode list,
 * uiomove(), the kernel credential, and file times. See <sys/vnode.h>.
 *
 * Shared vnodes come from a zone and are never freed. A vnode no one
 * references sits on one of VN_NFREE free lists: at the head if it has
 * no identity left, at the tail, least recently freed nearest the head,
 * if it is still cached. vn_rele() puts it on the shorter of two lists
 * chosen by a rotor, so the lists stay balanced without a global lock.
 *
 * vn_alloc() builds a new vnode while there are fewer than vn_minvn, or
 * fewer free vnodes than vnodes in use (the effective IRIX policy), and
 * otherwise tries up to VN_ALLOC_TRIES free ones. It skips any with
 * dirty pages and any whose file system will not give it up without
 * waiting (VOP_RECLAIM with VRC_NOWAIT), so it can be called with a
 * file system lock held.
 *
 * Vnodes with dirty pages are also on the dirty list, oldest first,
 * for vn_flush_aged() (from bdflush) and vn_flush_vfs().
 *
 * Lock order: dnlc_lock, then vn_dirty_lock, then a free-list lock,
 * then v_lock. vn_all_lock is taken alone. A vnode's pc_lock is taken
 * before vn_dirty_lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/spinlock.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include <vm/pvn.h>

#define VN_NFREE        4
#define VN_ALLOC_TRIES  4

struct vfreelist {
        struct spinlock vf_lock;
        struct vnode    *vf_head;
        struct vnode    *vf_tail;
        volatile uint32_t vf_len;
};

static struct cred kernel_cred;         /* uid 0, gid 0 */
struct cred *kcred = &kernel_cred;
struct vnode *rootdir;

uint32_t vn_minvn = 512;

static struct kmem_zone *vn_zone;
static struct vfreelist vn_free[VN_NFREE];
static volatile uint32_t vn_rotor;
static volatile uint64_t vn_numbers;
static struct spinlock vn_all_lock = SPINLOCK_INITIALIZER("vn_all");
static struct vnode *vn_all;
static struct spinlock vn_dirty_lock = SPINLOCK_INITIALIZER("vn_dirty");
static struct vnode *vn_dirty_head, *vn_dirty_tail;
static volatile uint32_t vn_flushgens;
static struct vn_stats vn_st;

static uint64_t vn_newnumber(void);
static void vf_insert(struct vfreelist *, struct vnode *, int);
static void vf_unlink(struct vfreelist *, struct vnode *);
static struct vfreelist *vf_pick(void);
static void vn_put_free(struct vnode *, int);
static void vn_take_ref(struct vnode *);
static void vn_clear_identity(struct vnode *);
static int vn_reclaim(struct vnode *, int);
static struct vnode *vn_reuse(void);
static int vn_flush(struct vfs *, uint64_t);

void
vn_sysinit(void)
{
        vn_zone = kmem_zone_init(sizeof(struct vnode), "vnode");
        for (int i = 0; i < VN_NFREE; i++)
                spinlock_init(&vn_free[i].vf_lock, "vn_free");
}

static uint64_t
vn_newnumber(void)
{
        return atomic_add_64_nv(&vn_numbers, 1);
}

/* The list's lock and the vnode's v_lock are held. */
static void
vf_insert(struct vfreelist *fl, struct vnode *vp, int head)
{
        if (head) {
                vp->v_fprev = NULL;
                vp->v_fnext = fl->vf_head;
                if (fl->vf_head != NULL)
                        fl->vf_head->v_fprev = vp;
                else
                        fl->vf_tail = vp;
                fl->vf_head = vp;
        } else {
                vp->v_fnext = NULL;
                vp->v_fprev = fl->vf_tail;
                if (fl->vf_tail != NULL)
                        fl->vf_tail->v_fnext = vp;
                else
                        fl->vf_head = vp;
                fl->vf_tail = vp;
        }
        vp->v_listid = (int)(fl - vn_free);
        vp->v_flag |= VFREE;
        fl->vf_len++;
        (void)atomic_inc_32_nv(&vn_st.vs_nfree);
}

static void
vf_unlink(struct vfreelist *fl, struct vnode *vp)
{
        if (vp->v_fprev != NULL)
                vp->v_fprev->v_fnext = vp->v_fnext;
        else
                fl->vf_head = vp->v_fnext;
        if (vp->v_fnext != NULL)
                vp->v_fnext->v_fprev = vp->v_fprev;
        else
                fl->vf_tail = vp->v_fprev;
        vp->v_fnext = vp->v_fprev = NULL;
        vp->v_flag &= ~(uint32_t)VFREE;
        fl->vf_len--;
        (void)atomic_dec_32_nv(&vn_st.vs_nfree);
}

/* The shorter of two neighbouring lists, chosen by the rotor. */
static struct vfreelist *
vf_pick(void)
{
        uint32_t r = atomic_inc_32_nv(&vn_rotor);
        struct vfreelist *a = &vn_free[r % VN_NFREE];
        struct vfreelist *b = &vn_free[(r + 1) % VN_NFREE];

        return b->vf_len < a->vf_len ? b : a;
}

/*
 * Put an unreferenced vnode that is off every list on a free list, at
 * the head if it has no identity. Clears VRECLM. If it was referenced
 * meanwhile, it stays off the lists.
 */
static void
vn_put_free(struct vnode *vp, int head)
{
        struct vfreelist *fl = vf_pick();
        int s = spin_lock(&fl->vf_lock);

        (void)spin_lock(&vp->v_lock);
        vp->v_flag &= ~(uint32_t)VRECLM;
        if (vp->v_count == 0)
                vf_insert(fl, vp, head);
        spin_unlock(&vp->v_lock, 0);
        spin_unlock(&fl->vf_lock, s);
}

/*
 * Take a reference; a vnode on a free list comes off it. The caller
 * holds no free-list lock or v_lock.
 */
static void
vn_take_ref(struct vnode *vp)
{
        for (;;) {
                struct vfreelist *fl;
                int s = spin_lock(&vp->v_lock);

                if ((vp->v_flag & VFREE) == 0) {
                        vp->v_count++;
                        spin_unlock(&vp->v_lock, s);
                        return;
                }
                fl = &vn_free[vp->v_listid];
                spin_unlock(&vp->v_lock, 0);
                (void)spin_lock(&fl->vf_lock);
                (void)spin_lock(&vp->v_lock);
                if ((vp->v_flag & VFREE) != 0 &&
                    &vn_free[vp->v_listid] == fl) {
                        vf_unlink(fl, vp);
                        vp->v_count++;
                        spin_unlock(&vp->v_lock, 0);
                        spin_unlock(&fl->vf_lock, s);
                        return;
                }
                spin_unlock(&vp->v_lock, 0);
                spin_unlock(&fl->vf_lock, s);
        }
}

void
vn_hold(struct vnode *vp)
{
        vn_take_ref(vp);
}

/*
 * Turn a soft reference (vp and the v_number seen) into a real one.
 * Returns 0 if vp is no longer that vnode, or is being inactivated or
 * reclaimed. The caller guarantees that vp is still a vnode: shared
 * vnodes always are, and a file system purges the name cache before
 * freeing a private one.
 */
int
vn_get(struct vnode *vp, uint64_t number)
{
        int s = spin_lock(&vp->v_lock);

        if (vp->v_number != number ||
            (vp->v_flag & (VINACT | VRECLM)) != 0) {
                spin_unlock(&vp->v_lock, s);
                return 0;
        }
        if ((vp->v_flag & VFREE) == 0) {
                vp->v_count++;
                spin_unlock(&vp->v_lock, s);
                return 1;
        }
        for (;;) {
                struct vfreelist *fl = &vn_free[vp->v_listid];

                spin_unlock(&vp->v_lock, 0);
                (void)spin_lock(&fl->vf_lock);
                (void)spin_lock(&vp->v_lock);
                if (vp->v_number != number ||
                    (vp->v_flag & (VINACT | VRECLM)) != 0) {
                        spin_unlock(&vp->v_lock, 0);
                        spin_unlock(&fl->vf_lock, s);
                        return 0;
                }
                if ((vp->v_flag & VFREE) == 0 ||
                    &vn_free[vp->v_listid] == fl) {
                        if ((vp->v_flag & VFREE) != 0)
                                vf_unlink(fl, vp);
                        vp->v_count++;
                        spin_unlock(&vp->v_lock, 0);
                        spin_unlock(&fl->vf_lock, s);
                        return 1;
                }
                spin_unlock(&fl->vf_lock, 0);
        }
}

/* A private vnode, embedded in its file system's node. */
void
vn_init(struct vnode *vp, struct vfs *vfsp, enum vtype type,
    const struct vnodeops *ops, void *data)
{
        memset(vp, 0, sizeof(*vp));
        spinlock_init(&vp->v_lock, "vnode");
        vp->v_type = type;
        vp->v_vfsp = vfsp;
        vp->v_op = ops;
        vp->v_data = data;
        vp->v_number = vn_newnumber();
        pvn_init(vp, PC_KEEP);
}

/* Free a private vnode's pages, before its file system frees it. */
void
vn_destroy(struct vnode *vp)
{
        if ((vp->v_flag & VSHARE) != 0)
                panic("vn_destroy: vnode %p is shared", (void *)vp);
        pvn_destroy(vp);
}

/*
 * A shared vnode for a file system node, referenced once. Reuses a free
 * vnode when the policy allows and one can be reclaimed; may sleep.
 */
struct vnode *
vn_alloc(struct vfs *vfsp, enum vtype type, const struct vnodeops *ops,
    void *data)
{
        uint32_t nv = vn_st.vs_nvnodes, nf = vn_st.vs_nfree;
        struct vnode *vp = NULL;
        int s;

        atomic_add_64(&vn_st.vs_allocs, 1);
        if (nv >= vn_minvn && nf >= nv - nf)
                vp = vn_reuse();
        if (vp == NULL) {
                vp = kmem_zone_alloc(vn_zone, KM_SLEEP);
                memset(vp, 0, sizeof(*vp));
                spinlock_init(&vp->v_lock, "vnode");
                pvn_init(vp, 0);
                s = spin_lock(&vn_all_lock);
                vp->v_allnext = vn_all;
                vn_all = vp;
                spin_unlock(&vn_all_lock, s);
                (void)atomic_inc_32_nv(&vn_st.vs_nvnodes);
                atomic_add_64(&vn_st.vs_built, 1);
        }
        s = spin_lock(&vp->v_lock);
        vp->v_count = 1;
        vp->v_flag = VSHARE;
        vp->v_type = type;
        vp->v_vfsp = vfsp;
        vp->v_vfsmountedhere = NULL;
        vp->v_op = ops;
        vp->v_data = data;
        vp->v_rdev = 0;
        vp->v_number = vn_newnumber();
        spin_unlock(&vp->v_lock, s);
        return vp;
}

/*
 * Take free vnodes from list heads until one can be reclaimed. Returns
 * it with VRECLM set and no identity, or NULL.
 */
static struct vnode *
vn_reuse(void)
{
        uint32_t r = vn_rotor;

        for (int t = 0; t < VN_ALLOC_TRIES; t++) {
                struct vfreelist *fl = &vn_free[(r + (uint32_t)t) % VN_NFREE];
                struct vnode *vp;
                int s = spin_lock(&fl->vf_lock);

                if ((vp = fl->vf_head) == NULL) {
                        spin_unlock(&fl->vf_lock, s);
                        continue;
                }
                (void)spin_lock(&vp->v_lock);
                vf_unlink(fl, vp);
                if (vp->v_pc.pc_ndirty != 0) {
                        /* Leave it for the flusher; try another. */
                        vf_insert(fl, vp, 0);
                        spin_unlock(&vp->v_lock, 0);
                        spin_unlock(&fl->vf_lock, s);
                        continue;
                }
                vp->v_flag |= VRECLM;
                spin_unlock(&vp->v_lock, 0);
                spin_unlock(&fl->vf_lock, s);
                if (vn_reclaim(vp, VRC_NOWAIT) == 0)
                        return vp;
        }
        return NULL;
}

/* Forget everything a reclaimed or torn-down vnode stood for. */
static void
vn_clear_identity(struct vnode *vp)
{
        int s;

        pvn_destroy(vp);
        s = spin_lock(&vp->v_lock);
        vp->v_op = NULL;
        vp->v_data = NULL;
        vp->v_vfsp = NULL;
        vp->v_vfsmountedhere = NULL;
        vp->v_number = vn_newnumber();
        vp->v_flag &= VRECLM | VSHARE;
        spin_unlock(&vp->v_lock, s);
}

/*
 * Reclaim a free vnode that the caller took off its list with VRECLM
 * set. Returns 0 with the identity gone and VRECLM still set, or EBUSY
 * if the file system refused, in which case the vnode is back on a free
 * list (if still unreferenced).
 */
static int
vn_reclaim(struct vnode *vp, int flags)
{
        if (vp->v_op != NULL && VOP_RECLAIM(vp, flags) != 0) {
                atomic_add_64(&vn_st.vs_reclaim_busy, 1);
                vn_put_free(vp, 0);
                return EBUSY;
        }
        if (vp->v_op != NULL)
                atomic_add_64(&vn_st.vs_reclaims, 1);
        vn_clear_identity(vp);
        return 0;
}

void
vn_rele(struct vnode *vp)
{
        vn_rele_inactive(vp, NULL);
}

/*
 * Drop a reference. The last one inactivates the vnode, with inact()
 * if given (for a file system that already holds the lock its
 * VOP_INACTIVE would take), else VOP_INACTIVE; either may sleep.
 *
 * For a shared vnode, a reference taken during VOP_INACTIVE keeps the
 * vnode off the free lists, and a release of that reference during it
 * (VREINACT) makes it run again, since the file may have changed.
 */
void
vn_rele_inactive(struct vnode *vp, int (*inact)(struct vnode *))
{
        int s = spin_lock(&vp->v_lock);

        if (vp->v_count == 0)
                panic("vn_rele: vnode %p released too often", (void *)vp);
        if (--vp->v_count != 0) {
                spin_unlock(&vp->v_lock, s);
                return;
        }
        if ((vp->v_flag & VSHARE) == 0) {
                vp->v_flag |= VINACT;
                spin_unlock(&vp->v_lock, s);
                if (inact != NULL)
                        (void)inact(vp);
                else
                        (void)VOP_INACTIVE(vp, kcred);
                return;
        }
        if ((vp->v_flag & VINACT) != 0) {
                vp->v_flag |= VREINACT;
                spin_unlock(&vp->v_lock, s);
                return;
        }
        vp->v_flag |= VINACT;
        spin_unlock(&vp->v_lock, s);

        for (;;) {
                int r = inact != NULL ? inact(vp) : VOP_INACTIVE(vp, kcred);
                struct vfreelist *fl;

                if (r == VN_INACTIVE_NOCACHE) {
                        /* Nothing can find it now; nobody else holds it. */
                        vn_clear_identity(vp);
                        s = spin_lock(&vp->v_lock);
                        vp->v_flag &= ~(uint32_t)(VINACT | VREINACT);
                        spin_unlock(&vp->v_lock, s);
                        vn_put_free(vp, 1);
                        return;
                }
                fl = vf_pick();
                s = spin_lock(&fl->vf_lock);
                (void)spin_lock(&vp->v_lock);
                if (vp->v_count != 0) {
                        vp->v_flag &= ~(uint32_t)(VINACT | VREINACT);
                        spin_unlock(&vp->v_lock, 0);
                        spin_unlock(&fl->vf_lock, s);
                        return;
                }
                if ((vp->v_flag & VREINACT) != 0) {
                        vp->v_flag &= ~(uint32_t)VREINACT;
                        spin_unlock(&vp->v_lock, 0);
                        spin_unlock(&fl->vf_lock, s);
                        continue;
                }
                vp->v_flag &= ~(uint32_t)VINACT;
                vf_insert(fl, vp, 0);
                spin_unlock(&vp->v_lock, 0);
                spin_unlock(&fl->vf_lock, s);
                return;
        }
}

/* A private vnode's file system kept it after VOP_INACTIVE. */
void
vn_inactive_done(struct vnode *vp)
{
        int s = spin_lock(&vp->v_lock);

        vp->v_flag &= ~(uint32_t)VINACT;
        spin_unlock(&vp->v_lock, s);
}

/*
 * The file system has torn down the node of a shared vnode, of which the
 * caller holds the last reference (unmount, for the root).
 */
void
vn_gone(struct vnode *vp)
{
        int s = spin_lock(&vp->v_lock);

        if (vp->v_count != 1 || (vp->v_flag & VSHARE) == 0)
                panic("vn_gone: vnode %p is not a shared vnode held once",
                    (void *)vp);
        vp->v_count = 0;
        vp->v_flag |= VRECLM;
        spin_unlock(&vp->v_lock, s);
        vn_clear_identity(vp);
        vn_put_free(vp, 1);
}

/*
 * Reclaim every free vnode of vfsp, waiting for its file system's locks
 * (unmount). Vnodes still referenced are left alone.
 */
void
vn_purge_vfs(struct vfs *vfsp)
{
        for (;;) {
                struct vnode *vp;
                struct vfreelist *fl;
                int s, got = 0;

                s = spin_lock(&vn_all_lock);
                for (vp = vn_all; vp != NULL; vp = vp->v_allnext) {
                        if (vp->v_vfsp == vfsp && (vp->v_flag & VFREE) != 0)
                                break;
                }
                spin_unlock(&vn_all_lock, s);
                if (vp == NULL)
                        return;

                fl = &vn_free[vp->v_listid];
                s = spin_lock(&fl->vf_lock);
                (void)spin_lock(&vp->v_lock);
                if ((vp->v_flag & VFREE) != 0 &&
                    &vn_free[vp->v_listid] == fl && vp->v_vfsp == vfsp) {
                        vf_unlink(fl, vp);
                        vp->v_flag |= VRECLM;
                        got = 1;
                }
                spin_unlock(&vp->v_lock, 0);
                spin_unlock(&fl->vf_lock, s);
                if (got && vn_reclaim(vp, 0) == 0)
                        vn_put_free(vp, 1);
        }
}

/* vp's first page became dirty. Its pc_lock is held. */
void
vn_dirty_add(struct vnode *vp)
{
        int s = spin_lock(&vn_dirty_lock);

        if (!vp->v_ondirty) {
                vp->v_dnext = NULL;
                vp->v_dprev = vn_dirty_tail;
                if (vn_dirty_tail != NULL)
                        vn_dirty_tail->v_dnext = vp;
                else
                        vn_dirty_head = vp;
                vn_dirty_tail = vp;
                vp->v_ondirty = 1;
        }
        spin_unlock(&vn_dirty_lock, s);
}

/* vp has no dirty pages left. Its pc_lock is held. */
void
vn_dirty_remove(struct vnode *vp)
{
        int s = spin_lock(&vn_dirty_lock);

        if (vp->v_ondirty) {
                if (vp->v_dprev != NULL)
                        vp->v_dprev->v_dnext = vp->v_dnext;
                else
                        vn_dirty_head = vp->v_dnext;
                if (vp->v_dnext != NULL)
                        vp->v_dnext->v_dprev = vp->v_dprev;
                else
                        vn_dirty_tail = vp->v_dprev;
                vp->v_dnext = vp->v_dprev = NULL;
                vp->v_ondirty = 0;
        }
        spin_unlock(&vn_dirty_lock, s);
}

/*
 * One pass over the dirty list: write back each vnode of vfsp (or any,
 * if NULL) that has been dirty for at least age ticks. Returns how many
 * were skipped because they were being inactivated or reclaimed.
 */
static int
vn_flush(struct vfs *vfsp, uint64_t age)
{
        uint32_t gen = atomic_inc_32_nv(&vn_flushgens);
        int skipped = 0;

        for (;;) {
                struct vnode *vp;
                int s = spin_lock(&vn_dirty_lock);

                for (vp = vn_dirty_head; vp != NULL; vp = vp->v_dnext) {
                        if (vp->v_flushgen == gen ||
                            (vfsp != NULL && vp->v_vfsp != vfsp))
                                continue;
                        if (age != 0 && lbolt - vp->v_pc.pc_dirtytime < age)
                                continue;
                        vp->v_flushgen = gen;
                        if (vn_get(vp, vp->v_number))
                                break;
                        skipped++;
                }
                spin_unlock(&vn_dirty_lock, s);
                if (vp == NULL)
                        return skipped;
                (void)VOP_PUTPAGE(vp, 0, kcred);
                VN_RELE(vp);
        }
}

/* Write back every dirty page of vfsp (sync, unmount). */
void
vn_flush_vfs(struct vfs *vfsp)
{
        for (int tries = 0; tries < 1000; tries++) {
                if (vn_flush(vfsp, 0) == 0)
                        return;
                delay(1);
        }
        printf("vn_flush_vfs: gave up on busy vnodes\n");
}

/* Write back vnodes dirty for at least age ticks (bdflush). */
void
vn_flush_aged(uint64_t age)
{
        (void)vn_flush(NULL, age == 0 ? 1 : age);
}

void
vn_get_stats(struct vn_stats *st)
{
        *st = vn_st;
}

int
uiomove(void *cp, size_t n, enum uio_rw rw, struct uio *uio)
{
        char *p = cp;

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
                if (uio->uio_segflg == UIO_USERSPACE) {
                        int error = rw == UIO_READ ?
                            copyout(p, iov->iov_base, cnt) :
                            copyin(iov->iov_base, p, cnt);

                        if (error != 0)
                                return error;
                } else if (rw == UIO_READ) {
                        memcpy(iov->iov_base, p, cnt);
                } else {
                        memcpy(p, iov->iov_base, cnt);
                }
                iov->iov_base = (char *)iov->iov_base + cnt;
                iov->iov_len -= cnt;
                uio->uio_resid -= (ssize_t)cnt;
                uio->uio_offset += (off_t)cnt;
                p += cnt;
                n -= cnt;
        }
        return 0;
}

int64_t boottime_sec;

void
gethrestime(struct timespec *ts)
{
        uint64_t t = lbolt;

        ts->tv_sec = boottime_sec + (int64_t)(t / HZ);
        ts->tv_nsec = (long)((t % HZ) * (1000000000 / HZ));
}
