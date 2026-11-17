/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Processes and LWPs: process 0, process ids, creating LWPs, starting
 * processes from the kernel, exit, and the kernel's wait for them. See
 * <sys/proc.h>.
 *
 * Exit follows the UnixWare order in outline: close files, let go of the
 * directories and the address space, give the children to process 0
 * (there is no init yet), become a zombie, and wake the parent. The LWP
 * is detached from its thread before the zombie is visible, so whoever
 * reaps it frees nothing the dying thread still uses; the thread then
 * leaves through kthread_exit().
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cons.h>
#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/pathname.h>
#include <sys/proc.h>
#include <sys/schedclass.h>
#include <sys/signal.h>
#include <sys/synch.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <machine/atomic.h>
#include <machine/intr.h>
#include <machine/md_var.h>

#include <vm/as.h>
#include <vm/hat.h>

#define PIDHASH         64
#define USER_PRI        PRI_KERNEL      /* until the thread joins TS */

struct proc proc0;
struct proc *initproc;                 /* process 1, once it runs */
struct mutex proctree_lock;

static struct proc *pidhash[PIDHASH];
static pid_t nextpid = 1;
static struct kmem_zone *proc_zone;
static volatile uint32_t nprocs;

static void proc_unlink(struct proc *);

struct proc *
curproc_get(void)
{
        struct lwp *l = curthread->kt_lwp;

        return l != NULL ? l->l_proc : &proc0;
}

struct lwp *
curlwp_get(void)
{
        return curthread->kt_lwp;
}

void
proc_init(void)
{
        proc_zone = kmem_zone_init(sizeof(struct proc), "proc");
        lwp_sysinit();
        mutex_init(&proctree_lock, "proctree");
        mutex_init(&proc0.p_lock, "proc");
        sv_init(&proc0.p_waitsv, "wait");
        proc0.p_pid = 0;
        proc0.p_stat = SRUN;
        proc0.p_flags = P_SYSTEM;
        spinlock_init(&proc0.p_crlock, "cred");
        crhold(kcred);
        proc0.p_cred = kcred;
        proc0.p_cmask = 022;
        proc0.p_nice = NZERO;
        rlimit_init(&proc0);
        memcpy(proc0.p_comm, "sched", 6);
        sig_proc_init(&proc0, NULL);
        proc0.p_sigflags = PS_JCTL;
        VN_RELE(cons_vnode());          /* set it up now */
}

unsigned int
proc_count(void)
{
        return nprocs;
}

/* The process with id pid, or NULL. proctree_lock is held. */
struct proc *
proc_find_locked(pid_t pid)
{
        struct proc *p = pidhash[(unsigned int)pid % PIDHASH];

        while (p != NULL && p->p_pid != pid)
                p = p->p_hnext;
        return p;
}

/* Call fn(p, arg) for every process but 0. proctree_lock is held. */
void
proc_iterate_locked(void (*fn)(struct proc *, void *), void *arg)
{
        for (unsigned int h = 0; h < PIDHASH; h++) {
                for (struct proc *p = pidhash[h]; p != NULL; p = p->p_hnext)
                        fn(p, arg);
        }
}

struct proc *
proc_find(pid_t pid)
{
        struct proc *p;

        mutex_lock(&proctree_lock);
        p = proc_find_locked(pid);
        mutex_unlock(&proctree_lock);
        return p;
}

/*
 * A new process, a child of parent, in state SIDL with a fresh process
 * id. Its address space, files, directories, and LWP are the caller's
 * to set up.
 */
struct proc *
proc_alloc(struct proc *parent)
{
        struct proc *p = kmem_zone_alloc(proc_zone, KM_SLEEP);
        unsigned int h;

        memset(p, 0, sizeof(*p));
        mutex_init(&p->p_lock, "proc");
        sv_init(&p->p_waitsv, "wait");
        sv_init(&p->p_lwpsv, "lwps");
        sv_init(&p->p_rendezsv, "parked");
        p->p_stat = SIDL;
        spinlock_init(&p->p_crlock, "cred");
        p->p_cred = cred_getproc(parent);
        p->p_cmask = parent->p_cmask;
        p->p_nice = parent->p_nice;
        {
                struct timespec ts;

                gethrestime(&ts);
                p->p_start[0] = ts.tv_sec;
                p->p_start[1] = ts.tv_nsec;
        }
        memcpy(p->p_rlimit, parent->p_rlimit, sizeof(p->p_rlimit));
        p->p_cpucheck = parent->p_cpucheck != 0 ? lbolt + 1 : 0;
        memcpy(p->p_comm, parent->p_comm, PSCOMSIZ);
        sig_proc_init(p, parent);

        mutex_lock(&proctree_lock);
        do {
                p->p_pid = nextpid++;
                if (nextpid >= PID_MAX)
                        nextpid = 1;
        } while (proc_find_locked(p->p_pid) != NULL);
        h = (unsigned int)p->p_pid % PIDHASH;
        p->p_hnext = pidhash[h];
        pidhash[h] = p;
        p->p_parent = parent;
        p->p_sibling = parent->p_children;
        parent->p_children = p;
        p->p_pgid = parent->p_pgid;
        p->p_sid = parent->p_sid;
        mutex_unlock(&proctree_lock);
        (void)atomic_inc_32_nv(&nprocs);
        return p;
}

/* Take p out of the hash and its parent's children. proctree_lock held. */
static void
proc_unlink(struct proc *p)
{
        struct proc **link = &pidhash[(unsigned int)p->p_pid % PIDHASH];

        while (*link != p)
                link = &(*link)->p_hnext;
        *link = p->p_hnext;
        for (link = &p->p_parent->p_children; *link != p;
            link = &(*link)->p_sibling)
                continue;
        *link = p->p_sibling;
}

/* Free a process that is unlinked and has nothing left. */
void
proc_free(struct proc *p)
{
        crfree(p->p_cred);
        if (p->p_ofile != NULL) {
                kmem_free(p->p_ofile, (size_t)p->p_nofile *
                    sizeof(struct file *));
                kmem_free(p->p_pofile, (size_t)p->p_nofile);
        }
        kmem_zone_free(proc_zone, p);
        (void)atomic_dec_32_nv(&nprocs);
}

struct spawn {
        char            sp_path[MAXPATHLEN];
        struct execargs *sp_args;
};

static void
spawn_start(void *arg)
{
        struct spawn *sp = arg;
        int error;

        error = exec_common(sp->sp_path, sp->sp_args);
        if (error != 0) {
                printf("proc_spawn: %s: error %d\n", sp->sp_path, error);
                kmem_free(sp, sizeof(*sp));
                proc_exit(CLD_EXITED, 127);
        }
        kmem_free(sp, sizeof(*sp));
        lwp_user_start(NULL);
}

/*
 * Start a process running the program path with argument and
 * environment vectors (NULL-terminated, kernel strings), as a child of
 * process 0, with the console on descriptors 0 to 2 and the root as its
 * current directory. A program that cannot be run exits with 127.
 */
static int
spawn(const char *path, char *const argv[], char *const envp[], pid_t *pidp,
    int isinit)
{
        struct spawn *sp;
        struct execargs *ea;
        struct lwp *l;
        struct proc *p;
        struct file *fp;
        const char *base = path;
        size_t len = strlen(path);
        int fd, error = 0;

        if (len >= MAXPATHLEN)
                return ENAMETOOLONG;
        ea = execargs_alloc();
        for (int i = 0; argv != NULL && argv[i] != NULL && error == 0; i++)
                error = execargs_add(ea, argv[i], 0);
        for (int i = 0; envp != NULL && envp[i] != NULL && error == 0; i++)
                error = execargs_add(ea, envp[i], 1);
        if (error != 0) {
                execargs_free(ea);
                return error;
        }
        sp = kmem_alloc(sizeof(*sp), KM_SLEEP);
        memcpy(sp->sp_path, path, len + 1);
        sp->sp_args = ea;

        p = proc_alloc(&proc0);
        for (const char *c = path; *c != '\0'; c++) {
                if (*c == '/')
                        base = c + 1;
        }
        memset(p->p_comm, 0, PSCOMSIZ);
        for (size_t i = 0; i + 1 < PSCOMSIZ && base[i] != '\0'; i++)
                p->p_comm[i] = base[i];
        fp = falloc(cons_open_vnode(), FREAD | FWRITE);
        for (int i = 0; i < 3; i++) {
                if (i != 0)
                        fhold(fp);
                (void)fd_alloc(p, fp, 0, &fd);
        }
        VN_HOLD(rootdir);
        p->p_cdir = rootdir;
        (void)lwp_alloc(p, &l);
        lwp_newthread(l, spawn_start, sp);
        lwp_join_ts(l, NULL);
        mutex_lock(&proctree_lock);
        /*
         * A process the kernel starts leads a session of its own; init
         * stays in process 0's and makes its own with setsid().
         */
        if (!isinit)
                p->p_pgid = p->p_sid = p->p_pid;
        p->p_stat = SRUN;
        if (isinit)
                initproc = p;
        mutex_unlock(&proctree_lock);
        *pidp = p->p_pid;
        lwp_run(l);
        return 0;
}

int
proc_spawn(const char *path, char *const argv[], char *const envp[],
    pid_t *pidp)
{
        return spawn(path, argv, envp, pidp, 0);
}

/* The same for init: orphans go to it from the moment it runs. */
int
proc_spawn_init(const char *path, char *const argv[], char *const envp[],
    pid_t *pidp)
{
        return spawn(path, argv, envp, pidp, 1);
}

/*
 * Wait, in the kernel, for pid, a child of process 0, to exit, and reap
 * it. *codep gets CLD_EXITED or CLD_KILLED, *datap the exit status or
 * signal. ECHILD if pid is not such a child.
 */
int
proc_wait(pid_t pid, int *codep, int *datap)
{
        mutex_lock(&proctree_lock);
        for (;;) {
                struct proc *c = proc_find_locked(pid);

                if (c == NULL || c->p_parent != &proc0 ||
                    (c->p_flags & P_NOWAIT) != 0) {
                        mutex_unlock(&proctree_lock);
                        return ECHILD;
                }
                if (c->p_stat == SZOMB) {
                        *codep = c->p_wcode;
                        *datap = c->p_wdata;
                        proc_unlink(c);
                        mutex_unlock(&proctree_lock);
                        proc_free(c);
                        return 0;
                }
                sv_wait_mutex(&proc0.p_waitsv, &proctree_lock);
                mutex_lock(&proctree_lock);
        }
}

/*
 * A zombie's processor time, with that of the children it reaped, as
 * its SIGCHLD and waitsys() report it: user and system, in ticks.
 */
void
proc_zombie_times(const struct proc *c, siginfo_t *si)
{
        uint64_t all = c->p_ticks + c->p_cticks;
        uint64_t user = c->p_uticks + c->p_cuticks;

        si->si_utime = (int64_t)user;
        si->si_stime = (int64_t)(all - user);
}

/*
 * Reap a zombie child for wait: collect its status and free it.
 * proctree_lock is held and is released.
 */
void
proc_reap_locked(struct proc *c)
{
        /* The parent's children's time, as times() reports it. */
        c->p_parent->p_cticks += c->p_ticks + c->p_cticks;
        c->p_parent->p_cuticks += c->p_uticks + c->p_cuticks;
        proc_unlink(c);
        mutex_unlock(&proctree_lock);
        proc_free(c);
}

/*
 * The process groups whose orphaning an exit checks, at most: ours and
 * those of up to EXIT_PGIDS - 1 children (distinct ones; later ones
 * beyond that go unchecked).
 */
#define EXIT_PGIDS      32

static void
exit_note_pgid(pid_t *pgids, int *n, pid_t pgid)
{
        for (int i = 0; i < *n; i++)
                if (pgids[i] == pgid)
                        return;
        if (*n < EXIT_PGIDS)
                pgids[(*n)++] = pgid;
}

/*
 * End the calling process: code is CLD_EXITED with an exit status, or
 * CLD_KILLED with a signal.
 */
void
proc_exit(int code, int data)
{
        struct lwp *l = curlwp;
        struct proc *p = l->l_proc;
        struct as *as;
        struct vnode *cdir;
        struct proc *c, *heir, *zombies = NULL;
        pid_t pgids[EXIT_PGIDS];
        int s, nowait, npgids = 0, heir_zombie = 0;

        /* The other LWPs go first; if another LWP is ending us, just go. */
        if (proc_single(1) != 0)
                lwp_exit();
        itimer_exit(p);
        fd_close_all(p);
        /* A session leader's terminal is let go (SIGHUP). */
        if (p->p_pid == p->p_sid)
                tty_session_end(p->p_sid);
        mutex_lock(&p->p_lock);
        cdir = p->p_cdir;
        p->p_cdir = NULL;
        mutex_unlock(&p->p_lock);
        if (cdir != NULL)
                VN_RELE(cdir);

        /* Leave the address space before freeing it (see <sys/proc.h>). */
        mutex_lock(&proctree_lock);
        s = splhi();
        as = p->p_as;
        p->p_as = NULL;
        hat_activate(NULL);
        splx(s);
        mutex_unlock(&proctree_lock);
        if (as != NULL)
                as_free(as);

        /*
         * Detach the LWP: from here on this is a kernel thread. Posters
         * see p_lwp gone under the signal lock and leave it alone.
         */
        s = spin_lock(&p->p_siglock);
        p->p_ticks += curthread->kt_ticks;
        p->p_uticks += curthread->kt_uticks;
        p->p_lwps = NULL;
        p->p_nlwp = 0;
        curthread->kt_sigpend = 0;
        spin_unlock(&p->p_siglock, s);
        s = splhi();
        curthread->kt_lwp = NULL;
        splx(s);
        md_lwp_free(l);
        lwp_free(l);
        lwp_free_zombies(p);

        mutex_lock(&proctree_lock);
        /*
         * Orphans go to init, which waits for them, or, until there is
         * one (and in a self-test boot), to process 0, which reaps them
         * as they exit. Init hears of zombies among them by SIGCHLD.
         * Our group and our children's may be orphaned now (see below).
         */
        pgids[npgids++] = p->p_pgid;
        heir = initproc != NULL && initproc != p ? initproc : NULL;
        while ((c = p->p_children) != NULL) {
                p->p_children = c->p_sibling;
                exit_note_pgid(pgids, &npgids, c->p_pgid);
                if (heir != NULL) {
                        c->p_parent = heir;
                        c->p_sibling = heir->p_children;
                        heir->p_children = c;
                        if (c->p_stat == SZOMB)
                                heir_zombie = 1;
                        continue;
                }
                c->p_parent = &proc0;
                c->p_flags |= P_NOWAIT;
                if (c->p_stat == SZOMB) {
                        c->p_sibling = zombies;
                        zombies = c;
                        continue;
                }
                c->p_sibling = proc0.p_children;
                proc0.p_children = c;
        }
        if (heir_zombie)
                sv_broadcast(&heir->p_waitsv);
        p->p_wcode = code;
        p->p_wdata = data;
        p->p_stat = SZOMB;
        nowait = (p->p_flags & P_NOWAIT) != 0 ||
            sig_child_exit(p, code, data);
        /* Wake the parent either way: a wait may now find no children. */
        sv_broadcast(&p->p_parent->p_waitsv);
        if (nowait)
                proc_unlink(p);
        mutex_unlock(&proctree_lock);
        if (heir_zombie)
                psignal(heir, SIGCHLD);
        for (int i = 0; i < npgids; i++)
                pgrp_orphan_check(pgids[i]);

        /* Orphaned zombies were already off the hash's other lists. */
        while (zombies != NULL) {
                c = zombies;
                zombies = c->p_sibling;
                mutex_lock(&proctree_lock);
                {
                        struct proc **link =
                            &pidhash[(unsigned int)c->p_pid % PIDHASH];

                        while (*link != c)
                                link = &(*link)->p_hnext;
                        *link = c->p_hnext;
                }
                mutex_unlock(&proctree_lock);
                proc_free(c);
        }
        if (nowait)
                proc_free(p);
        kthread_exit();
}

