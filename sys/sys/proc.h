/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Processes and LWPs, after the SVR4 model: a process owns the address
 * space, open files, directories, and credentials; its LWPs are what
 * run, each on a kthread of its own (see kern_lwp.c).
 *
 * Process 0 stands for the kernel. Kernel threads belong to it, and
 * processes the kernel starts (proc_spawn) are its children.
 *
 * Locking:
 * - proctree_lock (a mutex) protects the process tree, the pid hash,
 *   p_stat, and the wait information; p_waitsv is waited on with it.
 * - p_lock (a mutex) protects a process's file table, directories, and
 *   break.
 * - p_as may be replaced only by the process itself (exec, exit), under
 *   proctree_lock, so its own LWP reads it without a lock and others
 *   (/proc) with proctree_lock.
 * - p_cred is replaced under p_lock and p_crlock; see <sys/cred.h>.
 * - p_siglock (a spin lock, since signals are posted from interrupts)
 *   protects the signal state of the process and of its LWPs: actions,
 *   pending signals and their information, masks and alternate stacks,
 *   and the stop state; and the LWP list and every LWP's l_flags. It
 *   ranks above sync object locks, so a poster can interrupt the LWP's
 *   sleep while holding it.
 */

#ifndef _SYS_PROC_H_
#define _SYS_PROC_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/compiler.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/types.h>

#include <machine/types.h>

struct as;
struct cred;
struct file;
struct kthread;
struct trapframe;
struct vnode;

#define PSCOMSIZ        16      /* command name, with its terminator */
#define PSARGSZ         80      /* the arguments, for /proc */
#define PID_MAX         30000   /* process ids are 1 to PID_MAX - 1 */
#define NOFILE_MAX      1024    /* open files per process */

/* A set of signals inside the kernel: bit (sig - 1). */
typedef uint64_t k_sigset_t;
#define sigbit(sig)     ((k_sigset_t)1 << ((sig) - 1))

struct lwp {
        struct kthread  *l_kthread;
        struct proc     *l_proc;
        struct lwp      *l_next;        /* p_lwps or p_lwpzombies */
        uint32_t        l_lwpid;
        int             l_flags;        /* LW_*, under p_siglock */
        struct trapframe *l_tf;         /* user registers */
        void            *l_fpu;         /* floating-point save area */
        uint64_t        l_fsbase;       /* LWP private data (%fs base) */
        struct cred     *l_cred;        /* held; see <sys/cred.h> */

        /* Signals, under p_siglock. */
        k_sigset_t      l_sigmask;      /* held */
        k_sigset_t      l_sigoldmask;   /* to restore after sigsuspend */
        int             l_sigsuspend;   /* l_sigoldmask is in use */
        uint64_t        l_altsp;        /* alternate signal stack */
        size_t          l_altsize;
        int             l_altflags;     /* SS_DISABLE, or 0 */
        k_sigset_t      l_sigpend;      /* directed at this LWP */
        siginfo_t       l_siginfo[NSIG];
        k_sigset_t      l_sigwait;      /* awaited in sigwait */
        int             l_timedout;     /* a timed wait ran out */
};

/* l_flags */
#define LW_DETACHED     0x0001  /* nobody will _lwp_wait for it */
#define LW_SUSPREQ      0x0002  /* to be suspended at the next chance */
#define LW_SUSPENDED    0x0004  /* suspended */
#define LW_DESTROY      0x0008  /* to exit at the next chance */
#define LW_QUIESCED     0x0010  /* parked for a rendezvous or a stop */
#define LW_EXITING      0x0020  /* a zombie still leaving its thread */

/* A signal's action, as the kernel keeps it. */
struct ksigaction {
        uint64_t        ksa_handler;    /* SIG_DFL, SIG_IGN, or a handler */
        int             ksa_flags;
        k_sigset_t      ksa_mask;
};

/* p_stat */
#define SIDL            1       /* being created */
#define SRUN            2       /* has LWPs */
#define SZOMB           3       /* exited, waiting to be reaped */

/* p_lwpflags */
#define PL_SINGLE       0x0001  /* exit or exec: other LWPs are going */
#define PL_RENDEZ       0x0002  /* other LWPs are to park */
#define PL_CONTNOTIFY   0x0004  /* tell the parent of a continue */

/* p_sigflags */
#define PS_JCTL         0x0001  /* tell the parent of stops (SIGCHLD) */
#define PS_NOCLDWAIT    0x0002  /* children leave no zombies */

/* p_flags */
#define P_NOWAIT        0x0001  /* freed at exit; nobody will wait */
#define P_SYSTEM        0x0002  /* process 0 */

struct proc {
        struct mutex    p_lock;
        pid_t           p_pid;
        int             p_stat;
        int             p_flags;

        /* The process tree, under proctree_lock. */
        struct proc     *p_parent;
        struct proc     *p_children;
        struct proc     *p_sibling;
        struct proc     *p_hnext;       /* pid hash */
        struct sv       p_waitsv;       /* a child changed state */
        int             p_wcode;        /* CLD_EXITED or CLD_KILLED */
        int             p_wdata;        /* exit status or signal */
        int             p_wstop;        /* a stop not yet waited for */
        int             p_wcont;        /* a continue not yet waited for */

        struct as       *p_as;
        struct cred     *p_cred;        /* see <sys/cred.h> */
        struct spinlock p_crlock;       /* the p_cred pointer */

        /*
         * Processor time in clock ticks, all of it and the part spent in
         * user mode: of the LWPs that have exited (p_siglock; the live
         * ones hold theirs in kt_ticks and kt_uticks), and of the
         * children reaped (proctree_lock).
         */
        uint64_t        p_ticks;
        uint64_t        p_uticks;
        uint64_t        p_cticks;
        uint64_t        p_cuticks;

        /* LWPs, under p_siglock. */
        struct lwp      *p_lwps;        /* live */
        int             p_nlwp;
        int             p_lwpleaving;   /* unlinked, thread still on it */
        struct lwp      *p_lwpzombies;  /* exited, waiting for _lwp_wait */
        uint32_t        p_lwpnext;      /* next id */
        int             p_lwpflags;     /* PL_* */
        struct lwp      *p_single;      /* making the process single */
        struct lwp      *p_rendez;      /* asking for a rendezvous */
        int             p_nquiesced;    /* LWPs parked (LW_QUIESCED) */
        struct sv       p_lwpsv;        /* LWP state changed */
        struct sv       p_rendezsv;     /* parked LWPs wait here */

        /* Under p_lock. */
        struct file     **p_ofile;
        uint8_t         *p_pofile;      /* FD_CLOEXEC per descriptor */
        int             p_nofile;       /* slots in the tables */
        struct vnode    *p_cdir;
        struct vnode    *p_rdir;        /* NULL: the system root */
        mode_t          p_cmask;
        int             p_nice;         /* 0 to 2 * NZERO - 1 (p_lock) */
        vaddr_t         p_brkbase;
        size_t          p_brksize;
        struct rlimit   p_rlimit[RLIM_NLIMITS];
        /* lbolt of the next processor time check, 0 for none; racy reads. */
        volatile uint64_t p_cpucheck;

        /* Process groups and sessions, under proctree_lock. */
        pid_t           p_pgid;
        uint64_t        p_identity;     /* stable across PID reuse */
        uint64_t        p_grpidentity;  /* stable while the group lives */
        pid_t           p_sid;

        /* Signals, under p_siglock. */
        struct spinlock p_siglock;
        struct ksigaction p_sigact[NSIG];
        k_sigset_t      p_sigpend;
        siginfo_t       p_siginfo[NSIG];
        uint64_t        p_sigreturn;    /* the library's return trampoline */
        int             p_sigflags;     /* PS_* */
        int             p_stopped;      /* stopped by a signal */
        int             p_stopsig;
        struct sv       p_stopsv;       /* where a stopped LWP waits */
        struct sv       p_pausesv;      /* pause, sigsuspend */
        /* Interval timers, in ticks (kern_itimer.c). */
        int64_t         p_alarm;        /* ITIMER_REAL's callout, or 0 */
        uint64_t        p_alarm_when;   /* lbolt it fires at */
        uint64_t        p_alarm_ival;   /* its interval, or 0 */
        uint64_t        p_itvalue[2];   /* VIRTUAL, PROF: left, 0 if off */
        uint64_t        p_itival[2];    /* their intervals */

        char            p_comm[PSCOMSIZ];
        char            p_psargs[PSARGSZ];      /* argv at exec, joined */
        int64_t         p_start[2];     /* fork: seconds, ns since 1970 */
};

extern struct proc proc0;
extern struct proc *initproc;
extern struct mutex proctree_lock;

/* The process of the running thread: process 0 for kernel threads. */
struct proc *curproc_get(void);
#define curproc         (curproc_get())
struct lwp *curlwp_get(void);
#define curlwp          (curlwp_get())

void proc_init(void);
struct proc *proc_alloc(struct proc *);
void proc_free(struct proc *);
struct proc *proc_find(pid_t);
struct proc *proc_find_locked(pid_t);
void proc_iterate_locked(void (*)(struct proc *, void *), void *);
uint64_t proc_identity_locked(void);
/* kern_lwp.c */
void lwp_sysinit(void);
void lwp_sema_init(void);
int lwp_alloc(struct proc *, struct lwp **);
void lwp_free(struct lwp *);
struct lwp *lwp_find_locked(struct proc *, uint32_t);
void lwp_newthread(struct lwp *, void (*)(void *), void *);
void lwp_join_ts(struct lwp *, struct lwp *);
void lwp_run(struct lwp *);
void lwp_user_start(void *);
int lwp_park(struct proc *, struct lwp *, int);
int lwp_park_pending(const struct proc *, const struct lwp *);
void lwp_exit(void) __dead;
void lwp_free_zombies(struct proc *);
int proc_single(int);
int lwp_rendezvous(void);
void lwp_rendezvous_done(void);
void proc_reap_locked(struct proc *);
void proc_zombie_times(const struct proc *, siginfo_t *);
int proc_spawn(const char *, char *const[], char *const[], pid_t *);
int proc_spawn_init(const char *, char *const[], char *const[], pid_t *);
int proc_wait(pid_t, int *, int *);
/* kern_time.c */
uint64_t proc_ticks(struct proc *, uint64_t *);
void proc_exit(int, int) __dead;
unsigned int proc_count(void);

/* kern_sig.c */
void sig_proc_init(struct proc *, struct proc *);
void sig_exec(struct proc *);
void sigtoproc(struct proc *, int, const siginfo_t *);
void sigtolwp_locked(struct proc *, struct lwp *, int, const siginfo_t *);
void sig_recalc_lwp(struct proc *, struct lwp *);
void sig_recalc_all(struct proc *);
void sig_nudge_lwp(struct lwp *);
void sig_nudge_all(struct proc *);
k_sigset_t sig_mask_from_user(const sigset_t *);
void psignal(struct proc *, int);
void pgsignal(pid_t, int);
int pgrp_in_session(pid_t, pid_t);
int pgrp_orphaned(pid_t, int *);
void pgrp_orphan_check(pid_t);
int sig_held_or_ignored(int);
void trapsignal(int, int, uint64_t);
void sig_userret(void);
int sig_restart(int);
int sig_child_exit(struct proc *, int, int);
int sig_sleep_check(void);
int sig_wait_mask(uint64_t);
void sig_wait_mask_end(int);
int lwp_timeout_ticks(uint64_t, long *);
int64_t lwp_timeout_start(long);
void lwp_timeout_stop(int64_t);
void itimer_exit(struct proc *);

/* kern_exec.c */
struct execargs;
int exec_common(const char *, struct execargs *);
struct execargs *execargs_alloc(void);
void execargs_free(struct execargs *);
int execargs_add(struct execargs *, const char *, int);
int execargs_copyin_vec(struct execargs *, char *const *, int);

/* kern_descrip.c */
int fd_alloc(struct proc *, struct file *, int, int *);
int fd_alloc_flags(struct proc *, struct file *, int, int, int *);
int fd_alloc_pair(struct proc *, struct file *[2], int, int [2]);
int fd_install(struct proc *, struct file *, int);
int fd_cloexec(struct proc *, int, int);
struct file *fd_get(struct proc *, int);
int fd_close(struct proc *, int);
void fd_dup_table(struct proc *, struct proc *);
void fd_close_all(struct proc *);
void fd_close_exec(struct proc *);

#endif /* _SYS_PROC_H_ */
