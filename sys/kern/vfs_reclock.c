/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-15
 * SPDX-License-Identifier: MIT
 */

/*
 * Locks on files (see <sys/flock.h>): fcntl's record locks, owned by a
 * process, and flock's whole-file locks, owned by an open file.
 *
 * Each vnode with locks has a list of them (v_locks), unordered, each a
 * range [start, end] of bytes (end RL_EOF for one that runs to the end
 * of the file and past it), a type, and an owner: the process (struct
 * proc) for a record lock, the open file (struct file) for a flock one.
 * The two kinds meet in one list and conflict with each other as any two
 * owners' locks do. An owner's own ranges on a vnode never overlap:
 * setting a lock first takes the range out of what the owner holds
 * (splitting a lock that sticks out on either side), then adds it,
 * joined to the owner's touching locks of the same type.
 *
 * Everything is under one mutex, reclock_mutex. A thread that waits
 * waits on reclock_sv, which is broadcast whenever a lock goes or
 * weakens; each waiter then looks again. A thread waiting for a record
 * lock puts a struct rlwait on the list of waiters, naming the owner it
 * acts for and the owner it waits for, so that a wait that would close
 * a circle of waiting owners fails with EDEADLK rather than sleeping for
 * ever. flock's waits are not looked at, as in BSD.
 *
 * There are at most reclock_max locks in all; a request that would need
 * more fails with ENOLCK.
 *
 * A process's record locks on a file go when it closes any descriptor
 * for it (fd_release in kern_descrip.c); an open file's flock lock goes
 * when the file is last closed (frele). A lock is set only while the
 * caller holds a reference to the open file, so a vnode with locks is
 * always in use, and its list is empty by the time it can be reused.
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
#define RL_MAXDEPTH     16              /* waits followed, for EDEADLK */

struct reclock {
        struct reclock  *rl_next;
        int64_t         rl_start;
        int64_t         rl_end;         /* inclusive; RL_EOF */
        int             rl_type;        /* F_RDLCK or F_WRLCK */
        const void      *rl_owner;      /* struct proc or struct file */
        pid_t           rl_pid;         /* F_GETLK's l_pid; -1 for flock */
};

/* A thread waiting for a record lock: on reclock_waiters, its stack. */
struct rlwait {
        struct rlwait   *rw_next;
        const void      *rw_self;       /* the owner it acts for */
        const void      *rw_for;        /* the owner it waits for */
};

static struct mutex reclock_mutex;
static struct sv reclock_sv;
static struct rlwait *reclock_waiters;
static unsigned int reclock_count;
unsigned int reclock_max = RECLOCK_MAX;

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

/* Another owner's lock on vp that a lock of type over the range meets. */
static struct reclock *
conflict(struct vnode *vp, const void *owner, int type, int64_t start,
    int64_t end)
{
        for (struct reclock *rl = vp->v_locks; rl != NULL; rl = rl->rl_next)
                if (rl->rl_owner != owner && overlaps(rl, start, end) &&
                    (type == F_WRLCK || rl->rl_type == F_WRLCK))
                        return rl;
        return NULL;
}

/*
 * Whether self waiting for other would close a circle: some thread
 * acting for other waits for an owner that... waits for self. The
 * search goes RL_MAXDEPTH waits deep.
 */
static int
would_deadlock(const void *self, const void *other, int depth)
{
        if (other == self)
                return 1;
        if (depth >= RL_MAXDEPTH)
                return 0;
        for (struct rlwait *rw = reclock_waiters; rw != NULL;
            rw = rw->rw_next)
                if (rw->rw_self == other &&
                    would_deadlock(self, rw->rw_for, depth + 1))
                        return 1;
        return 0;
}

/* Up to n new records, counted against reclock_max, or ENOLCK. */
static int
rl_reserve(struct reclock **spare, int n)
{
        if (reclock_count + (unsigned int)n > reclock_max)
                return ENOLCK;
        for (int i = 0; i < n; i++)
                spare[i] = kmem_alloc(sizeof(struct reclock), KM_SLEEP);
        reclock_count += (unsigned int)n;
        return 0;
}

static void
rl_free(struct reclock *rl)
{
        kmem_free(rl, sizeof(*rl));
        reclock_count--;
}

/* One of the reserved records, filled in. */
static struct reclock *
rl_take(struct reclock **spare, const void *owner, pid_t pid, int type,
    int64_t start, int64_t end)
{
        struct reclock *rl;

        if (spare[0] != NULL) {
                rl = spare[0];
                spare[0] = NULL;
        } else {
                rl = spare[1];
                spare[1] = NULL;
        }
        rl->rl_start = start;
        rl->rl_end = end;
        rl->rl_type = type;
        rl->rl_owner = owner;
        rl->rl_pid = pid;
        return rl;
}

/*
 * Make owner's locks on vp over [start, end] be of type (F_UNLCK: none),
 * leaving the rest of its locks as they were. Under reclock_mutex. It
 * needs at most two new records, reserved first: one for the lock, one
 * for the right part of a lock it splits (the owner's locks do not
 * overlap, so there is at most one), and an unlock that splits nothing
 * needs none. Returns 0 or ENOLCK, and in *weakerp whether any lock went
 * or weakened, so that waiters look again.
 */
static int
apply(struct vnode *vp, const void *owner, pid_t pid, int type,
    int64_t start, int64_t end, int *weakerp)
{
        struct reclock **rlp, *rl, *spare[2] = { NULL, NULL };
        int error, need = type != F_UNLCK;

        /* A new lock needs a record; so does splitting one of the owner's. */
        *weakerp = 0;
        for (rl = vp->v_locks; rl != NULL; rl = rl->rl_next)
                if (rl->rl_owner == owner && rl->rl_start < start &&
                    rl->rl_end > end)
                        need++;
        if ((error = rl_reserve(spare, need)) != 0)
                return error;

        /* Take the range out of the owner's locks. */
        for (rlp = &vp->v_locks; (rl = *rlp) != NULL;) {
                if (rl->rl_owner != owner || !overlaps(rl, start, end)) {
                        rlp = &rl->rl_next;
                        continue;
                }
                if (type == F_UNLCK ||
                    (rl->rl_type == F_WRLCK && type != F_WRLCK))
                        *weakerp = 1;
                if (rl->rl_start < start && rl->rl_end > end) {
                        /* Sticks out on both sides: split it. */
                        struct reclock *right = rl_take(spare, owner,
                            rl->rl_pid, rl->rl_type, end + 1, rl->rl_end);

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
                        rl_free(rl);
                }
        }

        if (type != F_UNLCK) {
                /* Join the owner's touching locks of the type to it. */
                for (rlp = &vp->v_locks; (rl = *rlp) != NULL;) {
                        if (rl->rl_owner == owner && rl->rl_type == type &&
                            ((start > 0 && rl->rl_end == start - 1) ||
                            (end != RL_EOF && rl->rl_start == end + 1))) {
                                if (rl->rl_start < start)
                                        start = rl->rl_start;
                                if (rl->rl_end > end)
                                        end = rl->rl_end;
                                *rlp = rl->rl_next;
                                rl_free(rl);
                        } else {
                                rlp = &rl->rl_next;
                        }
                }
                rl = rl_take(spare, owner, pid, type, start, end);
                rl->rl_next = vp->v_locks;
                vp->v_locks = rl;
        }
        for (int i = 0; i < 2; i++)
                if (spare[i] != NULL)
                        rl_free(spare[i]);
        return 0;
}

/*
 * Wait, under reclock_mutex, until owner may have a lock of type over
 * the range: EAGAIN at once unless wait; EDEADLK if detect and the wait
 * would close a circle; EINTR or ERESTART for a signal.
 */
static int
acquire(struct vnode *vp, const void *owner, int type, int64_t start,
    int64_t end, int wait, int detect)
{
        struct reclock *rl;
        struct rlwait rw, **rwp;
        int error = 0, r;

        while ((rl = conflict(vp, owner, type, start, end)) != NULL) {
                if (!wait)
                        return EAGAIN;
                if (detect && would_deadlock(owner, rl->rl_owner, 0))
                        return EDEADLK;
                rw.rw_self = owner;
                rw.rw_for = rl->rl_owner;
                rw.rw_next = reclock_waiters;
                reclock_waiters = &rw;
                if (sv_wait_mutex_sig(&reclock_sv, &reclock_mutex) != 0 &&
                    (r = sig_sleep_check()) != 0)
                        error = r == 1 ? EINTR : ERESTART;
                mutex_lock(&reclock_mutex);
                for (rwp = &reclock_waiters; *rwp != &rw;
                    rwp = &(*rwp)->rw_next)
                        ;
                *rwp = rw.rw_next;
                if (error != 0)
                        return error;
        }
        return 0;
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
        int error = 0, weaker;

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
        if (fl->l_type != F_UNLCK)
                error = acquire(vp, p, fl->l_type, start, end,
                    cmd == F_SETLKW, 1);
        if (error == 0 &&
            (error = apply(vp, p, p->p_pid, fl->l_type, start, end,
            &weaker)) == 0 && weaker)
                sv_broadcast(&reclock_sv);
        mutex_unlock(&reclock_mutex);
        return error;
}

/*
 * flock(fd, op) on the open file fp: LOCK_SH, LOCK_EX or LOCK_UN, with
 * LOCK_NB not to wait. As in BSD, a lock is changed by giving it up and
 * then asking for the new one, so two holders of a shared lock that both
 * ask for an exclusive one do not wait for each other.
 */
int
reclock_flock(struct file *fp, int op)
{
        struct vnode *vp = fp->f_vnode;
        int type, error, weaker;

        switch (op & ~LOCK_NB) {
        case LOCK_SH:
                type = F_RDLCK;
                break;
        case LOCK_EX:
                type = F_WRLCK;
                break;
        case LOCK_UN:
                type = F_UNLCK;
                break;
        default:
                return EINVAL;
        }
        mutex_lock(&reclock_mutex);
        if ((error = apply(vp, fp, -1, F_UNLCK, 0, RL_EOF, &weaker)) == 0) {
                if (weaker)
                        sv_broadcast(&reclock_sv);
                if (type != F_UNLCK &&
                    (error = acquire(vp, fp, type, 0, RL_EOF,
                    (op & LOCK_NB) == 0, 0)) == 0)
                        error = apply(vp, fp, -1, type, 0, RL_EOF, &weaker);
        }
        mutex_unlock(&reclock_mutex);
        return error;
}

/*
 * owner's locks on vp all go: a process's when it closes a descriptor
 * for vp, an open file's when it is last closed.
 */
void
reclock_release(struct vnode *vp, const void *owner)
{
        struct reclock **rlp, *rl;
        int any = 0;

        /*
         * An unlocked look first: a process's locks are set only by its
         * LWPs, and one that set a lock through a descriptor this close
         * raced with looks at the descriptor again afterwards
         * (sys_fcntl), and drops the locks if it has gone; an open
         * file's are set only while a reference to it is held, so not
         * once its last is being given up.
         */
        if (vp->v_locks == NULL)
                return;
        mutex_lock(&reclock_mutex);
        for (rlp = &vp->v_locks; (rl = *rlp) != NULL;) {
                if (rl->rl_owner == owner) {
                        *rlp = rl->rl_next;
                        rl_free(rl);
                        any = 1;
                } else {
                        rlp = &rl->rl_next;
                }
        }
        if (any)
                sv_broadcast(&reclock_sv);
        mutex_unlock(&reclock_mutex);
}
