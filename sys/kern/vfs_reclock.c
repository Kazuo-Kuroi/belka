/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-15
 * SPDX-License-Identifier: MIT
 */

/*
 * Record locks (see <sys/flock.h>).
 *
 * Each vnode with locks has a list of them (v_locks), unordered, each a
 * range [start, end] of bytes (end RL_EOF for one that runs to the end
 * of the file and past it), a type, and an owning process. A process's
 * own ranges on a vnode never overlap: setting a lock first takes the
 * range out of what the process holds (splitting a lock that sticks out
 * on either side), then adds it, joined to the process's touching locks
 * of the same type.
 *
 * Everything is under one mutex, reclock_mutex, and a process that
 * waits (F_SETLKW) waits on reclock_sv, which is broadcast whenever a
 * lock goes or weakens; each waiter then looks again. A process that
 * waits records whose lock it waits for (p_lkwait, under the mutex), so
 * that a wait that would close a circle of waiting processes fails with
 * EDEADLK rather than sleeping for ever.
 *
 * Every lock of a process on a file goes when it closes any descriptor
 * for that file (fd_release in kern_descrip.c calls reclock_release); a
 * lock is set only while the caller holds a reference to the open file,
 * so a vnode with locks is always in use, and its list is empty by the
 * time it can be reused.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/flock.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#define RL_EOF          INT64_MAX       /* the last byte there could be */
#define RL_MAXCHAIN     64              /* waits looked through, EDEADLK */

struct reclock {
        struct reclock  *rl_next;
        int64_t         rl_start;
        int64_t         rl_end;         /* inclusive; RL_EOF */
        int             rl_type;        /* F_RDLCK or F_WRLCK */
        struct proc     *rl_owner;
        pid_t           rl_pid;
};

static struct mutex reclock_mutex;
static struct sv reclock_sv;

void
reclock_init(void)
{
        mutex_init(&reclock_mutex, "reclock");
        sv_init(&reclock_sv, "reclock");
}

static int
overlaps(const struct reclock *rl, int64_t start, int64_t end)
{
        return rl->rl_start <= end && start <= rl->rl_end;
}

/* Another process's lock on vp that a lock of type over the range meets. */
static struct reclock *
conflict(struct vnode *vp, struct proc *p, int type, int64_t start,
    int64_t end)
{
        for (struct reclock *rl = vp->v_locks; rl != NULL; rl = rl->rl_next)
                if (rl->rl_owner != p && overlaps(rl, start, end) &&
                    (type == F_WRLCK || rl->rl_type == F_WRLCK))
                        return rl;
        return NULL;
}

/*
 * Whether p waiting for owner would close a circle: owner waits for a
 * lock of a process that waits... for one of p's.
 */
static int
would_deadlock(struct proc *p, struct proc *owner)
{
        for (int n = 0; owner != NULL && n < RL_MAXCHAIN; n++) {
                if (owner == p)
                        return 1;
                owner = owner->p_lkwait;
        }
        return 0;
}

static struct reclock *
rl_new(struct proc *p, int type, int64_t start, int64_t end)
{
        struct reclock *rl = kmem_alloc(sizeof(*rl), KM_SLEEP);

        rl->rl_start = start;
        rl->rl_end = end;
        rl->rl_type = type;
        rl->rl_owner = p;
        rl->rl_pid = p->p_pid;
        return rl;
}

/*
 * Make p's locks on vp over [start, end] be of type (F_UNLCK: none),
 * leaving the rest of its locks as they were. Under reclock_mutex.
 * Returns whether any lock went or weakened, so that waiters look again.
 */
static int
apply(struct vnode *vp, struct proc *p, int type, int64_t start,
    int64_t end)
{
        struct reclock **rlp = &vp->v_locks, *rl;
        int weaker = 0;

        /* Take the range out of p's locks. */
        while ((rl = *rlp) != NULL) {
                if (rl->rl_owner != p || !overlaps(rl, start, end)) {
                        rlp = &rl->rl_next;
                        continue;
                }
                if (rl->rl_type == F_WRLCK && type != F_WRLCK)
                        weaker = 1;
                else if (type == F_UNLCK)
                        weaker = 1;
                if (rl->rl_start < start && rl->rl_end > end) {
                        /* Sticks out on both sides: split it. */
                        struct reclock *right = rl_new(p, rl->rl_type,
                            end + 1, rl->rl_end);

                        rl->rl_end = start - 1;
                        right->rl_next = rl->rl_next;
                        rl->rl_next = right;
                        rlp = &right->rl_next;
                } else if (rl->rl_start < start) {
                        rl->rl_end = start - 1;
                        rlp = &rl->rl_next;
                } else if (rl->rl_end > end) {
                        rl->rl_start = end + 1;
                        rlp = &rl->rl_next;
                } else {
                        *rlp = rl->rl_next;
                        kmem_free(rl, sizeof(*rl));
                }
        }
        if (type == F_UNLCK)
                return weaker;

        /* Join p's touching locks of the same type to the new one. */
        for (rlp = &vp->v_locks; (rl = *rlp) != NULL;) {
                if (rl->rl_owner == p && rl->rl_type == type &&
                    ((start > 0 && rl->rl_end == start - 1) ||
                    (end != RL_EOF && rl->rl_start == end + 1))) {
                        if (rl->rl_start < start)
                                start = rl->rl_start;
                        if (rl->rl_end > end)
                                end = rl->rl_end;
                        *rlp = rl->rl_next;
                        kmem_free(rl, sizeof(*rl));
                } else {
                        rlp = &rl->rl_next;
                }
        }
        rl = rl_new(p, type, start, end);
        rl->rl_next = vp->v_locks;
        vp->v_locks = rl;
        return weaker;
}

/* The range fl names in fp's file: [*startp, *endp], or an error. */
static int
range(struct file *fp, const struct flock *fl, int64_t *startp,
    int64_t *endp)
{
        struct vattr va;
        int64_t base, start, end;
        int error;

        switch (fl->l_whence) {
        case SEEK_SET:
                base = 0;
                break;
        case SEEK_CUR:
                mutex_lock(&fp->f_offlock);
                base = fp->f_offset;
                mutex_unlock(&fp->f_offlock);
                break;
        case SEEK_END:
                va.va_mask = AT_SIZE;
                if ((error = VOP_GETATTR(fp->f_vnode, &va, fp->f_cred)) != 0)
                        return error;
                base = va.va_size;
                break;
        default:
                return EINVAL;
        }
        if (__builtin_add_overflow(base, fl->l_start, &start))
                return EOVERFLOW;
        if (fl->l_len > 0) {
                if (__builtin_add_overflow(start, fl->l_len - 1, &end))
                        return EOVERFLOW;
        } else if (fl->l_len == 0) {
                end = RL_EOF;
        } else {
                /* The l_len bytes before start (POSIX 2008). */
                end = start - 1;
                if (__builtin_add_overflow(start, fl->l_len, &start))
                        return EINVAL;
        }
        if (start < 0 || end < start)
                return EINVAL;
        *startp = start;
        *endp = end;
        return 0;
}

/*
 * fcntl(fd, cmd, &fl) for F_GETLK, F_SETLK and F_SETLKW on the open file
 * fp, by the calling process; fl is the caller's copy, which F_GETLK
 * rewrites.
 */
int
reclock_fcntl(struct file *fp, int cmd, struct flock *fl)
{
        struct vnode *vp = fp->f_vnode;
        struct proc *p = curproc;
        struct reclock *rl;
        int64_t start, end;
        int error, r;

        if (fl->l_type != F_RDLCK && fl->l_type != F_WRLCK &&
            fl->l_type != F_UNLCK)
                return EINVAL;
        if ((error = range(fp, fl, &start, &end)) != 0)
                return error;

        if (cmd == F_GETLK) {
                if (fl->l_type == F_UNLCK)
                        return EINVAL;
                mutex_lock(&reclock_mutex);
                if ((rl = conflict(vp, p, fl->l_type, start, end)) == NULL) {
                        fl->l_type = F_UNLCK;
                } else {
                        fl->l_type = (int16_t)rl->rl_type;
                        fl->l_whence = SEEK_SET;
                        fl->l_start = rl->rl_start;
                        fl->l_len = rl->rl_end == RL_EOF ? 0 :
                            rl->rl_end - rl->rl_start + 1;
                        fl->l_pid = rl->rl_pid;
                }
                mutex_unlock(&reclock_mutex);
                return 0;
        }

        /* A read lock needs the file open for reading; a write one, writing. */
        if ((fl->l_type == F_RDLCK && (fp->f_flag & FREAD) == 0) ||
            (fl->l_type == F_WRLCK && (fp->f_flag & FWRITE) == 0))
                return EBADF;

        mutex_lock(&reclock_mutex);
        if (fl->l_type != F_UNLCK) {
                while ((rl = conflict(vp, p, fl->l_type, start, end)) !=
                    NULL) {
                        if (cmd != F_SETLKW) {
                                error = EAGAIN;
                                break;
                        }
                        if (would_deadlock(p, rl->rl_owner)) {
                                error = EDEADLK;
                                break;
                        }
                        p->p_lkwait = rl->rl_owner;
                        if (sv_wait_mutex_sig(&reclock_sv,
                            &reclock_mutex) != 0 &&
                            (r = sig_sleep_check()) != 0)
                                error = r == 1 ? EINTR : ERESTART;
                        mutex_lock(&reclock_mutex);
                        p->p_lkwait = NULL;
                        if (error != 0)
                                break;
                }
        }
        if (error == 0 && apply(vp, p, fl->l_type, start, end))
                sv_broadcast(&reclock_sv);
        mutex_unlock(&reclock_mutex);
        return error;
}

/* p closed a descriptor for vp: all its locks on vp go. */
void
reclock_release(struct vnode *vp, struct proc *p)
{
        struct reclock **rlp, *rl;
        int any = 0;

        /*
         * An unlocked look first: p's locks are set only by p's LWPs,
         * and one that set a lock through a descriptor this close
         * raced with looks at the descriptor again afterwards
         * (sys_fcntl), and drops the locks if it has gone.
         */
        if (vp->v_locks == NULL)
                return;
        mutex_lock(&reclock_mutex);
        for (rlp = &vp->v_locks; (rl = *rlp) != NULL;) {
                if (rl->rl_owner == p) {
                        *rlp = rl->rl_next;
                        kmem_free(rl, sizeof(*rl));
                        any = 1;
                } else {
                        rlp = &rl->rl_next;
                }
        }
        if (any)
                sv_broadcast(&reclock_sv);
        mutex_unlock(&reclock_mutex);
}
