/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-20
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/proc.h>
#include <sys/schedclass.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <machine/md_var.h>
#include <machine/atomic.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/as.h>
#include <vm/seg_vn.h>

/* Membership changes take proctree_lock before sg_lock. */
struct sharegroup {
        struct mutex sg_lock;
        struct proc *sg_members;
        unsigned int sg_count;
        struct vnode *sg_dir, *sg_root;
        struct cred *sg_ids;
        mode_t sg_mask;
        rlim_t sg_fsize;
        uint64_t sg_version[4];
};

static const unsigned int shared_bits[4] = {
        PR_SDIR, PR_SUMASK, PR_SULIMIT, PR_SID
};

void
sproc_notify_exit(struct proc *p)
{
        unsigned char done = 1;

        if (p->p_sprocexited != 0) {
                membar_producer();
                (void)copyout(&done, (void *)p->p_sprocexited, 1);
                p->p_sprocexited = 0;
        }
}

static void
sproc_group_init(struct proc *p)
{
        struct sharegroup *g = kmem_zalloc(sizeof(*g), KM_SLEEP);

        mutex_init(&g->sg_lock, "sharegroup");
        g->sg_members = p;
        g->sg_count = 1;
        mutex_lock(&p->p_lock);
        g->sg_dir = p->p_cdir;
        g->sg_root = p->p_rdir;
        if (g->sg_dir != NULL)
                VN_HOLD(g->sg_dir);
        if (g->sg_root != NULL)
                VN_HOLD(g->sg_root);
        g->sg_mask = p->p_cmask;
        g->sg_fsize = p->p_rlimit[RLIMIT_FSIZE].rlim_cur;
        g->sg_ids = p->p_cred;
        crhold(g->sg_ids);
        mutex_unlock(&p->p_lock);
        for (int i = 0; i < 4; i++)
                g->sg_version[i] = p->p_shseen[i] = 1;
        mutex_lock(&proctree_lock);
        p->p_shmask = PR_SALL;
        p->p_sharegroup = g;
        mutex_unlock(&proctree_lock);
}

/* Publish completed changes without holding a lock across the syscall. */
void
sproc_publish(struct proc *p, unsigned int bits)
{
        struct sharegroup *g = p->p_sharegroup;
        struct vnode *dir = NULL, *root = NULL;
        struct cred *ids = NULL;

        bits &= p->p_shmask;
        if (g == NULL || bits == 0)
                return;
        mutex_lock(&g->sg_lock);
        mutex_lock(&p->p_lock);
        if ((bits & PR_SDIR) != 0) {
                dir = g->sg_dir;
                root = g->sg_root;
                g->sg_dir = p->p_cdir;
                g->sg_root = p->p_rdir;
                if (g->sg_dir != NULL)
                        VN_HOLD(g->sg_dir);
                if (g->sg_root != NULL)
                        VN_HOLD(g->sg_root);
        }
        if ((bits & PR_SUMASK) != 0)
                g->sg_mask = p->p_cmask;
        if ((bits & PR_SULIMIT) != 0)
                g->sg_fsize = p->p_rlimit[RLIMIT_FSIZE].rlim_cur;
        if ((bits & PR_SID) != 0) {
                ids = g->sg_ids;
                g->sg_ids = p->p_cred;
                crhold(g->sg_ids);
        }
        for (int i = 0; i < 4; i++)
                if ((bits & shared_bits[i]) != 0)
                        p->p_shseen[i] = ++g->sg_version[i];
        mutex_unlock(&p->p_lock);
        mutex_unlock(&g->sg_lock);
        if (dir != NULL)
                VN_RELE(dir);
        if (root != NULL)
                VN_RELE(root);
        if (ids != NULL)
                crfree(ids);
}

/* Only this process applies its resource snapshots, at a user boundary. */
void
sproc_sync(struct proc *p)
{
        struct sharegroup *g = p->p_sharegroup;
        struct vnode *dir = NULL, *root = NULL;

        if (g == NULL)
                return;
        mutex_lock(&g->sg_lock);
        mutex_lock(&p->p_lock);
        for (int i = 0; i < 4; i++) {
                if ((p->p_shmask & shared_bits[i]) == 0 ||
                    p->p_shseen[i] == g->sg_version[i])
                        continue;
                switch (shared_bits[i]) {
                case PR_SDIR:
                        dir = p->p_cdir;
                        root = p->p_rdir;
                        p->p_cdir = g->sg_dir;
                        p->p_rdir = g->sg_root;
                        if (p->p_cdir != NULL)
                                VN_HOLD(p->p_cdir);
                        if (p->p_rdir != NULL)
                                VN_HOLD(p->p_rdir);
                        break;
                case PR_SUMASK:
                        p->p_cmask = g->sg_mask;
                        break;
                case PR_SULIMIT:
                        p->p_rlimit[RLIMIT_FSIZE].rlim_cur = g->sg_fsize;
                        break;
                case PR_SID: {
                        struct cred *cr;

                        if (CRED_ISROOT(p->p_cred)) {
                                cr = g->sg_ids;
                                crhold(cr);
                        } else {
                                cr = crdup(p->p_cred);
                                cr->cr_uid = g->sg_ids->cr_uid;
                                cr->cr_gid = g->sg_ids->cr_gid;
                        }
                        cred_setproc(p, cr);
                        break;
                }
                }
                p->p_shseen[i] = g->sg_version[i];
        }
        mutex_unlock(&p->p_lock);
        mutex_unlock(&g->sg_lock);
        if (dir != NULL)
                VN_RELE(dir);
        if (root != NULL)
                VN_RELE(root);
}

void
sproc_drop_stack(struct proc *p, struct as *as)
{
        if (p->p_shstacksize != 0) {
                (void)as_unmap(as, p->p_shstack, p->p_shstacksize);
                p->p_shstack = 0;
                p->p_shstacksize = 0;
        }
}

void
sproc_detach(struct proc *p, int why)
{
        struct sharegroup *g;
        struct proc **link;
        int last = 0, s;

        mutex_lock(&proctree_lock);
        g = p->p_sharegroup;
        if (g != NULL) {
                mutex_lock(&g->sg_lock);
                for (link = &g->sg_members; *link != p;
                    link = &(*link)->p_shnext)
                        continue;
                *link = p->p_shnext;
                last = --g->sg_count == 0;
                if (p->p_exitsig != 0 &&
                    (!p->p_abortsig || (why != 0 && why != CLD_EXITED))) {
                        siginfo_t si = { 0 };

                        si.si_signo = p->p_exitsig;
                        si.si_code = why;
                        si.si_pid = p->p_pid;
                        si.si_uid = p->p_cred->cr_ruid;
                        for (struct proc *o = g->sg_members; o != NULL;
                            o = o->p_shnext)
                                (void)sigtoproc(o, si.si_signo, &si);
                }
                p->p_sharegroup = NULL;
                p->p_shnext = NULL;
                p->p_shmask = 0;
                mutex_unlock(&g->sg_lock);
        }
        mutex_unlock(&proctree_lock);
        s = spin_lock(&p->p_siglock);
        p->p_lwpflags &= ~PL_SPROC;
        if (why == 0)
                p->p_lwpflags &= ~PL_THREADED;
        spin_unlock(&p->p_siglock, s);
        if (g != NULL && why == 0 && kthread_getclass(curthread) == SCHED_TS)
                (void)kthread_setclass(curthread, SCHED_TS,
                    TS_NICEWEIGHT(p->p_nice));
        if (last) {
                if (g->sg_dir != NULL)
                        VN_RELE(g->sg_dir);
                if (g->sg_root != NULL)
                        VN_RELE(g->sg_root);
                crfree(g->sg_ids);
                kmem_free(g, sizeof(*g));
        }
}

static void
sproc_start(void *bad)
{
        if (bad != NULL)
                trapsignal(SIGSEGV, SEGV_MAPERR, (uint64_t)(uintptr_t)bad);
        sproc_userret();
        lwp_user_start(NULL);
}

static int
sproc_create(const uint64_t *args, uint64_t *rv, int supplied)
{
        struct proc *p = curproc, *cp;
        struct lwp *l = curlwp, *cl;
        struct sharegroup *g;
        struct as *as;
        struct segvn_crargs a = { NULL, 0, MAP_PRIVATE,
            PROT_READ | PROT_WRITE, PROT_ALL };
        unsigned int mask = (unsigned int)args[1] &
            (PR_SALL | PR_BLOCK | PR_NOLIBC);
        vaddr_t top = supplied ? args[3] : 0, base = 0;
        size_t len = supplied ? (size_t)args[4] : (size_t)-1;
        struct belka_sproc_private private = { NULL, NULL };
        void *bad = NULL;
        int s, error;

        if (top == 0 && len != (size_t)-1 && len < 8192)
                return EINVAL;
        if (args[5] != 0 && (error = copyin((const void *)args[5],
            &private, sizeof(private))) != 0)
                return error;
        if ((uintptr_t)private.bsp_private >= VM_USER_MAX ||
            (uintptr_t)private.bsp_exited >= VM_USER_MAX)
                return EINVAL;
        s = spin_lock(&p->p_siglock);
        if ((p->p_lwpflags & PL_THREADED) != 0 || p->p_nlwp != 1) {
                spin_unlock(&p->p_siglock, s);
                return EPERM;
        }
        p->p_lwpflags |= PL_SPROC;
        spin_unlock(&p->p_siglock, s);
        if (p->p_sharegroup == NULL)
                sproc_group_init(p);
        g = p->p_sharegroup;
        mask = (mask & p->p_shmask & PR_SALL) | PR_SPROC |
            (mask & (PR_BLOCK | PR_NOLIBC));
        if ((mask & PR_SADDR) != 0) {
                as = p->p_as;
                as_hold(as);
        } else if ((as = as_dup(p->p_as)) == NULL) {
                return ENOMEM;
        }
        if (top == 0) {
                if (len == (size_t)-1) {
                        rlim_t limit = rlimit_cur(p, RLIMIT_STACK);

                        len = limit == RLIM_INFINITY ? 64 * 1024 * 1024 :
                            (size_t)limit;
                }
                if (len == 0 || len > VM_USER_MAX - VM_USER_MIN) {
                        as_free(as);
                        return ENOMEM;
                }
                len = pround(len);
                if ((error = as_map_topdown(as, len, USRSTACK,
                    segvn_create, &a, &base)) != 0) {
                        as_free(as);
                        return error;
                }
                top = base + len;
        }
        cp = proc_alloc(p);
        if (cp == NULL) {
                if (base != 0)
                        (void)as_unmap(as, base, len);
                as_free(as);
                return EAGAIN;
        }
        cp->p_as = as;
        cp->p_sprocexited = (vaddr_t)private.bsp_exited;
        cp->p_shmask = mask;
        cp->p_lwpflags |= PL_SPROC;
        if (base != 0) {
                cp->p_shstack = base;
                cp->p_shstacksize = len;
        }
        if ((mask & PR_SFDS) != 0)
                fd_share_table(p, cp);
        else
                fd_dup_table(p, cp);
        mutex_lock(&p->p_lock);
        cp->p_cdir = p->p_cdir;
        cp->p_rdir = p->p_rdir;
        if (cp->p_cdir != NULL)
                VN_HOLD(cp->p_cdir);
        if (cp->p_rdir != NULL)
                VN_HOLD(cp->p_rdir);
        memcpy(cp->p_psargs, p->p_psargs, sizeof(cp->p_psargs));
        mutex_unlock(&p->p_lock);
        (void)lwp_alloc(cp, &cl);
        if (args[0] >= VM_USER_MAX)
                bad = (void *)(uintptr_t)args[0];
        else if (top < VM_USER_MIN + 16 || top > VM_USER_MAX)
                bad = (void *)(uintptr_t)top;
        lwp_newthread(cl, sproc_start, bad);
        md_lwp_sproc(l, cl, args[0], args[2], top, len,
            (uint64_t)(uintptr_t)private.bsp_private);
        s = spin_lock(&p->p_siglock);
        cl->l_sigmask = l->l_sigmask;
        cl->l_altsp = l->l_altsp;
        cl->l_altsize = l->l_altsize;
        cl->l_altflags = l->l_altflags;
        spin_unlock(&p->p_siglock, s);
        lwp_join_ts(cl, l);
        mutex_lock(&proctree_lock);
        mutex_lock(&g->sg_lock);
        cp->p_exitsig = p->p_exitsig;
        cp->p_abortsig = p->p_abortsig;
        cp->p_sharegroup = g;
        cp->p_shnext = g->sg_members;
        g->sg_members = cp;
        g->sg_count++;
        mutex_unlock(&g->sg_lock);
        cp->p_stat = SRUN;
        mutex_unlock(&proctree_lock);
        if ((mask & PR_BLOCK) != 0) {
                mutex_lock(&p->p_lock);
                if (p->p_blockwait < PR_MAXBLOCKCNT)
                        p->p_blockwait++;
                mutex_unlock(&p->p_lock);
        }
        rv[0] = (uint64_t)cp->p_pid;
        rv[1] = 0;
        lwp_run(cl);
        return 0;
}

int
sys_sproc(const uint64_t *args, uint64_t *rv)
{
        return sproc_create(args, rv, 0);
}

int
sys_sprocsp(const uint64_t *args, uint64_t *rv)
{
        return sproc_create(args, rv, 1);
}

/* proctree_lock keeps the target and any share-group membership alive. */
static int
sproc_canblock(struct proc *target)
{
        struct cred *caller = CRED(), *cr;
        struct sharegroup *g = target->p_sharegroup;
        uid_t uid, ruid;
        int allowed;

        if (target == curproc || CRED_ISROOT(caller))
                return 1;
        cr = cred_getproc(target);
        uid = cr->cr_uid;
        ruid = cr->cr_ruid;
        if (g != NULL && (target->p_shmask & PR_SID) != 0) {
                mutex_lock(&g->sg_lock);
                uid = g->sg_ids->cr_uid;
                if (CRED_ISROOT(cr))
                        ruid = g->sg_ids->cr_ruid;
                mutex_unlock(&g->sg_lock);
        }
        allowed = caller->cr_uid == uid || caller->cr_uid == ruid ||
            caller->cr_ruid == uid || caller->cr_ruid == ruid;
        crfree(cr);
        return allowed;
}

static int
sproc_block_one(struct proc *p, unsigned int action, int count)
{
        int s = spin_lock(&p->p_siglock);
        int threaded = (p->p_lwpflags & PL_THREADED) != 0;

        spin_unlock(&p->p_siglock, s);
        if (threaded || !sproc_canblock(p))
                return EPERM;
        mutex_lock(&p->p_lock);
        switch (action) {
        case 0:
                if (p->p_blockwait < PR_MAXBLOCKCNT)
                        p->p_blockwait++;
                break;
        case 1:
                if (p->p_blocksleep && p->p_blockwait > 0)
                        p->p_blockwait--;
                else if (p->p_blockcredit < PR_MAXBLOCKCNT)
                        p->p_blockcredit++;
                break;
        case 2:
                p->p_blockwait = count < 0 ? -count : 0;
                p->p_blockcredit = count > 0 ? count : 0;
                break;
        }
        sv_broadcast(&p->p_blocksv);
        mutex_unlock(&p->p_lock);
        return 0;
}

int
sys_procblk(const uint64_t *args, uint64_t *rv __unused)
{
        unsigned int action = (unsigned int)args[0];
        pid_t pid = (pid_t)args[1];
        int count = (int)args[2], error = 0;
        struct proc *p;

        if (action > 5 || (action % 3 == 2 &&
            (count < PR_MINBLOCKCNT || count > PR_MAXBLOCKCNT)))
                return EINVAL;
        mutex_lock(&proctree_lock);
        p = proc_find_locked(pid);
        if (p == NULL || p->p_stat == SZOMB) {
                error = ESRCH;
        } else if (action >= 3 && p->p_sharegroup != NULL) {
                for (struct proc *o = p->p_sharegroup->sg_members; o != NULL;
                    o = o->p_shnext) {
                        int e = sproc_block_one(o, action % 3, count);

                        if (e != 0)
                                error = e;
                }
        } else {
                error = sproc_block_one(p, action % 3, count);
        }
        mutex_unlock(&proctree_lock);
        return error;
}

void
sproc_userret(void)
{
        struct proc *p = curproc;

        sproc_sync(p);
        cred_refresh(curlwp);
        if (p->p_blockwait == 0)
                return;
        mutex_lock(&p->p_lock);
        while (p->p_blockwait != 0) {
                int consume = p->p_blockwait < p->p_blockcredit ?
                    p->p_blockwait : p->p_blockcredit;

                p->p_blockwait -= consume;
                p->p_blockcredit -= consume;
                if (p->p_blockwait == 0)
                        break;
                if (curthread->kt_sigpend) {
                        mutex_unlock(&p->p_lock);
                        sig_userret();
                        mutex_lock(&p->p_lock);
                        continue;
                }
                p->p_blocksleep = 1;
                (void)sv_wait_mutex_sig(&p->p_blocksv, &p->p_lock);
                mutex_lock(&p->p_lock);
                p->p_blocksleep = 0;
        }
        mutex_unlock(&p->p_lock);
}

void
sproc_unblock_on_exec(struct proc *p)
{
        struct proc *target;

        if (p->p_unblockpid == 0)
                return;
        mutex_lock(&proctree_lock);
        target = proc_find_locked(p->p_unblockpid);
        if (target != NULL && target->p_stat != SZOMB &&
            target->p_identity == p->p_unblockidentity)
                (void)sproc_block_one(target, 1, 0);
        p->p_unblockpid = 0;
        p->p_unblockidentity = 0;
        mutex_unlock(&proctree_lock);
}

int
sys_prctl(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc, *target;
        struct sharegroup *g;
        unsigned int option = (unsigned int)args[0];
        pid_t pid = (pid_t)args[1];
        int error = 0, s;

        switch (option) {
        case PR_GETSTACKSIZE:
                rv[0] = rlimit_cur(p, RLIMIT_STACK);
                return 0;
        case PR_SETSTACKSIZE:
                mutex_lock(&p->p_lock);
                if (args[1] > p->p_rlimit[RLIMIT_STACK].rlim_max &&
                    !CRED_ISROOT(CRED())) {
                        mutex_unlock(&p->p_lock);
                        return EPERM;
                }
                p->p_rlimit[RLIMIT_STACK].rlim_cur = args[1];
                p->p_rlimit[RLIMIT_STACK].rlim_max = args[1];
                rv[0] = args[1];
                mutex_unlock(&p->p_lock);
                return 0;
        case PR_INIT_THREADS:
                s = spin_lock(&p->p_siglock);
                if ((p->p_lwpflags & PL_SPROC) != 0)
                        error = EINVAL;
                else
                        p->p_lwpflags |= PL_THREADED;
                spin_unlock(&p->p_siglock, s);
                return error;
        case PR_ISBLOCKED:
        case PR_GETSHMASK:
                mutex_lock(&proctree_lock);
                target = pid == 0 || pid == p->p_pid ? p : proc_find_locked(pid);
                if (target == NULL || target->p_stat == SZOMB) {
                        error = ESRCH;
                } else if (option == PR_GETSHMASK &&
                    (p->p_sharegroup == NULL ||
                    p->p_sharegroup != target->p_sharegroup)) {
                        error = EINVAL;
                } else if (!sproc_canblock(target)) {
                        error = EPERM;
                } else if (option == PR_GETSHMASK) {
                        rv[0] = target == p ? p->p_shmask :
                            target->p_shmask & p->p_shmask;
                } else {
                        s = spin_lock(&target->p_siglock);
                        if ((target->p_lwpflags & PL_THREADED) == 0) {
                                spin_unlock(&target->p_siglock, s);
                                mutex_lock(&target->p_lock);
                                rv[0] = target->p_blocksleep ||
                                    target->p_blockwait > target->p_blockcredit;
                                mutex_unlock(&target->p_lock);
                        } else {
                                spin_unlock(&target->p_siglock, s);
                        }
                }
                mutex_unlock(&proctree_lock);
                return error;
        case PR_UNBLKONEXEC:
                mutex_lock(&proctree_lock);
                target = proc_find_locked(pid);
                if (pid == 0 || pid == p->p_pid || p->p_unblockpid != 0)
                        error = EINVAL;
                else if (target == NULL || target->p_stat == SZOMB)
                        error = ESRCH;
                else if (!sproc_canblock(target))
                        error = EPERM;
                else {
                        p->p_unblockpid = pid;
                        p->p_unblockidentity = target->p_identity;
                }
                mutex_unlock(&proctree_lock);
                return error;
        case PR_GETNSHARE:
        case PR_LASTSHEXIT:
        case PR_SETEXITSIG:
        case PR_SETABORTSIG:
        case PR_TERMCHILD:
                break;
        default:
                return EINVAL;
        }
        if ((option == PR_SETEXITSIG || option == PR_SETABORTSIG) &&
            args[1] >= NSIG)
                return EINVAL;
        mutex_lock(&proctree_lock);
        g = p->p_sharegroup;
        if (option == PR_GETNSHARE) {
                rv[0] = g != NULL ? g->sg_count : 0;
        } else if (option == PR_TERMCHILD) {
                p->p_termchild = 1;
        } else if (option == PR_LASTSHEXIT) {
                p->p_willexit = 1;
                rv[0] = 1;
                for (struct proc *o = g != NULL ? g->sg_members : NULL;
                    o != NULL; o = o->p_shnext)
                        if (!o->p_willexit)
                                rv[0] = 0;
        } else {
                struct proc *o = g != NULL ? g->sg_members : p;

                for (; o != NULL; o = g != NULL ? o->p_shnext : NULL) {
                        o->p_exitsig = (int)args[1];
                        o->p_abortsig = option == PR_SETABORTSIG && args[1] != 0;
                }
        }
        mutex_unlock(&proctree_lock);
        return 0;
}
