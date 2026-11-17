/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Signals, after UnixWare: posting, choosing, and acting on signals,
 * stopping and continuing, and the signal system calls.
 *
 * Posting (sigtoproc) follows the UnixWare rules: a signal already
 * pending is not posted again, and keeps its first siginfo; SIGCONT
 * cancels pending stop signals and a stop signal cancels a pending
 * SIGCONT; an ignored signal is discarded. The LWP is then nudged: an
 * interruptible sleep is ended, and a processor running it is asked to
 * reschedule, so it looks at its signals on the way back to user mode.
 *
 * On every return to user mode (sig_userret) the LWP takes SIGKILL
 * first, then the highest-numbered pending signal it does not hold:
 * - caught: a frame is built on the user stack and the handler runs,
 *   with the handler's mask and, unless SA_NODEFER, the signal held;
 * - default stop: the process stops until SIGCONT or SIGKILL;
 * - default ignore: discarded;
 * - default otherwise: the process exits as killed by the signal. There
 *   are no core files yet, so the status is CLD_KILLED, as UnixWare
 *   reports when no core was written.
 *
 * Kernel sleeps a signal may end (pause, sigsuspend, wait) use
 * sig_sleep_check(), which takes stops and discards in place, so a
 * stopped and continued process goes back to sleep, as in UnixWare.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/ucontext.h>
#include <sys/wait.h>

#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/frame.h>
#include <machine/md_var.h>
#include <machine/vmparam.h>

#define SIG_ALL         (sigbit(MAXSIG) | (sigbit(MAXSIG) - 1))
#define CANTMASK        (sigbit(SIGKILL) | sigbit(SIGSTOP))
#define IGNDEFAULT      (sigbit(SIGCONT) | sigbit(SIGCLD) | sigbit(SIGPWR) | \
                         sigbit(SIGWINCH) | sigbit(SIGURG) | \
                         sigbit(SIGWAITING) | sigbit(SIGLWP) | \
                         sigbit(SIGAIO))
#define STOPDEFAULT     (sigbit(SIGSTOP) | sigbit(SIGTSTP) | \
                         sigbit(SIGTTIN) | sigbit(SIGTTOU))
#define NORESET         (sigbit(SIGILL) | sigbit(SIGTRAP) | sigbit(SIGPWR))

#define H_DFL           ((uint64_t)0)
#define H_IGN           ((uint64_t)1)
#define H_HOLD          ((uint64_t)2)

/* Set up p's signal state; a child inherits its parent's actions. */
void
sig_proc_init(struct proc *p, struct proc *parent)
{
        spinlock_init(&p->p_siglock, "siglock");
        sv_init(&p->p_stopsv, "stopped");
        sv_init(&p->p_pausesv, "pause");
        if (parent != NULL) {
                int s = spin_lock(&parent->p_siglock);

                memcpy(p->p_sigact, parent->p_sigact, sizeof(p->p_sigact));
                p->p_sigflags = parent->p_sigflags;
                p->p_sigreturn = parent->p_sigreturn;
                spin_unlock(&parent->p_siglock, s);
        }
}

static int
sig_ignored(const struct proc *p, int sig)
{
        uint64_t h = p->p_sigact[sig].ksa_handler;

        return h == H_IGN || (h == H_DFL && (IGNDEFAULT & sigbit(sig)) != 0);
}

/* The highest-numbered signal of a non-empty set; SIGKILL first. */
static int
sig_pick(k_sigset_t set)
{
        int sig = MAXSIG;

        if ((set & sigbit(SIGKILL)) != 0)
                return SIGKILL;
        while ((set & sigbit(sig)) == 0)
                sig--;
        return sig;
}

/*
 * Whether LWP l has something to do on its way to user mode: a signal
 * it does not hold (or awaits in sigwait), or a reason to park or exit.
 * p_siglock is held.
 */
void
sig_recalc_lwp(struct proc *p, struct lwp *l)
{
        k_sigset_t avail = (p->p_sigpend | l->l_sigpend) &
            ~(l->l_sigmask & ~l->l_sigwait);

        if (l->l_kthread == NULL)
                return;
        l->l_kthread->kt_sigpend = avail != 0 || l->l_timedout ||
            (l->l_flags & (LW_DESTROY | LW_SUSPREQ)) != 0 || p->p_stopped ||
            ((p->p_lwpflags & PL_RENDEZ) != 0 && p->p_rendez != l);
}

void
sig_recalc_all(struct proc *p)
{
        for (struct lwp *l = p->p_lwps; l != NULL; l = l->l_next)
                sig_recalc_lwp(p, l);
}

/* Make l look at its signals soon. p_siglock is held. */
void
sig_nudge_lwp(struct lwp *l)
{
        struct kthread *kt = l->l_kthread;

        if (kt == NULL || !kt->kt_sigpend)
                return;
        sleep_interrupt(kt);
        if (kt->kt_state == KT_ONPROC && kt->kt_cpu != NULL &&
            kt->kt_cpu != curcpu())
                cpu_need_resched(kt->kt_cpu);
}

void
sig_nudge_all(struct proc *p)
{
        for (struct lwp *l = p->p_lwps; l != NULL; l = l->l_next)
                sig_nudge_lwp(l);
}

/*
 * Job control side effects of posting sig: SIGCONT and SIGKILL end a
 * stop and cancel pending stop signals; a stop signal cancels a pending
 * SIGCONT. p_siglock is held.
 */
static void
sig_jobctl(struct proc *p, int sig)
{
        k_sigset_t bit = sigbit(sig);
        k_sigset_t cancel = 0;

        if (sig == SIGCONT || sig == SIGKILL) {
                cancel = STOPDEFAULT;
                if (p->p_stopped) {
                        p->p_stopped = 0;
                        if (sig == SIGCONT)
                                p->p_lwpflags |= PL_CONTNOTIFY;
                        sv_broadcast(&p->p_rendezsv);
                }
        }
        if ((STOPDEFAULT & bit) != 0)
                cancel = sigbit(SIGCONT);
        p->p_sigpend &= ~cancel;
        for (struct lwp *l = p->p_lwps; l != NULL; l = l->l_next)
                l->l_sigpend &= ~cancel;
}

/* Whether some LWP of p waits for sig in sigwait. p_siglock is held. */
static int
sig_awaited(const struct proc *p, int sig)
{
        for (const struct lwp *l = p->p_lwps; l != NULL; l = l->l_next) {
                if ((l->l_sigwait & sigbit(sig)) != 0)
                        return 1;
        }
        return 0;
}

static void
sig_store_info(siginfo_t *dst, int sig, const siginfo_t *info)
{
        if (info != NULL)
                *dst = *info;
        else
                memset(dst, 0, sizeof(*dst));
        dst->si_signo = sig;
}

/*
 * Post sig to p, with info (or none). Callable from any context,
 * interrupts included. The signal goes to whichever LWP takes it first;
 * one waiting for it in sigwait is nudged, or else one not holding it.
 */
void
sigtoproc(struct proc *p, int sig, const siginfo_t *info)
{
        k_sigset_t bit = sigbit(sig);
        struct lwp *target = NULL;
        int s;

        if (sig <= 0 || sig > MAXSIG || (p->p_flags & P_SYSTEM) != 0)
                return;
        s = spin_lock(&p->p_siglock);
        if (p->p_lwps == NULL) {                /* exiting */
                spin_unlock(&p->p_siglock, s);
                return;
        }
        sig_jobctl(p, sig);
        if ((sig_ignored(p, sig) && !sig_awaited(p, sig)) ||
            (p->p_sigpend & bit) != 0) {
                sig_recalc_all(p);
                spin_unlock(&p->p_siglock, s);
                return;
        }
        p->p_sigpend |= bit;
        sig_store_info(&p->p_siginfo[sig], sig, info);
        sig_recalc_all(p);
        for (struct lwp *l = p->p_lwps; l != NULL; l = l->l_next) {
                if ((l->l_sigwait & bit) != 0) {
                        target = l;
                        break;
                }
                if (target == NULL && (l->l_sigmask & bit) == 0)
                        target = l;
        }
        if (target != NULL)
                sig_nudge_lwp(target);
        spin_unlock(&p->p_siglock, s);
}

/* Post sig to LWP l of p (faults, _lwp_kill). p_siglock is held. */
void
sigtolwp_locked(struct proc *p, struct lwp *l, int sig, const siginfo_t *info)
{
        k_sigset_t bit = sigbit(sig);

        sig_jobctl(p, sig);
        if ((sig_ignored(p, sig) && (l->l_sigwait & bit) == 0) ||
            (l->l_sigpend & bit) != 0) {
                sig_recalc_all(p);
                return;
        }
        l->l_sigpend |= bit;
        sig_store_info(&l->l_siginfo[sig], sig, info);
        sig_recalc_all(p);
        sig_nudge_lwp(l);
}

/* A signal from the kernel, with no particular information. */
void
psignal(struct proc *p, int sig)
{
        sigtoproc(p, sig, NULL);
}

/*
 * A fault in the current LWP raises sig for it. A fault cannot be held
 * off or ignored: if it would be, the default action is restored first.
 */
void
trapsignal(int sig, int code, uint64_t addr)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        siginfo_t si;
        int s;

        memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code = code;
        si.si_addr = addr;
        s = spin_lock(&p->p_siglock);
        if ((l->l_sigmask & sigbit(sig)) != 0 ||
            p->p_sigact[sig].ksa_handler == H_IGN) {
                l->l_sigmask &= ~sigbit(sig);
                p->p_sigact[sig].ksa_handler = H_DFL;
        }
        sigtolwp_locked(p, l, sig, &si);
        spin_unlock(&p->p_siglock, s);
}

/* Tell p's parent that p stopped (with sig) or continued (sig 0). */
static void
sig_tell_parent(struct proc *p, int sig)
{
        struct proc *pp;
        siginfo_t si;

        memset(&si, 0, sizeof(si));
        si.si_signo = SIGCHLD;
        si.si_code = sig != 0 ? CLD_STOPPED : CLD_CONTINUED;
        si.si_pid = p->p_pid;
        si.si_status = sig != 0 ? sig : SIGCONT;
        mutex_lock(&proctree_lock);
        p->p_wstop = sig != 0;
        p->p_wcont = sig == 0;
        pp = p->p_parent;
        sv_broadcast(&pp->p_waitsv);
        if ((pp->p_sigflags & PS_JCTL) != 0)
                sigtoproc(pp, SIGCHLD, &si);
        mutex_unlock(&proctree_lock);
}

/* Take sig from l's or the process's pending set. p_siglock is held. */
static void
sig_take(struct proc *p, struct lwp *l, int sig, siginfo_t *info)
{
        k_sigset_t bit = sigbit(sig);

        if ((l->l_sigpend & bit) != 0) {
                l->l_sigpend &= ~bit;
                *info = l->l_siginfo[sig];
        } else {
                p->p_sigpend &= ~bit;
                *info = p->p_siginfo[sig];
        }
        sig_recalc_all(p);
}

/*
 * After an interruptible sleep was ended: discard what needs no action
 * and return what the sleeper should do. 0: go on sleeping. 1: return
 * EINTR (a handler or a fatal default awaits). 2: return ERESTART, so
 * that the LWP can park, stop, or exit on its way out and reissue the
 * call afterwards.
 */
int
sig_sleep_check(void)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        int s = spin_lock(&p->p_siglock);

        for (;;) {
                k_sigset_t cand = (p->p_sigpend | l->l_sigpend) &
                    ~l->l_sigmask;
                const struct ksigaction *a;
                siginfo_t info;
                int sig;

                if ((l->l_flags & LW_DESTROY) != 0 || lwp_park_pending(p, l)) {
                        spin_unlock(&p->p_siglock, s);
                        return 2;
                }
                if (cand == 0) {
                        sig_recalc_lwp(p, l);
                        spin_unlock(&p->p_siglock, s);
                        return 0;
                }
                sig = sig_pick(cand);
                a = &p->p_sigact[sig];
                if (a->ksa_handler > H_HOLD || (a->ksa_handler == H_DFL &&
                    (IGNDEFAULT & sigbit(sig)) == 0)) {
                        spin_unlock(&p->p_siglock, s);
                        return (STOPDEFAULT & sigbit(sig)) != 0 &&
                            a->ksa_handler == H_DFL ? 2 : 1;
                }
                sig_take(p, l, sig, &info);
        }
}

/*
 * The error for a wait a signal ended: ERESTART if the signal about to
 * be handled was set up with SA_RESTART, or if there is none to handle
 * (the LWP was stopped, suspended, or parked); EINTR otherwise.
 */
int
sig_restart(int error)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        k_sigset_t cand;
        int s, sig;

        if (error != EINTR)
                return error;
        s = spin_lock(&p->p_siglock);
        cand = (p->p_sigpend | l->l_sigpend) & ~l->l_sigmask;
        if (cand == 0) {
                error = ERESTART;
        } else {
                sig = sig_pick(cand);
                if (p->p_sigact[sig].ksa_handler > H_HOLD &&
                    (p->p_sigact[sig].ksa_flags & SA_RESTART) != 0)
                        error = ERESTART;
        }
        spin_unlock(&p->p_siglock, s);
        return error;
}

static int
on_altstack(const struct lwp *l, uint64_t sp)
{
        return (l->l_altflags & SS_DISABLE) == 0 && sp > l->l_altsp &&
            sp <= l->l_altsp + l->l_altsize;
}

/*
 * Everything an LWP does on its way to user mode, once kt_sigpend says
 * there is something: exit if destroyed, park while it must, then act
 * on one signal. At most one handler frame is built per return; signals
 * still pending are taken at the next return.
 */
void
sig_userret(void)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;

        for (;;) {
                struct ksigaction act;
                k_sigset_t bit, restore, cand;
                siginfo_t info;
                int s, sig, onstack, insig;
                uint64_t retaddr;

                s = spin_lock(&p->p_siglock);
                if ((l->l_flags & LW_DESTROY) != 0) {
                        spin_unlock(&p->p_siglock, s);
                        lwp_exit();
                }
                if (lwp_park_pending(p, l)) {
                        s = lwp_park(p, l, s);
                        if ((p->p_lwpflags & PL_CONTNOTIFY) != 0) {
                                p->p_lwpflags &= ~PL_CONTNOTIFY;
                                spin_unlock(&p->p_siglock, s);
                                sig_tell_parent(p, 0);
                                continue;
                        }
                        sig_recalc_lwp(p, l);
                        spin_unlock(&p->p_siglock, s);
                        continue;
                }
                cand = (p->p_sigpend | l->l_sigpend) & ~l->l_sigmask;
                if (cand == 0) {
                        /* A sigsuspend whose signal needed no handler. */
                        if (l->l_sigsuspend) {
                                l->l_sigmask = l->l_sigoldmask;
                                l->l_sigsuspend = 0;
                        }
                        sig_recalc_lwp(p, l);
                        spin_unlock(&p->p_siglock, s);
                        return;
                }
                sig = sig_pick(cand);
                bit = sigbit(sig);
                sig_take(p, l, sig, &info);
                act = p->p_sigact[sig];
                if (act.ksa_handler == H_IGN || (act.ksa_handler == H_DFL &&
                    (IGNDEFAULT & bit) != 0)) {
                        spin_unlock(&p->p_siglock, s);
                        continue;
                }
                if (act.ksa_handler == H_DFL) {
                        if ((STOPDEFAULT & bit) != 0) {
                                /* The whole process stops. */
                                p->p_stopped = 1;
                                p->p_stopsig = sig;
                                sig_recalc_all(p);
                                sig_nudge_all(p);
                                spin_unlock(&p->p_siglock, s);
                                sig_tell_parent(p, sig);
                                continue;
                        }
                        spin_unlock(&p->p_siglock, s);
                        proc_exit(CLD_KILLED, sig);
                }

                /* Caught. */
                restore = l->l_sigsuspend ? l->l_sigoldmask : l->l_sigmask;
                l->l_sigsuspend = 0;
                l->l_sigmask |= act.ksa_mask |
                    ((act.ksa_flags & SA_NODEFER) != 0 ? 0 : bit);
                l->l_sigmask &= ~CANTMASK;
                if ((act.ksa_flags & SA_RESETHAND) != 0 &&
                    (NORESET & bit) == 0) {
                        p->p_sigact[sig].ksa_handler = H_DFL;
                        p->p_sigact[sig].ksa_flags &= ~SA_SIGINFO;
                }
                insig = on_altstack(l, l->l_tf->tf_rsp);
                onstack = (act.ksa_flags & SA_ONSTACK) != 0 &&
                    (l->l_altflags & SS_DISABLE) == 0 && !insig;
                retaddr = p->p_sigreturn;
                sig_recalc_lwp(p, l);
                spin_unlock(&p->p_siglock, s);
                if (md_sendsig(l, sig, (act.ksa_flags & SA_SIGINFO) != 0 ?
                    &info : NULL, act.ksa_handler, restore, onstack,
                    insig, retaddr) != 0) {
                        printf("pid %d (%s): no room for a signal frame; "
                            "killed\n", p->p_pid, p->p_comm);
                        proc_exit(CLD_KILLED, SIGSEGV);
                }
                return;
        }
}

/*
 * A child of p's is exiting (proctree_lock is held): send SIGCHLD.
 * Returns 1 if the parent takes no zombies (SIGCHLD ignored, or
 * SA_NOCLDWAIT), in which case the child is freed at once.
 */
int
sig_child_exit(struct proc *child, int code, int data)
{
        struct proc *pp = child->p_parent;
        siginfo_t si;
        int nowait;

        if ((pp->p_flags & P_SYSTEM) != 0)
                return 0;
        nowait = pp->p_sigact[SIGCHLD].ksa_handler == H_IGN ||
            (pp->p_sigflags & PS_NOCLDWAIT) != 0;
        memset(&si, 0, sizeof(si));
        si.si_signo = SIGCHLD;
        si.si_code = code;
        si.si_pid = child->p_pid;
        si.si_status = data;
        proc_zombie_times(child, &si);
        sigtoproc(pp, SIGCHLD, &si);
        return nowait;
}

/* exec: caught signals go back to default; see UnixWare's sigexec. */
void
sig_exec(struct proc *p)
{
        struct lwp *l = p->p_lwps;      /* the only one now */
        int s = spin_lock(&p->p_siglock);

        for (int sig = 1; sig <= MAXSIG; sig++) {
                struct ksigaction *a = &p->p_sigact[sig];

                if (a->ksa_handler > H_HOLD)
                        a->ksa_handler = H_DFL;
                a->ksa_flags &= ~(SA_ONSTACK | SA_RESETHAND | SA_NODEFER);
                if (a->ksa_handler == H_DFL &&
                    (IGNDEFAULT & sigbit(sig)) != 0) {
                        p->p_sigpend &= ~sigbit(sig);
                        l->l_sigpend &= ~sigbit(sig);
                }
        }
        l->l_altflags = SS_DISABLE;
        l->l_altsp = 0;
        l->l_altsize = 0;
        p->p_sigflags = 0;
        p->p_sigreturn = 0;
        sig_recalc_lwp(p, l);
        spin_unlock(&p->p_siglock, s);
}

/* --- Signal sets between user and kernel. --- */

static k_sigset_t
sigset_in(const sigset_t *us)
{
        return ((k_sigset_t)us->__sigbits[0] |
            (k_sigset_t)us->__sigbits[1] << 32) & SIG_ALL;
}

/* A mask from user mode, without what cannot be held. */
k_sigset_t
sig_mask_from_user(const sigset_t *us)
{
        return sigset_in(us) & ~CANTMASK;
}

static void
sigset_out(k_sigset_t ks, sigset_t *us)
{
        memset(us, 0, sizeof(*us));
        us->__sigbits[0] = (uint32_t)ks;
        us->__sigbits[1] = (uint32_t)(ks >> 32);
}

static int
copyin_sigset(uint64_t uaddr, k_sigset_t *ksp)
{
        sigset_t us;
        int error = copyin((const void *)(uintptr_t)uaddr, &us, sizeof(us));

        if (error == 0)
                *ksp = sigset_in(&us);
        return error;
}

static int
copyout_sigset(k_sigset_t ks, uint64_t uaddr)
{
        sigset_t us;

        sigset_out(ks, &us);
        return copyout(&us, (void *)(uintptr_t)uaddr, sizeof(us));
}

/* --- System calls. --- */

/*
 * Whether a process with credentials cr, in session sid, may signal t
 * with sig: the superuser may signal anyone; others need their real or
 * effective user id to be t's real or saved one, except that SIGCONT
 * may go to any process of the same session (System V).
 */
static int
sig_permitted(struct cred *cr, pid_t sid, struct proc *t, int sig)
{
        struct cred *tcr;
        int ok;

        if (CRED_ISROOT(cr))
                return 1;
        tcr = cred_getproc(t);
        ok = cr->cr_ruid == tcr->cr_ruid || cr->cr_uid == tcr->cr_ruid ||
            cr->cr_ruid == tcr->cr_suid || cr->cr_uid == tcr->cr_suid;
        crfree(tcr);
        return ok || (sig == SIGCONT && t->p_sid == sid);
}

struct killarg {
        int             ka_all;
        pid_t           ka_pgid;
        int             ka_sig;
        const siginfo_t *ka_si;
        struct cred     *ka_cred;
        pid_t           ka_sid;
        int             ka_found;       /* a process matched */
        int             ka_sent;        /* one was allowed */
};

static void
kill_one(struct proc *t, void *arg)
{
        struct killarg *ka = arg;

        /* kill(-1, sig) leaves init alone, as System V does. */
        if (t->p_stat == SZOMB || t->p_stat == SIDL ||
            (!ka->ka_all && t->p_pgid != ka->ka_pgid) ||
            (ka->ka_all && t == initproc))
                return;
        ka->ka_found = 1;
        if (!sig_permitted(ka->ka_cred, ka->ka_sid, t, ka->ka_sig))
                return;
        ka->ka_sent = 1;
        if (ka->ka_sig != 0)
                sigtoproc(t, ka->ka_sig, ka->ka_si);
}

/*
 * Send sig to every process matching (all, or process group pgid) that
 * the caller may signal: ESRCH if none matched, EPERM if none could be.
 */
static int
kill_group(int all, pid_t pgid, int sig, const siginfo_t *si, struct cred *cr)
{
        struct killarg ka = { all, pgid, sig, si, cr, 0, 0, 0 };

        mutex_lock(&proctree_lock);
        ka.ka_sid = curproc->p_sid;
        proc_iterate_locked(kill_one, &ka);
        mutex_unlock(&proctree_lock);
        return !ka.ka_found ? ESRCH : !ka.ka_sent ? EPERM : 0;
}

struct pgfind {
        pid_t           pf_pgid;
        pid_t           pf_sid;
        int             pf_found;
};

static void
pgid_one(struct proc *t, void *arg)
{
        struct pgfind *pf = arg;

        if (t->p_pgid == pf->pf_pgid && t->p_sid == pf->pf_sid)
                pf->pf_found = 1;
}

/* Whether process group pgid exists in session sid. */
int
pgrp_in_session(pid_t pgid, pid_t sid)
{
        struct pgfind pf = { pgid, sid, 0 };

        mutex_lock(&proctree_lock);
        proc_iterate_locked(pgid_one, &pf);
        mutex_unlock(&proctree_lock);
        return pf.pf_found;
}

/*
 * Whether process group pgid is orphaned: no live member has a parent in
 * another group of the same session (POSIX). One with no live members
 * counts as orphaned. With stopped, whether a live member is stopped.
 */
struct orphan {
        pid_t   o_pgid;
        int     o_tied;         /* a member's parent keeps it */
        int     o_members;
        int     o_stopped;
};

static void
orphan_one(struct proc *t, void *arg)
{
        struct orphan *o = arg;
        struct proc *pp = t->p_parent;

        if (t->p_pgid != o->o_pgid || t->p_stat == SZOMB ||
            t->p_stat == SIDL)
                return;
        o->o_members++;
        if (t->p_stopped)
                o->o_stopped = 1;
        if (pp != NULL && pp->p_pgid != t->p_pgid && pp->p_sid == t->p_sid)
                o->o_tied = 1;
}

int
pgrp_orphaned(pid_t pgid, int *stopped)
{
        struct orphan o = { pgid, 0, 0, 0 };

        mutex_lock(&proctree_lock);
        proc_iterate_locked(orphan_one, &o);
        mutex_unlock(&proctree_lock);
        if (stopped != NULL)
                *stopped = o.o_stopped;
        return !o.o_tied;
}

/*
 * A process left group pgid, or its parent did: if that orphaned the
 * group and a member is stopped, nothing could ever continue it, so
 * each member gets SIGHUP and then SIGCONT (POSIX).
 */
void
pgrp_orphan_check(pid_t pgid)
{
        int stopped;

        if (pgid > 0 && pgrp_orphaned(pgid, &stopped) && stopped) {
                pgsignal(pgid, SIGHUP);
                pgsignal(pgid, SIGCONT);
        }
}

/* Whether the calling LWP holds sig, or its process ignores it. */
int
sig_held_or_ignored(int sig)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        int s = spin_lock(&p->p_siglock);
        int r = (l->l_sigmask & sigbit(sig)) != 0 ||
            p->p_sigact[sig].ksa_handler == H_IGN;

        spin_unlock(&p->p_siglock, s);
        return r;
}

/* A signal from the kernel to every process of group pgid (a terminal). */
void
pgsignal(pid_t pgid, int sig)
{
        siginfo_t si;

        memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code = SI_USER;
        (void)kill_group(0, pgid, sig, &si, kcred);
}

/* Send to a single positive PID using the caller's credentials. */
static int
kill_pid(pid_t pid, int sig, const siginfo_t *si)
{
        struct proc *t;
        int error = 0;

        mutex_lock(&proctree_lock);
        t = proc_find_locked(pid);
        if (t == NULL || t->p_stat == SZOMB || (t->p_flags & P_SYSTEM) != 0)
                error = ESRCH;
        else if (!sig_permitted(CRED(), curproc->p_sid, t, sig))
                error = EPERM;
        else if (sig != 0)
                sigtoproc(t, sig, si);
        mutex_unlock(&proctree_lock);
        return error;
}

/* kill(pid, sig) */
int
sys_kill(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        pid_t pid = (pid_t)args[0];
        int sig = (int)args[1];
        siginfo_t si;

        if (sig < 0 || sig > MAXSIG)
                return EINVAL;
        memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code = SI_USER;
        si.si_pid = p->p_pid;
        si.si_uid = (int32_t)CRED()->cr_ruid;
        if (pid == 0)
                return kill_group(0, p->p_pgid, sig, &si, CRED());
        if (pid == -1)
                return kill_group(1, 0, sig, &si, CRED());
        if (pid < -1)
                return kill_group(0, -pid, sig, &si, CRED());
        return kill_pid(pid, sig, &si);
}

/* sigqueue(pid, sig, &value): standard signals keep the first posting. */
int
sys_sigqueue(const uint64_t *args, uint64_t *rv __unused)
{
        pid_t pid = (pid_t)args[0];
        int sig = (int)args[1];
        siginfo_t si;
        int error;

        if (sig < 0 || sig > MAXSIG)
                return EINVAL;
        if (pid <= 0)
                return ESRCH;
        if (sig == 0)
                return kill_pid(pid, 0, NULL);
        memset(&si, 0, sizeof(si));
        error = copyin((const void *)(uintptr_t)args[2], &si.si_value,
            sizeof(si.si_value));
        if (error != 0)
                return error;
        si.si_code = SI_QUEUE;
        si.si_pid = curproc->p_pid;
        si.si_uid = (int32_t)CRED()->cr_ruid;
        return kill_pid(pid, sig, &si);
}

/*
 * Discard pending instances of sig after its action became ignore.
 * p_siglock is held.
 */
static void
sig_discard_if_ignored(struct proc *p, int sig)
{
        if (sig_ignored(p, sig)) {
                p->p_sigpend &= ~sigbit(sig);
                for (struct lwp *l = p->p_lwps; l != NULL; l = l->l_next)
                        l->l_sigpend &= ~sigbit(sig);
                sig_recalc_all(p);
        }
}

/* Free p's zombie children (SA_NOCLDWAIT set). */
static void
free_zombies(struct proc *p)
{
        for (;;) {
                struct proc *c;

                mutex_lock(&proctree_lock);
                for (c = p->p_children; c != NULL; c = c->p_sibling) {
                        if (c->p_stat == SZOMB)
                                break;
                }
                if (c == NULL) {
                        mutex_unlock(&proctree_lock);
                        return;
                }
                proc_reap_locked(c);
        }
}

/* sigaction(sig, act, oact, sigreturn): UnixWare's four arguments. */
int
sys_sigaction(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        int sig = (int)args[0];
        struct sigaction ua;
        struct ksigaction old;
        int oflags, s, error, nocldwait = 0;

        if (sig <= 0 || sig > MAXSIG)
                return EINVAL;
        if (args[1] != 0) {
                if ((error = copyin((const void *)(uintptr_t)args[1], &ua,
                    sizeof(ua))) != 0)
                        return error;
                if ((sigbit(sig) & CANTMASK) != 0 &&
                    (uint64_t)(uintptr_t)ua.sa_handler != H_DFL)
                        return EINVAL;
                if ((uint64_t)(uintptr_t)ua.sa_handler == H_HOLD ||
                    (uint64_t)(uintptr_t)ua.sa_handler >= VM_USER_MAX)
                        return EINVAL;
        }
        s = spin_lock(&p->p_siglock);
        old = p->p_sigact[sig];
        oflags = old.ksa_flags;
        if (sig == SIGCHLD) {
                oflags &= ~(SA_NOCLDSTOP | SA_NOCLDWAIT);
                if ((p->p_sigflags & PS_JCTL) == 0)
                        oflags |= SA_NOCLDSTOP;
                if ((p->p_sigflags & PS_NOCLDWAIT) != 0)
                        oflags |= SA_NOCLDWAIT;
        }
        if (args[1] != 0) {
                struct ksigaction *a = &p->p_sigact[sig];

                a->ksa_handler = (uint64_t)(uintptr_t)ua.sa_handler;
                a->ksa_flags = ua.sa_flags & (SA_ONSTACK | SA_RESETHAND |
                    SA_RESTART | SA_SIGINFO | SA_NODEFER);
                a->ksa_mask = sigset_in(&ua.sa_mask) & ~CANTMASK;
                if (sig == SIGCHLD) {
                        if ((ua.sa_flags & SA_NOCLDSTOP) != 0)
                                p->p_sigflags &= ~PS_JCTL;
                        else
                                p->p_sigflags |= PS_JCTL;
                        if ((ua.sa_flags & SA_NOCLDWAIT) != 0) {
                                p->p_sigflags |= PS_NOCLDWAIT;
                                nocldwait = 1;
                        } else {
                                p->p_sigflags &= ~PS_NOCLDWAIT;
                        }
                }
                sig_discard_if_ignored(p, sig);
        }
        if (args[3] != 0)
                p->p_sigreturn = args[3];
        spin_unlock(&p->p_siglock, s);
        if (nocldwait || (sig == SIGCHLD && args[1] != 0 &&
            (uint64_t)(uintptr_t)ua.sa_handler == H_IGN))
                free_zombies(p);
        if (args[2] != 0) {
                memset(&ua, 0, sizeof(ua));
                ua.sa_handler = (void (*)(int))(uintptr_t)old.ksa_handler;
                ua.sa_flags = oflags;
                sigset_out(old.ksa_mask, &ua.sa_mask);
                return copyout(&ua, (void *)(uintptr_t)args[2], sizeof(ua));
        }
        return 0;
}

/* sigprocmask(how, set, oset) */
int
sys_sigprocmask(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        k_sigset_t set = 0, old;
        int how = (int)args[0], s, error;

        if (args[1] != 0 && (error = copyin_sigset(args[1], &set)) != 0)
                return error;
        s = spin_lock(&p->p_siglock);
        old = l->l_sigmask;
        if (args[1] != 0) {
                switch (how) {
                case SIG_BLOCK:
                        l->l_sigmask |= set;
                        break;
                case SIG_UNBLOCK:
                        l->l_sigmask &= ~set;
                        break;
                case SIG_SETMASK:
                        l->l_sigmask = set;
                        break;
                default:
                        spin_unlock(&p->p_siglock, s);
                        return EINVAL;
                }
                l->l_sigmask &= ~CANTMASK;
                sig_recalc_lwp(p, curlwp);
        }
        spin_unlock(&p->p_siglock, s);
        return args[2] != 0 ? copyout_sigset(old, args[2]) : 0;
}

/* Sleep until a signal must reach user mode. Always EINTR. */
static int
sig_pause(void)
{
        struct proc *p = curproc;

        for (;;) {
                int s = spin_lock(&p->p_siglock), r;

                (void)sv_wait_sig(&p->p_pausesv, &p->p_siglock, s);
                if ((r = sig_sleep_check()) != 0)
                        return r == 1 ? EINTR : ERESTART;
        }
}

int
sys_pause(const uint64_t *args __unused, uint64_t *rv __unused)
{
        return sig_pause();
}

/* Hold mask while waiting; the old one comes back after the handler. */
static int
sig_suspend(k_sigset_t mask)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        int s = spin_lock(&p->p_siglock);

        /* A reissued sigsuspend keeps the mask the first one saved. */
        if (!l->l_sigsuspend)
                l->l_sigoldmask = l->l_sigmask;
        l->l_sigsuspend = 1;
        l->l_sigmask = mask & ~CANTMASK;
        sig_recalc_lwp(p, curlwp);
        spin_unlock(&p->p_siglock, s);
        return sig_pause();
}

/*
 * For a wait that a signal may end with another mask held for it
 * (pollsys): hold the mask at umask, as sigsuspend does, and keep the
 * old one to come back.
 */
int
sig_wait_mask(uint64_t umask)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        k_sigset_t mask;
        int s, error;

        if ((error = copyin_sigset(umask, &mask)) != 0)
                return error;
        s = spin_lock(&p->p_siglock);
        if (!l->l_sigsuspend)
                l->l_sigoldmask = l->l_sigmask;
        l->l_sigsuspend = 1;
        l->l_sigmask = mask & ~CANTMASK;
        sig_recalc_lwp(p, l);
        spin_unlock(&p->p_siglock, s);
        return 0;
}

/*
 * The wait is over, with error. After EINTR or ERESTART a signal is
 * due, and its delivery puts the old mask back (after the handler, if
 * there is one); otherwise it comes back now.
 */
void
sig_wait_mask_end(int error)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        int s;

        if (error == EINTR || error == ERESTART)
                return;
        s = spin_lock(&p->p_siglock);
        if (l->l_sigsuspend) {
                l->l_sigmask = l->l_sigoldmask;
                l->l_sigsuspend = 0;
        }
        sig_recalc_lwp(p, l);
        spin_unlock(&p->p_siglock, s);
}

int
sys_sigsuspend(const uint64_t *args, uint64_t *rv __unused)
{
        k_sigset_t mask;
        int error;

        if ((error = copyin_sigset(args[0], &mask)) != 0)
                return error;
        return sig_suspend(mask);
}

/*
 * Timed interruptible waits. A callout sets l_timedout and nudges the
 * LWP, which ends its sleep as a pending signal would (sig_recalc_lwp
 * counts l_timedout), so a timeout that fires just before the LWP goes
 * to sleep is not missed. The waiter checks l_timedout after each
 * sleep, and must stop the timeout before it returns.
 */
static void
lwp_timeout_fire(void *arg)
{
        struct lwp *l = arg;
        struct proc *p = l->l_proc;
        int s = spin_lock(&p->p_siglock);

        l->l_timedout = 1;
        sig_recalc_lwp(p, l);
        sig_nudge_lwp(l);
        spin_unlock(&p->p_siglock, s);
}

/*
 * Copy in a relative timeout (a timespec of two int64_t) and convert
 * it to clock ticks: rounded up, and one more, since a callout set for
 * n ticks runs at the n-th tick boundary, which may be all but a tick
 * sooner, and the wait must last at least the time given. A zero
 * timeout stays zero: no wait.
 */
int
lwp_timeout_ticks(uint64_t uts, long *ticks)
{
        int64_t ts[2];
        int error;

        if ((error = copyin((const void *)(uintptr_t)uts, ts,
            sizeof(ts))) != 0)
                return error;
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000)
                return EINVAL;
        if (ts[0] > INT32_MAX)
                ts[0] = INT32_MAX;
        *ticks = (long)(ts[0] * HZ + (ts[1] * HZ + 999999999) / 1000000000);
        if (*ticks > 0)
                (*ticks)++;
        return 0;
}

/*
 * Start timing the calling LWP's wait: after ticks ticks (none if
 * ticks <= 0) it sees l_timedout. Returns the callout's id, or 0.
 */
int64_t
lwp_timeout_start(long ticks)
{
        struct lwp *l = curlwp;
        int s = spin_lock(&l->l_proc->p_siglock);

        l->l_timedout = 0;
        spin_unlock(&l->l_proc->p_siglock, s);
        return ticks > 0 ? timeout(lwp_timeout_fire, l, ticks) : 0;
}

/*
 * End the timed wait: cancel the callout, waiting for it if it is
 * running, and only then clear l_timedout, which a late callout would
 * otherwise leave set, cutting short every later sleep. Called without
 * p_siglock.
 */
void
lwp_timeout_stop(int64_t id)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        int s;

        if (id != 0)
                (void)untimeout(id);
        s = spin_lock(&p->p_siglock);
        l->l_timedout = 0;
        sig_recalc_lwp(p, l);
        spin_unlock(&p->p_siglock, s);
}

/*
 * sigtimedwait(set, siginfo_t *, timeout), UnixWare's sigwait (194):
 * take a pending signal of set, waiting for one up to timeout (none:
 * forever). Returns the signal; EAGAIN if the time ran out.
 */
int
sys_sigwait(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        k_sigset_t set;
        siginfo_t info;
        long ticks = -1;
        int64_t id;
        int s, sig = 0, error, r;

        if ((error = copyin_sigset(args[0], &set)) != 0)
                return error;
        set &= ~CANTMASK;
        if (set == 0)
                return EINVAL;
        if (args[2] != 0 && (error = lwp_timeout_ticks(args[2], &ticks)) != 0)
                return error;
        id = lwp_timeout_start(ticks);
        s = spin_lock(&p->p_siglock);
        l->l_sigwait = set;
        for (;;) {
                k_sigset_t cand = (p->p_sigpend | l->l_sigpend) & set;

                if (cand != 0) {
                        sig = sig_pick(cand);
                        sig_take(p, l, sig, &info);
                        break;
                }
                if (ticks == 0 || l->l_timedout) {
                        error = EAGAIN;
                        break;
                }
                (void)sv_wait_sig(&p->p_pausesv, &p->p_siglock, s);
                s = spin_lock(&p->p_siglock);
                if (((p->p_sigpend | l->l_sigpend) & set) != 0 ||
                    l->l_timedout)
                        continue;
                spin_unlock(&p->p_siglock, s);
                r = sig_sleep_check();
                s = spin_lock(&p->p_siglock);
                if (r != 0) {
                        error = r == 1 ? EINTR : ERESTART;
                        break;
                }
        }
        l->l_sigwait = 0;
        sig_recalc_lwp(p, l);
        spin_unlock(&p->p_siglock, s);
        lwp_timeout_stop(id);
        if (error != 0)
                return error;
        rv[0] = (uint64_t)sig;
        return args[1] != 0 ? copyout(&info, (void *)(uintptr_t)args[1],
            sizeof(info)) : 0;
}

/* sigpending(cmd, set): the pending set, or the set of valid signals. */
int
sys_sigpending(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        k_sigset_t set;
        int s;

        switch ((int)args[0]) {
        case SIGPENDING_PENDING:
                s = spin_lock(&p->p_siglock);
                set = (p->p_sigpend | curlwp->l_sigpend) &
                    curlwp->l_sigmask;
                spin_unlock(&p->p_siglock, s);
                break;
        case SIGPENDING_FILLSET:
                set = SIG_ALL;
                break;
        default:
                return EINVAL;
        }
        return copyout_sigset(set, args[1]);
}

/* sigaltstack(ss, oss) */
int
sys_sigaltstack(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        stack_t ss, oss;
        int s, error, onstack;

        if (args[0] != 0) {
                if ((error = copyin((const void *)(uintptr_t)args[0], &ss,
                    sizeof(ss))) != 0)
                        return error;
                if ((ss.ss_flags & ~SS_DISABLE) != 0)
                        return EINVAL;
                if ((ss.ss_flags & SS_DISABLE) == 0 &&
                    (ss.ss_size < MINSIGSTKSZ ||
                    (uint64_t)(uintptr_t)ss.ss_sp >= VM_USER_MAX))
                        return ss.ss_size < MINSIGSTKSZ ? ENOMEM : EINVAL;
        }
        s = spin_lock(&p->p_siglock);
        onstack = on_altstack(l, l->l_tf->tf_rsp);
        memset(&oss, 0, sizeof(oss));
        oss.ss_sp = (void *)(uintptr_t)l->l_altsp;
        oss.ss_size = l->l_altsize;
        oss.ss_flags = onstack ? SS_ONSTACK : (l->l_altflags & SS_DISABLE);
        if (args[0] != 0) {
                if (onstack) {
                        spin_unlock(&p->p_siglock, s);
                        return EPERM;
                }
                l->l_altflags = ss.ss_flags & SS_DISABLE;
                l->l_altsp = (uint64_t)(uintptr_t)ss.ss_sp;
                l->l_altsize = ss.ss_size;
        }
        spin_unlock(&p->p_siglock, s);
        return args[1] != 0 ? copyout(&oss, (void *)(uintptr_t)args[1],
            sizeof(oss)) : 0;
}

/*
 * signal(48): UnixWare's multiplexed old interfaces. The low byte is
 * the signal; SIGDEFER is sigset, SIGHOLD sighold, SIGRELSE sigrelse,
 * SIGIGNORE sigignore, SIGPAUSE sigpause, and none of them signal.
 * signal() handlers are reset on delivery and not deferred; sigset()
 * handlers hold the signal while they run.
 */
int
sys_signal(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        int sig = (int)(args[0] & SIGNO_MASK);
        int cmd = (int)(args[0] & ~(uint64_t)SIGNO_MASK);
        uint64_t func = args[1];
        k_sigset_t bit;
        int s;

        if (sig <= 0 || sig > MAXSIG)
                return EINVAL;
        bit = sigbit(sig);
        if ((bit & CANTMASK) != 0 && (cmd != 0 || func != H_DFL) &&
            cmd != SIGRELSE && cmd != SIGPAUSE)
                return EINVAL;
        if (cmd == 0 || cmd == SIGDEFER) {
                if (func >= VM_USER_MAX || (cmd == 0 && func == H_HOLD))
                        return EINVAL;
        }
        if (cmd == SIGPAUSE) {
                k_sigset_t mask;

                s = spin_lock(&p->p_siglock);
                mask = l->l_sigmask & ~bit;
                spin_unlock(&p->p_siglock, s);
                return sig_suspend(mask);
        }
        s = spin_lock(&p->p_siglock);
        switch (cmd) {
        case 0:
        case SIGDEFER:
                rv[0] = (l->l_sigmask & bit) != 0 && cmd == SIGDEFER ?
                    H_HOLD : p->p_sigact[sig].ksa_handler;
                if (cmd == SIGDEFER && func == H_HOLD) {
                        l->l_sigmask |= bit;
                        break;
                }
                p->p_sigact[sig].ksa_handler = func;
                p->p_sigact[sig].ksa_mask = 0;
                p->p_sigact[sig].ksa_flags = cmd == 0 ?
                    SA_RESETHAND | SA_NODEFER : 0;
                if (cmd == SIGDEFER)
                        l->l_sigmask &= ~bit;
                if (sig == SIGCHLD)
                        p->p_sigflags |= PS_JCTL;
                sig_discard_if_ignored(p, sig);
                break;
        case SIGHOLD:
                l->l_sigmask |= bit;
                break;
        case SIGRELSE:
                l->l_sigmask &= ~bit;
                break;
        case SIGIGNORE:
                p->p_sigact[sig].ksa_handler = H_IGN;
                sig_discard_if_ignored(p, sig);
                break;
        default:
                spin_unlock(&p->p_siglock, s);
                return EINVAL;
        }
        l->l_sigmask &= ~CANTMASK;
        sig_recalc_lwp(p, curlwp);
        spin_unlock(&p->p_siglock, s);
        return 0;
}

/* context(cmd, ucp): getcontext and setcontext. */
int
sys_context(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        ucontext_t *uc = kmem_zalloc(sizeof(*uc), KM_SLEEP);
        int s, error;

        switch ((int)args[0]) {
        case GETCONTEXT:
                md_getcontext(l, uc);
                s = spin_lock(&p->p_siglock);
                sigset_out(l->l_sigmask, &uc->uc_sigmask);
                uc->uc_stack.ss_sp = (void *)(uintptr_t)l->l_altsp;
                uc->uc_stack.ss_size = l->l_altsize;
                uc->uc_stack.ss_flags = on_altstack(l, l->l_tf->tf_rsp) ?
                    SS_ONSTACK : (l->l_altflags & SS_DISABLE);
                spin_unlock(&p->p_siglock, s);
                uc->uc_flags = UC_ALL;
                error = copyout(uc, (void *)(uintptr_t)args[1], sizeof(*uc));
                break;
        case SETCONTEXT:
                if ((error = copyin((const void *)(uintptr_t)args[1], uc,
                    sizeof(*uc))) != 0)
                        break;
                if ((uc->uc_flags & UC_MCONTEXT) != 0 &&
                    (error = md_setcontext(l, uc)) != 0)
                        break;
                if ((uc->uc_flags & UC_SIGMASK) != 0) {
                        s = spin_lock(&p->p_siglock);
                        l->l_sigmask = sigset_in(&uc->uc_sigmask) &
                            ~CANTMASK;
                        sig_recalc_lwp(p, curlwp);
                        spin_unlock(&p->p_siglock, s);
                }
                error = (uc->uc_flags & UC_MCONTEXT) != 0 ? EJUSTRETURN : 0;
                break;
        default:
                error = EINVAL;
                break;
        }
        kmem_free(uc, sizeof(*uc));
        return error;
}

/*
 * pgrpsys(cmd, pid, pgid), as in System V: 0 getpgrp, 1 setpgrp,
 * 2 getsid, 3 setsid, 4 getpgid, 5 setpgid.
 */
int
sys_pgrpsys(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc, *t;
        int cmd = (int)args[0];
        pid_t pid = (pid_t)args[1], pgid = (pid_t)args[2];
        int error = 0;

        mutex_lock(&proctree_lock);
        switch (cmd) {
        case 0:
                rv[0] = (uint64_t)p->p_pgid;
                break;
        case 1:
                if (p->p_sid != p->p_pid)
                        p->p_sid = p->p_pgid = p->p_pid;
                rv[0] = (uint64_t)p->p_pgid;
                break;
        case 2:
        case 4:
                t = pid == 0 ? p : proc_find_locked(pid);
                if (t == NULL || (t->p_flags & P_SYSTEM) != 0)
                        error = ESRCH;
                else
                        rv[0] = (uint64_t)(cmd == 2 ? t->p_sid : t->p_pgid);
                break;
        case 3:
                if (p->p_pgid == p->p_pid) {
                        error = EPERM;
                        break;
                }
                p->p_sid = p->p_pgid = p->p_pid;
                rv[0] = (uint64_t)p->p_pid;
                break;
        case 5:
                t = pid == 0 ? p : proc_find_locked(pid);
                if (t == NULL || (t != p && t->p_parent != p)) {
                        error = ESRCH;
                        break;
                }
                if (pgid == 0)
                        pgid = t->p_pid;
                if (t->p_sid == t->p_pid || t->p_sid != p->p_sid ||
                    pgid < 0) {
                        error = EPERM;
                        break;
                }
                if (pgid != t->p_pid) {
                        struct pgfind pf = { pgid, p->p_sid, 0 };

                        proc_iterate_locked(pgid_one, &pf);
                        if (!pf.pf_found) {
                                error = EPERM;
                                break;
                        }
                }
                t->p_pgid = pgid;
                break;
        default:
                error = EINVAL;
                break;
        }
        mutex_unlock(&proctree_lock);
        return error;
}
