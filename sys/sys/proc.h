/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Processes and LWPs, after the SVR4 model: a process owns the address
 * space, open files, directories, and credentials; its LWPs are what
 * run, each on a kthread of its own. Each process has exactly one LWP
 * for now, but nothing outside fork, exec, and exit assumes it.
 *
 * Process 0 stands for the kernel. Kernel threads belong to it, and
 * processes the kernel starts (proc_spawn) are its children.
 *
 * Locking:
 * - proctree_lock (a mutex) protects the process tree, the pid hash,
 *   p_stat, and the wait information; p_waitsv is waited on with it.
 * - p_lock (a mutex) protects a process's file table, directories, and
 *   break.
 * - p_as may be replaced only by the process itself (exec), so its own
 *   LWP reads it without a lock.
 * - p_siglock (a spin lock, since signals are posted from interrupts)
 *   protects the signal state of the process and of its LWP: actions,
 *   pending signals and their information, the LWP's mask and alternate
 *   stack, and the stop state. It ranks above sync object locks, so a
 *   poster can interrupt the LWP's sleep while holding it.
 */

#ifndef _SYS_PROC_H_
#define _SYS_PROC_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/compiler.h>
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
#define NOFILE_MAX      1024    /* open files per process */

/* A set of signals inside the kernel: bit (sig - 1). */
typedef uint64_t k_sigset_t;
#define sigbit(sig)     ((k_sigset_t)1 << ((sig) - 1))

struct lwp {
        struct kthread  *l_kthread;
        struct proc     *l_proc;
        uint32_t        l_lwpid;
        struct trapframe *l_tf;         /* user registers */
        void            *l_fpu;         /* floating-point save area */
        uint64_t        l_fsbase;       /* LWP private data (%fs base) */

        /* Signals, under p_siglock. */
        k_sigset_t      l_sigmask;      /* held */
        k_sigset_t      l_sigoldmask;   /* to restore after sigsuspend */
        int             l_sigsuspend;   /* l_sigoldmask is in use */
        uint64_t        l_altsp;        /* alternate signal stack */
        size_t          l_altsize;
        int             l_altflags;     /* SS_DISABLE, or 0 */
};

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
        struct lwp      *p_lwp;         /* the only LWP, for now */
        struct cred     *p_cred;

        /* Under p_lock. */
        struct file     **p_ofile;
        uint8_t         *p_pofile;      /* FD_CLOEXEC per descriptor */
        int             p_nofile;       /* slots in the tables */
        struct vnode    *p_cdir;
        struct vnode    *p_rdir;        /* NULL: the system root */
        mode_t          p_cmask;
        vaddr_t         p_brkbase;
        size_t          p_brksize;

        /* Process groups and sessions, under proctree_lock. */
        pid_t           p_pgid;
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
        int64_t         p_alarm;        /* pending alarm callout, or 0 */
        uint64_t        p_alarm_when;   /* lbolt it fires at */

        char            p_comm[PSCOMSIZ];
};

extern struct proc proc0;
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
struct lwp *lwp_alloc(struct proc *);
void lwp_free(struct lwp *);
void lwp_start(struct lwp *, void (*)(void *), void *);
void lwp_start_stopped(struct lwp *, void (*)(void *), void *);
void lwp_run_new(struct lwp *);
void lwp_fork_return(void *);
void proc_reap_locked(struct proc *);
int proc_spawn(const char *, char *const[], char *const[], pid_t *);
int proc_wait(pid_t, int *, int *);
void proc_exit(int, int) __dead;
unsigned int proc_count(void);

/* kern_sig.c */
void sig_proc_init(struct proc *, struct proc *);
void sig_exec(struct proc *);
void sigtoproc(struct proc *, int, const siginfo_t *);
void psignal(struct proc *, int);
void trapsignal(int, int, uint64_t);
void sig_userret(void);
int sig_restart(int);
int sig_child_exit(struct proc *, int, int);
int sig_sleep_check(void);
void sig_cancel_alarm(struct proc *);

/* kern_exec.c */
struct execargs;
int exec_common(const char *, struct execargs *);
struct execargs *execargs_alloc(void);
void execargs_free(struct execargs *);
int execargs_add(struct execargs *, const char *, int);
int execargs_copyin_vec(struct execargs *, char *const *, int);

/* kern_descrip.c */
int fd_alloc(struct proc *, struct file *, int, int *);
int fd_install(struct proc *, struct file *, int);
int fd_cloexec(struct proc *, int, int);
struct file *fd_get(struct proc *, int);
int fd_close(struct proc *, int);
void fd_dup_table(struct proc *, struct proc *);
void fd_close_all(struct proc *);
void fd_close_exec(struct proc *);

#endif /* _SYS_PROC_H_ */
