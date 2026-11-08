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
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <machine/atomic.h>
#include <machine/intr.h>
#include <machine/md_var.h>

#include <vm/as.h>
#include <vm/hat.h>

#define PIDHASH         64
#define PID_MAX         30000
#define USER_PRI        PRI_KERNEL      /* until the thread joins TS */

struct proc proc0;
struct mutex proctree_lock;

static struct proc *pidhash[PIDHASH];
static pid_t nextpid = 1;
static struct kmem_zone *proc_zone;
static struct kmem_zone *lwp_zone;
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
        lwp_zone = kmem_zone_init(sizeof(struct lwp), "lwp");
        mutex_init(&proctree_lock, "proctree");
        mutex_init(&proc0.p_lock, "proc");
        sv_init(&proc0.p_waitsv, "wait");
        proc0.p_pid = 0;
        proc0.p_stat = SRUN;
        proc0.p_flags = P_SYSTEM;
        proc0.p_cred = kcred;
        proc0.p_cmask = 022;
        memcpy(proc0.p_comm, "sched", 6);
        VN_RELE(cons_vnode());          /* set it up now */
}

unsigned int
proc_count(void)
{
        return nprocs;
}

/* The process with id pid, or NULL. proctree_lock is held. */
static struct proc *
proc_find_locked(pid_t pid)
{
        struct proc *p = pidhash[(unsigned int)pid % PIDHASH];

        while (p != NULL && p->p_pid != pid)
                p = p->p_hnext;
        return p;
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
        p->p_stat = SIDL;
        p->p_cred = parent->p_cred;
        p->p_cmask = parent->p_cmask;
        memcpy(p->p_comm, parent->p_comm, PSCOMSIZ);

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
        if (p->p_ofile != NULL) {
                kmem_free(p->p_ofile, (size_t)p->p_nofile *
                    sizeof(struct file *));
                kmem_free(p->p_pofile, (size_t)p->p_nofile);
        }
        kmem_zone_free(proc_zone, p);
        (void)atomic_dec_32_nv(&nprocs);
}

struct lwp *
lwp_alloc(struct proc *p)
{
        struct lwp *l = kmem_zone_alloc(lwp_zone, KM_SLEEP);

        memset(l, 0, sizeof(*l));
        l->l_proc = p;
        l->l_lwpid = 1;
        p->p_lwp = l;
        return l;
}

void
lwp_free(struct lwp *l)
{
        md_lwp_free(l);
        kmem_zone_free(lwp_zone, l);
}

/*
 * Give l a stopped kernel thread that will run fn(arg) in the kernel,
 * with its user registers and floating-point area set up; lwp_run()
 * starts it.
 */
static void
lwp_create(struct lwp *l, void (*fn)(void *), void *arg)
{
        struct kthread *kt = kthread_create_stopped(fn, arg, USER_PRI,
            l->l_proc->p_comm);

        kt->kt_lwp = l;
        l->l_kthread = kt;
        md_lwp_init(l, kt->kt_stack);
}

static void
lwp_run(struct lwp *l)
{
        kthread_run(l->l_kthread);
}

/* For fork, which fills the registers in between. */
void
lwp_start_stopped(struct lwp *l, void (*fn)(void *), void *arg)
{
        lwp_create(l, fn, arg);
}

void
lwp_run_new(struct lwp *l)
{
        lwp_run(l);
}

void
lwp_start(struct lwp *l, void (*fn)(void *), void *arg)
{
        lwp_create(l, fn, arg);
        lwp_run(l);
}

/* First thing a new LWP's thread does: join time-sharing. */
static void
lwp_enter_ts(void)
{
        (void)kthread_setclass(curthread, SCHED_TS, TS_DEFWEIGHT);
}

/* fork: the child's LWP goes straight to user mode. */
void
lwp_fork_return(void *arg __unused)
{
        lwp_enter_ts();
        user_enter(curlwp->l_tf);
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

        lwp_enter_ts();
        error = exec_common(sp->sp_path, sp->sp_args);
        if (error != 0) {
                printf("proc_spawn: %s: error %d\n", sp->sp_path, error);
                kmem_free(sp, sizeof(*sp));
                proc_exit(CLD_EXITED, 127);
        }
        kmem_free(sp, sizeof(*sp));
        user_enter(curlwp->l_tf);
}

/*
 * Start a process running the program path with argument and
 * environment vectors (NULL-terminated, kernel strings), as a child of
 * process 0, with the console on descriptors 0 to 2 and the root as its
 * current directory. A program that cannot be run exits with 127.
 */
int
proc_spawn(const char *path, char *const argv[], char *const envp[],
    pid_t *pidp)
{
        struct spawn *sp;
        struct execargs *ea;
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
        fp = falloc(cons_vnode(), FREAD | FWRITE);
        for (int i = 0; i < 3; i++) {
                if (i != 0)
                        fhold(fp);
                (void)fd_alloc(p, fp, 0, &fd);
        }
        VN_HOLD(rootdir);
        p->p_cdir = rootdir;
        (void)lwp_alloc(p);
        lwp_create(p->p_lwp, spawn_start, sp);
        mutex_lock(&proctree_lock);
        p->p_stat = SRUN;
        mutex_unlock(&proctree_lock);
        *pidp = p->p_pid;
        lwp_run(p->p_lwp);
        return 0;
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
 * Reap a zombie child for wait: collect its status and free it.
 * proctree_lock is held and is released.
 */
void
proc_reap_locked(struct proc *c)
{
        proc_unlink(c);
        mutex_unlock(&proctree_lock);
        proc_free(c);
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
        struct proc *c, *zombies = NULL;
        int s, nowait;

        fd_close_all(p);
        mutex_lock(&p->p_lock);
        cdir = p->p_cdir;
        p->p_cdir = NULL;
        mutex_unlock(&p->p_lock);
        if (cdir != NULL)
                VN_RELE(cdir);

        /* Leave the address space before freeing it. */
        s = splhi();
        as = p->p_as;
        p->p_as = NULL;
        hat_activate(NULL);
        splx(s);
        if (as != NULL)
                as_free(as);

        /* Detach the LWP: from here on this is a kernel thread. */
        s = splhi();
        curthread->kt_lwp = NULL;
        splx(s);
        p->p_lwp = NULL;
        lwp_free(l);

        mutex_lock(&proctree_lock);
        /* Orphans go to process 0, which reaps them as they exit. */
        while ((c = p->p_children) != NULL) {
                p->p_children = c->p_sibling;
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
        p->p_wcode = code;
        p->p_wdata = data;
        p->p_stat = SZOMB;
        nowait = (p->p_flags & P_NOWAIT) != 0;
        if (nowait)
                proc_unlink(p);
        else
                sv_broadcast(&p->p_parent->p_waitsv);
        mutex_unlock(&proctree_lock);

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

/*
 * A fault the process cannot survive. Without signal delivery yet, it
 * ends as if killed by sig, with a note on the console.
 */
void
proc_fault(int sig, uint64_t addr)
{
        struct proc *p = curproc;

        printf("pid %d (%s): signal %d at %#jx; killed\n", p->p_pid,
            p->p_comm, sig, (uintmax_t)addr);
        proc_exit(CLD_KILLED, sig);
}
