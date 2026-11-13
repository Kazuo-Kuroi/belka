/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-13
 * SPDX-License-Identifier: MIT
 */

/*
 * Credentials (see <sys/cred.h>) and the system calls on user and group
 * ids, with System V numbers: setuid 23, getuid 24, setgid 46, getgid
 * 47, setgroups 91, getgroups 92, setegid 136, seteuid 141.
 *
 * The rules are POSIX's with saved ids. The superuser's setuid sets all
 * three user ids, and seteuid the effective one only; anyone else may
 * set the effective id to the real or the saved one. Group ids follow
 * the same rules, privilege still being an effective user id of 0.
 * Only the superuser may set the supplementary groups.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/spinlock.h>
#include <sys/sysent.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#define MAXUID          0x7fffffff      /* largest user or group id */

static struct kmem_zone *cred_zone;

void
cred_init(void)
{
        cred_zone = kmem_zone_init(sizeof(struct cred), "cred");
}

void
crhold(struct cred *cr)
{
        (void)atomic_inc_32_nv(&cr->cr_ref);
}

/* Drop a reference; kcred is never freed (it starts with one). */
void
crfree(struct cred *cr)
{
        if (atomic_dec_32_nv(&cr->cr_ref) == 0)
                kmem_zone_free(cred_zone, cr);
}

/* A private copy of cr, with one reference. */
struct cred *
crdup(const struct cred *cr)
{
        struct cred *ncr = kmem_zone_alloc(cred_zone, KM_SLEEP);

        *ncr = *cr;
        ncr->cr_ref = 1;
        return ncr;
}

/* Whether gid is the effective group or a supplementary one of cr. */
int
groupmember(gid_t gid, const struct cred *cr)
{
        if (gid == cr->cr_gid)
                return 1;
        for (int i = 0; i < cr->cr_ngroups; i++)
                if (cr->cr_groups[i] == gid)
                        return 1;
        return 0;
}

/* A reference to p's credentials. */
struct cred *
cred_getproc(struct proc *p)
{
        struct cred *cr;
        int s = spin_lock(&p->p_crlock);

        cr = p->p_cred;
        crhold(cr);
        spin_unlock(&p->p_crlock, s);
        return cr;
}

/*
 * Give p the credentials cr, whose reference passes to p. p is the
 * calling process, and p_lock is held.
 */
void
cred_setproc(struct proc *p, struct cred *cr)
{
        struct cred *old;
        int s = spin_lock(&p->p_crlock);

        old = p->p_cred;
        p->p_cred = cr;
        spin_unlock(&p->p_crlock, s);
        crfree(old);
}

/*
 * Bring l's credentials up to date with its process's, on entry to a
 * system call. The unlocked look is only a hint: the process's own
 * changes are made before it returns from the call that makes them.
 */
void
cred_refresh(struct lwp *l)
{
        struct proc *p = l->l_proc;
        struct cred *old = l->l_cred;

        if ((struct cred *)(uintptr_t)atomic_load_64(&p->p_cred) == old)
                return;
        l->l_cred = cred_getproc(p);
        crfree(old);
}

struct cred *
cred_get(void)
{
        struct lwp *l = curlwp;

        return l != NULL && l->l_cred != NULL ? l->l_cred : kcred;
}

/* --- System calls. --- */

/* Which of the ids an id call sets. */
#define SET_USER        0x1
#define SET_EFFECTIVE   0x2     /* the effective id only */

/*
 * Set the calling process's user (or, without SET_USER, group) id to
 * id, by the rules above.
 */
static int
setid(uint64_t arg, int how)
{
        struct proc *p = curproc;
        struct cred *cr, *ncr;
        uint32_t id = (uint32_t)arg;
        uint32_t *eff, *real, *saved;
        int root;

        if (arg > MAXUID)
                return EINVAL;
        mutex_lock(&p->p_lock);
        cr = p->p_cred;
        root = CRED_ISROOT(cr);
        ncr = crdup(cr);
        if ((how & SET_USER) != 0) {
                eff = &ncr->cr_uid;
                real = &ncr->cr_ruid;
                saved = &ncr->cr_suid;
        } else {
                eff = &ncr->cr_gid;
                real = &ncr->cr_rgid;
                saved = &ncr->cr_sgid;
        }
        if (root && (how & SET_EFFECTIVE) == 0) {
                *eff = *real = *saved = id;
        } else if (root || id == *real || id == *saved) {
                *eff = id;
        } else {
                mutex_unlock(&p->p_lock);
                crfree(ncr);
                return EPERM;
        }
        cred_setproc(p, ncr);
        mutex_unlock(&p->p_lock);
        cred_refresh(curlwp);
        return 0;
}

/* setuid(uid) */
int
sys_setuid(const uint64_t *args, uint64_t *rv __unused)
{
        return setid(args[0], SET_USER);
}

/* seteuid(uid) */
int
sys_seteuid(const uint64_t *args, uint64_t *rv __unused)
{
        return setid(args[0], SET_USER | SET_EFFECTIVE);
}

/* setgid(gid) */
int
sys_setgid(const uint64_t *args, uint64_t *rv __unused)
{
        return setid(args[0], 0);
}

/* setegid(gid) */
int
sys_setegid(const uint64_t *args, uint64_t *rv __unused)
{
        return setid(args[0], SET_EFFECTIVE);
}

/* getuid(): the real user id, and the effective one second (System V). */
int
sys_getuid(const uint64_t *args __unused, uint64_t *rv)
{
        struct cred *cr = CRED();

        rv[0] = cr->cr_ruid;
        rv[1] = cr->cr_uid;
        return 0;
}

/* getgid(): the real group id, and the effective one second. */
int
sys_getgid(const uint64_t *args __unused, uint64_t *rv)
{
        struct cred *cr = CRED();

        rv[0] = cr->cr_rgid;
        rv[1] = cr->cr_gid;
        return 0;
}

/*
 * getgroups(n, gid_t *list): the supplementary groups, or with n 0
 * only their number.
 */
int
sys_getgroups(const uint64_t *args, uint64_t *rv)
{
        struct cred *cr = CRED();
        int64_t n = (int64_t)args[0];
        int error = 0;

        if (n < 0)
                return EINVAL;
        if (n != 0) {
                if (n < cr->cr_ngroups)
                        return EINVAL;
                error = copyout(cr->cr_groups, (void *)(uintptr_t)args[1],
                    (size_t)cr->cr_ngroups * sizeof(gid_t));
        }
        rv[0] = (uint64_t)cr->cr_ngroups;
        return error;
}

/* setgroups(n, const gid_t *list) */
int
sys_setgroups(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        int64_t n = (int64_t)args[0];
        gid_t groups[NGROUPS_MAX];
        struct cred *ncr;
        int error;

        if (n < 0 || n > NGROUPS_MAX)
                return EINVAL;
        if (n != 0 && (error = copyin((const void *)(uintptr_t)args[1],
            groups, (size_t)n * sizeof(gid_t))) != 0)
                return error;
        for (int64_t i = 0; i < n; i++)
                if (groups[i] > MAXUID)
                        return EINVAL;
        mutex_lock(&p->p_lock);
        if (!CRED_ISROOT(p->p_cred)) {
                mutex_unlock(&p->p_lock);
                return EPERM;
        }
        ncr = crdup(p->p_cred);
        ncr->cr_ngroups = (int)n;
        memcpy(ncr->cr_groups, groups, (size_t)n * sizeof(gid_t));
        cred_setproc(p, ncr);
        mutex_unlock(&p->p_lock);
        cred_refresh(curlwp);
        return 0;
}
