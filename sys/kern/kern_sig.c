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

/* Whether the LWP has a signal to look at. p_siglock is held. */
static void
sig_recalc(struct proc *p)
{
        struct lwp *l = p->p_lwp;

        if (l != NULL && l->l_kthread != NULL)
                l->l_kthread->kt_sigpend =
                    (p->p_sigpend & ~l->l_sigmask) != 0;
}

/* Make the LWP look at its signals soon. p_siglock is held. */
static void
sig_nudge(struct proc *p)
{
        struct kthread *kt;

        if (p->p_lwp == NULL || (kt = p->p_lwp->l_kthread) == NULL ||
            !kt->kt_sigpend)
                return;
        sleep_interrupt(kt);
        if (kt->kt_state == KT_ONPROC && kt->kt_cpu != NULL &&
            kt->kt_cpu != curcpu())
                cpu_need_resched(kt->kt_cpu);
}

/*
 * Post sig to p, with info (or none). Callable from any context,
 * interrupts included.
 */
void
sigtoproc(struct proc *p, int sig, const siginfo_t *info)
{
        k_sigset_t bit = sigbit(sig);
        int s;

        if (sig <= 0 || sig > MAXSIG || (p->p_flags & P_SYSTEM) != 0)
                return;
        s = spin_lock(&p->p_siglock);
        if (p->p_lwp == NULL) {                 /* exiting */
                spin_unlock(&p->p_siglock, s);
                return;
        }
        if (sig == SIGCONT || sig == SIGKILL) {
                p->p_sigpend &= ~STOPDEFAULT;
                if (p->p_stopped) {
                        p->p_stopped = 0;
                        sv_broadcast(&p->p_stopsv);
                }
        }
        if ((STOPDEFAULT & bit) != 0)
                p->p_sigpend &= ~sigbit(SIGCONT);
        if (sig_ignored(p, sig) || (p->p_sigpend & bit) != 0) {
                spin_unlock(&p->p_siglock, s);
                return;
        }
        p->p_sigpend |= bit;
        if (info != NULL) {
                p->p_siginfo[sig] = *info;
        } else {
                memset(&p->p_siginfo[sig], 0, sizeof(siginfo_t));
                p->p_siginfo[sig].si_signo = sig;
        }
        p->p_siginfo[sig].si_signo = sig;
        sig_recalc(p);
        sig_nudge(p);
        spin_unlock(&p->p_siglock, s);
}

/* A signal from the kernel, with no particular information. */
void
psignal(struct proc *p, int sig)
{
        sigtoproc(p, sig, NULL);
}

/*
 * A fault in the current LWP raises sig. A fault cannot be held off or
 * ignored: if it would be, the default action is restored first.
 */
void
trapsignal(int sig, int code, uint64_t addr)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;
        siginfo_t si;
        int s;

        s = spin_lock(&p->p_siglock);
        if ((l->l_sigmask & sigbit(sig)) != 0 ||
            p->p_sigact[sig].ksa_handler == H_IGN) {
                l->l_sigmask &= ~sigbit(sig);
                p->p_sigact[sig].ksa_handler = H_DFL;
        }
        spin_unlock(&p->p_siglock, s);
        memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code = code;
        si.si_addr = addr;
        sigtoproc(p, sig, &si);
}

/*
 * Stop the calling process for sig, which it has just taken (p_stopped
 * is set), and wait to be continued or killed. No locks are held.
 */
static void
sig_stop(struct proc *p, int sig)
{
        struct proc *pp;
        siginfo_t si;
        int s, killed;

        memset(&si, 0, sizeof(si));
        si.si_signo = SIGCHLD;
        si.si_code = CLD_STOPPED;
        si.si_pid = p->p_pid;
        si.si_status = sig;
        mutex_lock(&proctree_lock);
        p->p_wstop = 1;
        p->p_wcont = 0;
        pp = p->p_parent;
        sv_broadcast(&pp->p_waitsv);
        if ((pp->p_sigflags & PS_JCTL) != 0)
                sigtoproc(pp, SIGCHLD, &si);
        mutex_unlock(&proctree_lock);

        s = spin_lock(&p->p_siglock);
        while (p->p_stopped) {
                sv_wait(&p->p_stopsv, &p->p_siglock, s);
                s = spin_lock(&p->p_siglock);
        }
        killed = (p->p_sigpend & sigbit(SIGKILL)) != 0;
        spin_unlock(&p->p_siglock, s);
        if (killed)
                return;

        /* Continued: tell the parent, as for a stop. */
        si.si_code = CLD_CONTINUED;
        si.si_status = SIGCONT;
        mutex_lock(&proctree_lock);
        p->p_wstop = 0;
        p->p_wcont = 1;
        pp = p->p_parent;
        sv_broadcast(&pp->p_waitsv);
        if ((pp->p_sigflags & PS_JCTL) != 0)
                sigtoproc(pp, SIGCHLD, &si);
        mutex_unlock(&proctree_lock);
}

/*
 * After an interruptible sleep was ended: take any pending stops and
 * discards here, and return 1 if a signal remains that must reach user
 * mode (a handler, or a fatal default), 0 if the sleep should go on.
 */
int
sig_sleep_check(void)
{
        struct proc *p = curproc;
        struct lwp *l = curlwp;

        for (;;) {
                int s = spin_lock(&p->p_siglock);
                k_sigset_t cand = p->p_sigpend & ~l->l_sigmask;
                const struct ksigaction *a;
                int sig;

                if (cand == 0) {
                        sig_recalc(p);
                        spin_unlock(&p->p_siglock, s);
                        return 0;
                }
                sig = sig_pick(cand);
                a = &p->p_sigact[sig];
                if (a->ksa_handler > H_HOLD || (a->ksa_handler == H_DFL &&
                    ((IGNDEFAULT | STOPDEFAULT) & sigbit(sig)) == 0)) {
                        spin_unlock(&p->p_siglock, s);
                        return 1;
                }
                p->p_sigpend &= ~sigbit(sig);
                sig_recalc(p);
                if (a->ksa_handler == H_DFL &&
                    (STOPDEFAULT & sigbit(sig)) != 0) {
                        p->p_stopped = 1;
                        p->p_stopsig = sig;
                        spin_unlock(&p->p_siglock, s);
                        sig_stop(p, sig);
                        continue;
                }
                spin_unlock(&p->p_siglock, s);
        }
}

/*
 * The error for a wait a signal ended: ERESTART if the signal about to
 * be handled was set up with SA_RESTART (the C library then reissues
 * the call), EINTR otherwise.
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
        cand = p->p_sigpend & ~l->l_sigmask;
        if (cand != 0) {
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
 * Act on the LWP's signals before it returns to user mode. At most one
 * handler frame is built per return; signals still pending are taken at
 * the next return, which the handler's own system calls provide.
 */
void
sig_userret(void)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;

        while (curthread->kt_sigpend) {
                struct ksigaction act;
                k_sigset_t bit, restore;
                siginfo_t info;
                int s, sig, onstack, insig;
                uint64_t retaddr;

                s = spin_lock(&p->p_siglock);
                if ((p->p_sigpend & ~l->l_sigmask) == 0) {
                        sig_recalc(p);
                        spin_unlock(&p->p_siglock, s);
                        break;
                }
                sig = sig_pick(p->p_sigpend & ~l->l_sigmask);
                bit = sigbit(sig);
                p->p_sigpend &= ~bit;
                info = p->p_siginfo[sig];
                act = p->p_sigact[sig];
                if (act.ksa_handler == H_IGN || (act.ksa_handler == H_DFL &&
                    (IGNDEFAULT & bit) != 0)) {
                        sig_recalc(p);
                        spin_unlock(&p->p_siglock, s);
                        continue;
                }
                if (act.ksa_handler == H_DFL) {
                        if ((STOPDEFAULT & bit) != 0) {
                                p->p_stopped = 1;
                                p->p_stopsig = sig;
                                sig_recalc(p);
                                spin_unlock(&p->p_siglock, s);
                                sig_stop(p, sig);
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
                sig_recalc(p);
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
        /* A sigsuspend whose signal turned out to need no handler. */
        if (l->l_sigsuspend) {
                int s = spin_lock(&p->p_siglock);

                if (l->l_sigsuspend) {
                        l->l_sigmask = l->l_sigoldmask;
                        l->l_sigsuspend = 0;
                        sig_recalc(p);
                }
                spin_unlock(&p->p_siglock, s);
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
        sigtoproc(pp, SIGCHLD, &si);
        return nowait;
}

/* exec: caught signals go back to default; see UnixWare's sigexec. */
void
sig_exec(struct proc *p)
{
        struct lwp *l = p->p_lwp;
        int s = spin_lock(&p->p_siglock);

        for (int sig = 1; sig <= MAXSIG; sig++) {
                struct ksigaction *a = &p->p_sigact[sig];

                if (a->ksa_handler > H_HOLD)
                        a->ksa_handler = H_DFL;
                a->ksa_flags &= ~(SA_ONSTACK | SA_RESETHAND | SA_NODEFER);
                if (a->ksa_handler == H_DFL && (IGNDEFAULT & sigbit(sig)) != 0)
                        p->p_sigpend &= ~sigbit(sig);
        }
        l->l_altflags = SS_DISABLE;
        l->l_altsp = 0;
        l->l_altsize = 0;
        p->p_sigflags = 0;
        p->p_sigreturn = 0;
        sig_recalc(p);
        spin_unlock(&p->p_siglock, s);
}

/* --- alarm --- */

static void
alarm_fire(void *arg)
{
        struct proc *p = arg;

        p->p_alarm = 0;
        psignal(p, SIGALRM);
}

/* Cancel p's alarm, returning the seconds that were left. */
static uint64_t
alarm_cancel(struct proc *p)
{
        toid_t id;
        uint64_t when;

        mutex_lock(&p->p_lock);
        id = p->p_alarm;
        when = p->p_alarm_when;
        p->p_alarm = 0;
        mutex_unlock(&p->p_lock);
        if (id == 0 || !untimeout(id) || when <= lbolt)
                return 0;
        return (when - lbolt + HZ - 1) / HZ;
}

void
sig_cancel_alarm(struct proc *p)
{
        (void)alarm_cancel(p);
}

int
sys_alarm(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        uint64_t secs = (uint32_t)args[0];

        rv[0] = alarm_cancel(p);
        if (secs != 0) {
                toid_t id = timeout(alarm_fire, p, (long)(secs * HZ));

                mutex_lock(&p->p_lock);
                p->p_alarm = id;
                p->p_alarm_when = lbolt + secs * HZ;
                mutex_unlock(&p->p_lock);
        }
        return 0;
}

/* --- Signal sets between user and kernel. --- */

static k_sigset_t
sigset_in(const sigset_t *us)
{
        return ((k_sigset_t)us->__sigbits[0] |
            (k_sigset_t)us->__sigbits[1] << 32) & SIG_ALL;
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

struct killarg {
        int             ka_all;
        pid_t           ka_pgid;
        int             ka_sig;
        const siginfo_t *ka_si;
        int             ka_found;
};

static void
kill_one(struct proc *t, void *arg)
{
        struct killarg *ka = arg;

        if (t->p_stat == SZOMB || t->p_stat == SIDL ||
            (!ka->ka_all && t->p_pgid != ka->ka_pgid))
                return;
        ka->ka_found = 1;
        if (ka->ka_sig != 0)
                sigtoproc(t, ka->ka_sig, ka->ka_si);
}

/* Send sig to every process matching (all, or process group pgid). */
static int
kill_group(int all, pid_t pgid, int sig, const siginfo_t *si)
{
        struct killarg ka = { all, pgid, sig, si, 0 };

        mutex_lock(&proctree_lock);
        proc_iterate_locked(kill_one, &ka);
        mutex_unlock(&proctree_lock);
        return ka.ka_found ? 0 : ESRCH;
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

/* kill(pid, sig) */
int
sys_kill(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        pid_t pid = (pid_t)args[0];
        int sig = (int)args[1];
        siginfo_t si;
        struct proc *t;

        if (sig < 0 || sig > MAXSIG)
                return EINVAL;
        memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code = SI_USER;
        si.si_pid = p->p_pid;
        si.si_uid = (int32_t)p->p_cred->cr_uid;
        if (pid == 0)
                return kill_group(0, p->p_pgid, sig, &si);
        if (pid == -1)
                return kill_group(1, 0, sig, &si);
        if (pid < -1)
                return kill_group(0, -pid, sig, &si);
        mutex_lock(&proctree_lock);
        t = proc_find_locked(pid);
        if (t == NULL || t->p_stat == SZOMB || (t->p_flags & P_SYSTEM) != 0) {
                mutex_unlock(&proctree_lock);
                return ESRCH;
        }
        if (sig != 0)
                sigtoproc(t, sig, &si);
        mutex_unlock(&proctree_lock);
        return 0;
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
                sig_recalc(p);
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
                sig_recalc(p);
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
                int s = spin_lock(&p->p_siglock);

                (void)sv_wait_sig(&p->p_pausesv, &p->p_siglock, s);
                if (sig_sleep_check())
                        return EINTR;
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

        l->l_sigoldmask = l->l_sigmask;
        l->l_sigsuspend = 1;
        l->l_sigmask = mask & ~CANTMASK;
        sig_recalc(p);
        spin_unlock(&p->p_siglock, s);
        return sig_pause();
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
                set = p->p_sigpend & curlwp->l_sigmask;
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
        sig_recalc(p);
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
                        sig_recalc(p);
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
