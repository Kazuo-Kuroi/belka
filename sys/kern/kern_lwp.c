/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * LWPs: creating and ending them, waiting for them, and the operations
 * that act on all the LWPs of a process, after UnixWare.
 *
 * A process's live LWPs are on p_lwps, and exited ones that someone
 * may _lwp_wait for on p_lwpzombies, both under p_siglock. LWP ids
 * count up from 1 and are not reused.
 *
 * Process-wide operations:
 * - Making the process single (exit, a fatal signal, exec): every other
 *   LWP is marked LW_DESTROY and exits at its next chance; the caller
 *   waits for them. If another LWP is already doing so, the caller just
 *   exits, and that operation wins (UnixWare's rule).
 * - Rendezvous (forkall): every other LWP parks before returning to
 *   user mode, with its user registers in its trap frame, until the
 *   requester is done.
 * - Suspension (_lwp_suspend) and stops (job control) park LWPs the
 *   same way. Parked LWPs count as quiesced.
 *
 * An LWP notices all of these in sig_userret() on its way to user
 * mode: they show in kt_sigpend, which also ends interruptible sleeps,
 * and such a sleep then returns ERESTART so that the system call is
 * reissued once the LWP is released.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/lwp.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/schedclass.h>
#include <sys/signal.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/ucontext.h>
#include <sys/wait.h>

#include <machine/intr.h>
#include <machine/md_var.h>
#include <machine/vmparam.h>

#include <vm/as.h>
#include <vm/hat.h>

#define USER_PRI        PRI_KERNEL      /* until the thread joins TS */
#define SEMA_HASH       64

static struct kmem_zone *lwp_zone;

void
lwp_sysinit(void)
{
        lwp_zone = kmem_zone_init(sizeof(struct lwp), "lwp");
        lwp_sema_init();
}

/*
 * A new LWP of p, linked in as live. EAGAIN if p is being made single
 * (it would be destroyed at once).
 */
int
lwp_alloc(struct proc *p, struct lwp **lp)
{
        struct lwp *l = kmem_zone_alloc(lwp_zone, KM_SLEEP);
        int s;

        memset(l, 0, sizeof(*l));
        l->l_proc = p;
        l->l_altflags = SS_DISABLE;
        l->l_cred = cred_getproc(p);
        s = spin_lock(&p->p_siglock);
        if ((p->p_lwpflags & PL_SINGLE) != 0) {
                spin_unlock(&p->p_siglock, s);
                crfree(l->l_cred);
                kmem_zone_free(lwp_zone, l);
                return EAGAIN;
        }
        l->l_lwpid = ++p->p_lwpnext;
        l->l_next = p->p_lwps;
        p->p_lwps = l;
        p->p_nlwp++;
        spin_unlock(&p->p_siglock, s);
        *lp = l;
        return 0;
}

/* Free an LWP structure; its machine state is already gone. */
void
lwp_free(struct lwp *l)
{
        crfree(l->l_cred);
        kmem_zone_free(lwp_zone, l);
}

/* The live LWP of p with id lwpid, or NULL. p_siglock is held. */
struct lwp *
lwp_find_locked(struct proc *p, lwpid_t lwpid)
{
        struct lwp *l = p->p_lwps;

        while (l != NULL && l->l_lwpid != lwpid)
                l = l->l_next;
        return l;
}

/*
 * Give l a stopped kernel thread that will run fn(arg) in the kernel,
 * with its user registers and floating-point area set up.
 */
void
lwp_newthread(struct lwp *l, void (*fn)(void *), void *arg)
{
        struct kthread *kt = kthread_create_stopped(fn, arg, USER_PRI,
            l->l_proc->p_comm);

        kt->kt_lwp = l;
        l->l_kthread = kt;
        md_lwp_init(l, kt->kt_stack);
}

/*
 * Put l's thread in time-sharing, in the job of leader (another LWP of
 * the process) if there is one, so that a process's LWPs share one
 * fair-share balance, at the weight of the process's nice value. The
 * thread has not run yet.
 */
void
lwp_join_ts(struct lwp *l, struct lwp *leader)
{
        (void)kthread_setclass(l->l_kthread, SCHED_TS,
            TS_NICEWEIGHT(l->l_proc->p_nice));
        if (leader != NULL && leader->l_kthread != NULL)
                (void)ts_join(l->l_kthread, leader->l_kthread);
}

void
lwp_run(struct lwp *l)
{
        kthread_run(l->l_kthread);
}

/* A new LWP's first step towards user mode. */
void
lwp_user_start(void *arg __unused)
{
        if (kthread_getclass(curthread) != SCHED_TS)
                (void)kthread_setclass(curthread, SCHED_TS,
                    TS_NICEWEIGHT(curproc->p_nice));
        if (curthread->kt_sigpend)
                sig_userret();
        user_enter(curlwp->l_tf);
}

/* Whether l must stay parked now. p_siglock is held. */
static int
lwp_must_park(const struct proc *p, const struct lwp *l)
{
        if ((l->l_flags & LW_DESTROY) != 0 ||
            ((p->p_sigpend | l->l_sigpend) & sigbit(SIGKILL)) != 0)
                return 0;
        return (l->l_flags & LW_SUSPREQ) != 0 || p->p_stopped ||
            ((p->p_lwpflags & PL_RENDEZ) != 0 && p->p_rendez != l);
}

/*
 * Park the calling LWP while it must (suspension, a stop, or another
 * LWP's rendezvous). p_siglock is held with cookie s and is held again
 * on return; the new cookie is returned.
 */
int
lwp_park(struct proc *p, struct lwp *l, int s)
{
        if (!lwp_must_park(p, l))
                return s;
        l->l_flags |= LW_QUIESCED;
        p->p_nquiesced++;
        sv_broadcast(&p->p_lwpsv);
        while (lwp_must_park(p, l)) {
                if ((l->l_flags & LW_SUSPREQ) != 0 &&
                    (l->l_flags & LW_SUSPENDED) == 0) {
                        l->l_flags |= LW_SUSPENDED;
                        sv_broadcast(&p->p_lwpsv);
                }
                sv_wait(&p->p_rendezsv, &p->p_siglock, s);
                s = spin_lock(&p->p_siglock);
        }
        l->l_flags &= ~(LW_QUIESCED | LW_SUSPENDED);
        p->p_nquiesced--;
        return s;
}

/* Whether the calling LWP has a reason to park. p_siglock is held. */
int
lwp_park_pending(const struct proc *p, const struct lwp *l)
{
        return lwp_must_park(p, l);
}

/*
 * The calling LWP ends. The last live LWP ending ends the process,
 * with status 0 (UnixWare). Otherwise the LWP becomes a zombie for
 * _lwp_wait, or, detached or destroyed, goes at once.
 */
void
lwp_exit(void)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        struct lwp **link;
        int s, zombie;

        s = spin_lock(&p->p_siglock);
        if (p->p_nlwp == 1 && (l->l_flags & LW_DESTROY) == 0) {
                spin_unlock(&p->p_siglock, s);
                proc_exit(CLD_EXITED, 0);
        }
        for (link = &p->p_lwps; *link != l; link = &(*link)->l_next)
                continue;
        *link = l->l_next;
        sig_lwp_exit_locked(l);
        p->p_nlwp--;
        p->p_lwpleaving++;
        p->p_ticks += curthread->kt_ticks;
        p->p_uticks += curthread->kt_uticks;
        zombie = (l->l_flags & (LW_DETACHED | LW_DESTROY)) == 0 &&
            (p->p_lwpflags & PL_SINGLE) == 0;
        /*
         * A zombie is visible to _lwp_wait at once, in the same step,
         * but marked LW_EXITING until this thread is done with it.
         */
        if (zombie) {
                l->l_flags |= LW_EXITING;
                l->l_next = p->p_lwpzombies;
                p->p_lwpzombies = l;
        }
        sv_broadcast(&p->p_lwpsv);
        spin_unlock(&p->p_siglock, s);

        /* Become a kernel thread before the machine state goes. */
        s = splhi();
        curthread->kt_lwp = NULL;
        curthread->kt_sigpend = 0;
        hat_activate(NULL);
        splx(s);
        md_lwp_free(l);

        /*
         * The last touch of p: until here, proc_single() (and so exit,
         * exec, and the parent's reaping) waits for this thread.
         */
        s = spin_lock(&p->p_siglock);
        if (zombie)
                l->l_flags &= ~LW_EXITING;
        p->p_lwpleaving--;
        sv_broadcast(&p->p_lwpsv);
        spin_unlock(&p->p_siglock, s);
        if (!zombie)
                lwp_free(l);
        kthread_exit();
}

/* Free p's LWP zombies (exit, exec). */
void
lwp_free_zombies(struct proc *p)
{
        for (;;) {
                int s = spin_lock(&p->p_siglock);
                struct lwp *l = p->p_lwpzombies;

                while (l != NULL && (l->l_flags & LW_EXITING) != 0) {
                        sv_wait(&p->p_lwpsv, &p->p_siglock, s);
                        s = spin_lock(&p->p_siglock);
                        l = p->p_lwpzombies;
                }
                if (l != NULL)
                        p->p_lwpzombies = l->l_next;
                spin_unlock(&p->p_siglock, s);
                if (l == NULL)
                        return;
                lwp_free(l);
        }
}

/* Mark every other LWP of p for destruction, and wake it. */
static void
lwp_destroy_others(struct proc *p, struct lwp *self)
{
        for (struct lwp *l = p->p_lwps; l != NULL; l = l->l_next) {
                if (l != self)
                        l->l_flags |= LW_DESTROY;
        }
        sig_recalc_all(p);
        sig_nudge_all(p);
        sv_broadcast(&p->p_rendezsv);
}

/*
 * Make the calling LWP the only one in its process, for exit or exec.
 * Returns 0 once every other LWP has exited and its thread has let go of
 * the process (so that exec may free the address space and the parent
 * the process), or -1 if another LWP got there first, in which case the
 * caller must just lwp_exit(). With
 * keep, the process stays marked (exit); otherwise it can have new LWPs
 * again afterwards (exec).
 */
int
proc_single(int keep)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        int s = spin_lock(&p->p_siglock);

        if ((p->p_lwpflags & PL_SINGLE) != 0 ||
            (l->l_flags & LW_DESTROY) != 0) {
                spin_unlock(&p->p_siglock, s);
                return -1;
        }
        p->p_lwpflags |= PL_SINGLE;
        p->p_single = l;
        lwp_destroy_others(p, l);
        while (p->p_nlwp > 1 || p->p_lwpleaving > 0) {
                sv_wait(&p->p_lwpsv, &p->p_siglock, s);
                s = spin_lock(&p->p_siglock);
        }
        if (!keep) {
                p->p_lwpflags &= ~PL_SINGLE;
                p->p_single = NULL;
        }
        spin_unlock(&p->p_siglock, s);
        lwp_free_zombies(p);
        return 0;
}

/*
 * Park every other LWP of the calling process (forkall). Returns 0 with
 * them parked, or EINTR if the caller is being destroyed meanwhile, or
 * EAGAIN if a process-wide operation is already under way.
 */
int
lwp_rendezvous(void)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        int s = spin_lock(&p->p_siglock);

        if ((p->p_lwpflags & (PL_SINGLE | PL_RENDEZ)) != 0) {
                spin_unlock(&p->p_siglock, s);
                return EAGAIN;
        }
        p->p_lwpflags |= PL_RENDEZ;
        p->p_rendez = l;
        sig_recalc_all(p);
        sig_nudge_all(p);
        while (p->p_nquiesced < p->p_nlwp - 1) {
                if ((l->l_flags & LW_DESTROY) != 0) {
                        spin_unlock(&p->p_siglock, s);
                        lwp_rendezvous_done();
                        return EINTR;
                }
                sv_wait(&p->p_lwpsv, &p->p_siglock, s);
                s = spin_lock(&p->p_siglock);
        }
        spin_unlock(&p->p_siglock, s);
        return 0;
}

void
lwp_rendezvous_done(void)
{
        struct proc *p = curproc;
        int s = spin_lock(&p->p_siglock);

        p->p_lwpflags &= ~PL_RENDEZ;
        p->p_rendez = NULL;
        sig_recalc_all(p);
        sv_broadcast(&p->p_rendezsv);
        spin_unlock(&p->p_siglock, s);
}

/* --- System calls. --- */

/* _lwp_create(ucontext_t *, flags): the new LWP's id. */
int
sys_lwpcreate(const uint64_t *args, uint64_t *rv)
{
        struct lwp *l = curlwp, *nl;
        struct proc *p = l->l_proc;
        ucontext_t *uc;
        uint64_t flags = args[1];
        int s, error;

        if ((flags & ~(uint64_t)(LWP_DETACHED | LWP_SUSPENDED)) != 0)
                return EINVAL;
        uc = kmem_alloc(sizeof(*uc), KM_SLEEP);
        if ((error = copyin((const void *)(uintptr_t)args[0], uc,
            sizeof(*uc))) != 0 || (error = md_context_ok(uc)) != 0 ||
            (error = lwp_alloc(p, &nl)) != 0) {
                kmem_free(uc, sizeof(*uc));
                return error;
        }
        lwp_newthread(nl, lwp_user_start, NULL);
        md_lwp_setcontext(nl, uc);
        s = spin_lock(&p->p_siglock);
        nl->l_sigmask = (uc->uc_flags & UC_SIGMASK) != 0 ?
            sig_mask_from_user(&uc->uc_sigmask) : l->l_sigmask;
        if ((flags & LWP_DETACHED) != 0)
                nl->l_flags |= LW_DETACHED;
        if ((flags & LWP_SUSPENDED) != 0)
                nl->l_flags |= LW_SUSPREQ;
        sig_recalc_lwp(p, nl);
        spin_unlock(&p->p_siglock, s);
        kmem_free(uc, sizeof(*uc));
        lwp_join_ts(nl, l);
        rv[0] = nl->l_lwpid;
        lwp_run(nl);
        return 0;
}

int
sys_lwpexit(const uint64_t *args __unused, uint64_t *rv __unused)
{
        lwp_exit();
}

/*
 * _lwp_wait(wait_for, lwpid_t *departed): wait for LWP wait_for, or any
 * (0), to exit, and reap it.
 */
int
sys_lwpwait(const uint64_t *args, uint64_t *rv __unused)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        lwpid_t want = (lwpid_t)args[0];
        int s, r;

        if (want == l->l_lwpid)
                return EDEADLK;
        s = spin_lock(&p->p_siglock);
        for (;;) {
                struct lwp **link, *z;
                int others = 0;

                for (link = &p->p_lwpzombies; (z = *link) != NULL;
                    link = &z->l_next) {
                        if (want == 0 || z->l_lwpid == want)
                                break;
                }
                if (z != NULL && (z->l_flags & LW_EXITING) != 0) {
                        /* Found, but its thread is not quite gone. */
                        sv_wait(&p->p_lwpsv, &p->p_siglock, s);
                        s = spin_lock(&p->p_siglock);
                        continue;
                }
                if (z != NULL) {
                        lwpid_t id = z->l_lwpid;

                        *link = z->l_next;
                        spin_unlock(&p->p_siglock, s);
                        lwp_free(z);
                        return args[1] != 0 ? copyout(&id,
                            (void *)(uintptr_t)args[1], sizeof(id)) : 0;
                }
                for (struct lwp *o = p->p_lwps; o != NULL; o = o->l_next) {
                        if (o != l && (o->l_flags & LW_DETACHED) == 0 &&
                            (want == 0 || o->l_lwpid == want))
                                others = 1;
                }
                if (!others) {
                        spin_unlock(&p->p_siglock, s);
                        return ESRCH;
                }
                if (sv_wait_sig(&p->p_lwpsv, &p->p_siglock, s) != 0 &&
                    (r = sig_sleep_check()) != 0)
                        return r == 1 ? EINTR : ERESTART;
                s = spin_lock(&p->p_siglock);
        }
}

int
sys_lwpself(const uint64_t *args __unused, uint64_t *rv)
{
        rv[0] = curlwp->l_lwpid;
        return 0;
}

/* _lwp_info(struct lwpinfo *): the calling LWP's user and system time. */
int
sys_lwpinfo(const uint64_t *args, uint64_t *rv __unused)
{
        struct lwpinfo li;
        uint64_t user = curthread->kt_uticks;
        uint64_t sys = curthread->kt_ticks - user;

        memset(&li, 0, sizeof(li));
        li.lwp_utime_sec = (int64_t)(user / HZ);
        li.lwp_utime_nsec = (int64_t)(user % HZ) * (1000000000 / HZ);
        li.lwp_stime_sec = (int64_t)(sys / HZ);
        li.lwp_stime_nsec = (int64_t)(sys % HZ) * (1000000000 / HZ);
        return copyout(&li, (void *)(uintptr_t)args[0], sizeof(li));
}

/* _lwp_kill(lwpid, sig): a signal for one LWP of the process. */
int
sys_lwpkill(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        lwpid_t id = (lwpid_t)args[0];
        int sig = (int)args[1];
        struct lwp *t;
        siginfo_t si;
        int s, error = 0;

        if (sig < 0 || sig > MAXSIG)
                return EINVAL;
        memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code = SI_LWP;
        si.si_pid = p->p_pid;
        s = spin_lock(&p->p_siglock);
        if ((t = lwp_find_locked(p, id)) == NULL) {
                spin_unlock(&p->p_siglock, s);
                return ESRCH;
        }
        if (sig != 0)
                error = sigtolwp_locked(p, t, sig, &si);
        spin_unlock(&p->p_siglock, s);
        return error;
}

/*
 * _lwp_suspend(lwpid): suspend another LWP and wait until it is, or
 * the caller at its return to user mode.
 */
int
sys_lwpsuspend(const uint64_t *args, uint64_t *rv __unused)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        lwpid_t id = (lwpid_t)args[0];
        struct lwp *t;
        int s = spin_lock(&p->p_siglock);

        if ((t = lwp_find_locked(p, id)) == NULL) {
                spin_unlock(&p->p_siglock, s);
                return ESRCH;
        }
        t->l_flags |= LW_SUSPREQ;
        sig_recalc_lwp(p, t);
        if (t != l) {
                sig_nudge_lwp(t);
                while ((t = lwp_find_locked(p, id)) != NULL &&
                    (t->l_flags & LW_SUSPREQ) != 0 &&
                    (t->l_flags & LW_SUSPENDED) == 0 &&
                    (l->l_flags & LW_DESTROY) == 0) {
                        sv_wait(&p->p_lwpsv, &p->p_siglock, s);
                        s = spin_lock(&p->p_siglock);
                }
        }
        spin_unlock(&p->p_siglock, s);
        return 0;
}

int
sys_lwpcontinue(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct lwp *t;
        int s = spin_lock(&p->p_siglock);

        if ((t = lwp_find_locked(p, (lwpid_t)args[0])) == NULL) {
                spin_unlock(&p->p_siglock, s);
                return ESRCH;
        }
        t->l_flags &= ~LW_SUSPREQ;
        sig_recalc_lwp(p, t);
        sv_broadcast(&p->p_rendezsv);
        spin_unlock(&p->p_siglock, s);
        return 0;
}

/*
 * LWP semaphores and address waits. Both are keyed by address space
 * and user address, hashed to a bucket whose mutex orders them and on
 * whose sync variable their waiters sleep.
 *
 * A semaphore's count lives in user memory; the kernel changes it only
 * under the bucket's mutex, and waiters recheck it each time one is
 * posted.
 *
 * An address waiter (_lwp_addr_wait) is queued on the bucket while the
 * mutex is held from the check of the word on: a waker, which marks
 * waiters under the same mutex, either finds it queued or ran before
 * the check and changed the word first. Waiters sleep on the bucket's
 * sync variable and go back to sleep until marked.
 */
struct addrwaiter {
        struct addrwaiter *aw_next;
        const void      *aw_as;
        uint64_t        aw_addr;
        int             aw_woken;
};

struct semabucket {
        struct mutex    sb_lock;
        struct sv       sb_sv;
        struct addrwaiter *sb_waiters;  /* address waiters, sb_lock */
};

static struct semabucket sema_hash[SEMA_HASH];

void
lwp_sema_init(void)
{
        for (int i = 0; i < SEMA_HASH; i++) {
                mutex_init(&sema_hash[i].sb_lock, "lwp_sema");
                sv_init(&sema_hash[i].sb_sv, "lwp_sema");
        }
}

static struct semabucket *
sema_bucket(uint64_t uaddr)
{
        uintptr_t key = (uintptr_t)curproc->p_as ^ (uintptr_t)(uaddr >> 4);

        return &sema_hash[(key ^ (key >> 7)) % SEMA_HASH];
}

static int
sema_take(uint64_t uaddr, int block)
{
        struct semabucket *b = sema_bucket(uaddr);
        int32_t *uc = (int32_t *)(uintptr_t)uaddr;
        int32_t count;
        int error, r;

        if ((uaddr & 3) != 0)
                return EINVAL;
        mutex_lock(&b->sb_lock);
        for (;;) {
                if ((error = copyin(uc, &count, sizeof(count))) != 0)
                        break;
                if (count > 0) {
                        count--;
                        error = copyout(&count, uc, sizeof(count));
                        break;
                }
                if (!block) {
                        error = EBUSY;
                        break;
                }
                if (sv_wait_mutex_sig(&b->sb_sv, &b->sb_lock) != 0 &&
                    (r = sig_sleep_check()) != 0)
                        return r == 1 ? EINTR : ERESTART;
                mutex_lock(&b->sb_lock);
        }
        mutex_unlock(&b->sb_lock);
        return error;
}

int
sys_lwp_sema_wait(const uint64_t *args, uint64_t *rv __unused)
{
        return sema_take(args[0], 1);
}

int
sys_lwp_sema_trywait(const uint64_t *args, uint64_t *rv __unused)
{
        return sema_take(args[0], 0);
}

int
sys_lwp_sema_post(const uint64_t *args, uint64_t *rv __unused)
{
        struct semabucket *b = sema_bucket(args[0]);
        int32_t *uc = (int32_t *)(uintptr_t)args[0];
        int32_t count;
        int error;

        if ((args[0] & 3) != 0)
                return EINVAL;
        mutex_lock(&b->sb_lock);
        if ((error = copyin(uc, &count, sizeof(count))) == 0) {
                count++;
                error = copyout(&count, uc, sizeof(count));
        }
        mutex_unlock(&b->sb_lock);
        if (error == 0)
                sv_broadcast(&b->sb_sv);
        return error;
}

/*
 * _lwp_addr_wait(int32_t *addr, int32_t expected, const timespec *rel):
 * sleep while *addr holds expected, until _lwp_addr_wake on addr, a
 * signal, or the relative timeout (none if rel is NULL). Returns EAGAIN
 * at once if *addr differs, ETIMEDOUT when the time runs out, and EINTR
 * when a handler is due; a wake may also be spurious.
 */
int
sys_lwp_addr_wait(const uint64_t *args, uint64_t *rv __unused)
{
        struct lwp *l = curlwp;
        uint64_t uaddr = args[0];
        struct semabucket *b = sema_bucket(uaddr);
        struct addrwaiter aw, **link;
        int32_t word;
        long ticks = -1;
        int64_t id;
        int error, r;

        if ((uaddr & 3) != 0)
                return EINVAL;
        if (args[2] != 0 && (error = lwp_timeout_ticks(args[2], &ticks)) != 0)
                return error;
        mutex_lock(&b->sb_lock);
        if ((error = copyin((const void *)(uintptr_t)uaddr, &word,
            sizeof(word))) != 0 || word != (int32_t)args[1]) {
                mutex_unlock(&b->sb_lock);
                return error != 0 ? error : EAGAIN;
        }
        if (ticks == 0) {
                mutex_unlock(&b->sb_lock);
                return ETIMEDOUT;
        }
        aw.aw_as = l->l_proc->p_as;
        aw.aw_addr = uaddr;
        aw.aw_woken = 0;
        aw.aw_next = b->sb_waiters;
        b->sb_waiters = &aw;
        id = lwp_timeout_start(ticks);
        for (;;) {
                if (aw.aw_woken) {
                        error = 0;
                        break;
                }
                if (l->l_timedout) {
                        error = ETIMEDOUT;
                        break;
                }
                if (sv_wait_mutex_sig(&b->sb_sv, &b->sb_lock) != 0 &&
                    !aw.aw_woken && !l->l_timedout &&
                    (r = sig_sleep_check()) != 0) {
                        mutex_lock(&b->sb_lock);
                        error = aw.aw_woken ? 0 : r == 1 ? EINTR : ERESTART;
                        break;
                }
                mutex_lock(&b->sb_lock);
        }
        for (link = &b->sb_waiters; *link != &aw; link = &(*link)->aw_next)
                continue;
        *link = aw.aw_next;
        mutex_unlock(&b->sb_lock);
        lwp_timeout_stop(id);
        return error;
}

/*
 * _lwp_addr_wake(int32_t *addr, int n): wake up to n LWPs of this
 * process waiting on addr (n <= 0: all of them). Returns how many.
 */
int
sys_lwp_addr_wake(const uint64_t *args, uint64_t *rv)
{
        uint64_t uaddr = args[0];
        struct semabucket *b = sema_bucket(uaddr);
        const void *as = curproc->p_as;
        int n = (int)args[1], woken = 0;

        if ((uaddr & 3) != 0)
                return EINVAL;
        mutex_lock(&b->sb_lock);
        for (struct addrwaiter *aw = b->sb_waiters; aw != NULL &&
            (n <= 0 || woken < n); aw = aw->aw_next) {
                if (aw->aw_as == as && aw->aw_addr == uaddr &&
                    !aw->aw_woken) {
                        aw->aw_woken = 1;
                        woken++;
                }
        }
        mutex_unlock(&b->sb_lock);
        if (woken > 0)
                sv_broadcast(&b->sb_sv);
        rv[0] = (uint64_t)woken;
        return 0;
}
