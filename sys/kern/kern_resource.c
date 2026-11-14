/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-12
 * SPDX-License-Identifier: MIT
 */

/*
 * Resource limits: getrlimit (129) and setrlimit (128), UnixWare's
 * numbers, and the processor time limit; and getrusage (226), Belka's
 * own.
 *
 * Each process keeps its limits in p_rlimit, under p_lock; fork copies
 * them. Where each is enforced:
 *
 * - RLIMIT_CPU: on the way back to user mode, about once a second while
 *   a finite limit is set (p_cpucheck): SIGXCPU past the soft limit,
 *   each second, and SIGKILL past the hard one.
 * - RLIMIT_FSIZE: writes to regular files stop at it (uio_limit); one
 *   that can write nothing fails with EFBIG and sends SIGXFSZ.
 * - RLIMIT_DATA: the break may not grow past it.
 * - RLIMIT_STACK: exec maps that much stack, up to VM_STACK_SIZE.
 * - RLIMIT_NOFILE: no descriptor at or above it; NOFILE_MAX caps it.
 * - RLIMIT_VMEM: mmap and the break may not make the mapped total pass
 *   it.
 * - RLIMIT_CORE is kept, for when there are core files.
 *
 * Only the superuser may raise a hard limit.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/schedclass.h>
#include <sys/signal.h>
#include <sys/sysent.h>
#include <sys/systm.h>

#include <machine/vmparam.h>

/* The limits process 0 starts with, and so every process. */
void
rlimit_init(struct proc *p)
{
        for (int i = 0; i < RLIM_NLIMITS; i++) {
                p->p_rlimit[i].rlim_cur = RLIM_INFINITY;
                p->p_rlimit[i].rlim_max = RLIM_INFINITY;
        }
        p->p_rlimit[RLIMIT_STACK].rlim_cur = VM_STACK_SIZE;
        p->p_rlimit[RLIMIT_NOFILE].rlim_cur = NOFILE_MAX;
        p->p_rlimit[RLIMIT_NOFILE].rlim_max = NOFILE_MAX;
        p->p_cpucheck = 0;
}

/* p's soft limit for resource which. */
rlim_t
rlimit_cur(struct proc *p, int which)
{
        rlim_t r;

        mutex_lock(&p->p_lock);
        r = p->p_rlimit[which].rlim_cur;
        mutex_unlock(&p->p_lock);
        return r;
}

/*
 * The processor time check, from userret() once lbolt reaches
 * p_cpucheck: signal the process if it is past a limit, and set the
 * next check a second on.
 */
void
rlimit_cpu_check(void)
{
        struct proc *p = curproc;
        struct rlimit lim;
        uint64_t secs;

        mutex_lock(&p->p_lock);
        lim = p->p_rlimit[RLIMIT_CPU];
        if (p->p_cpucheck != 0)
                p->p_cpucheck = lbolt + HZ;
        mutex_unlock(&p->p_lock);
        secs = proc_ticks(p, NULL) / HZ;
        if (lim.rlim_max != RLIM_INFINITY && secs >= lim.rlim_max)
                psignal(p, SIGKILL);
        else if (lim.rlim_cur != RLIM_INFINITY && secs >= lim.rlim_cur)
                psignal(p, SIGXCPU);
}

/* getrlimit(resource, struct rlimit *) */
int
sys_getrlimit(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        int which = (int)args[0];
        struct rlimit lim;

        if (which < 0 || which >= RLIM_NLIMITS)
                return EINVAL;
        mutex_lock(&p->p_lock);
        lim = p->p_rlimit[which];
        mutex_unlock(&p->p_lock);
        return copyout(&lim, (void *)(uintptr_t)args[1], sizeof(lim));
}

/* setrlimit(resource, const struct rlimit *) */
int
sys_setrlimit(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        int which = (int)args[0];
        struct rlimit lim;
        int error;

        if (which < 0 || which >= RLIM_NLIMITS)
                return EINVAL;
        if ((error = copyin((const void *)(uintptr_t)args[1], &lim,
            sizeof(lim))) != 0)
                return error;
        if (lim.rlim_cur > lim.rlim_max)
                return EINVAL;
        if (which == RLIMIT_NOFILE) {
                if (lim.rlim_cur > NOFILE_MAX)
                        lim.rlim_cur = NOFILE_MAX;
                if (lim.rlim_max > NOFILE_MAX)
                        lim.rlim_max = NOFILE_MAX;
        }
        mutex_lock(&p->p_lock);
        if (lim.rlim_max > p->p_rlimit[which].rlim_max &&
            !CRED_ISROOT(CRED())) {
                mutex_unlock(&p->p_lock);
                return EPERM;
        }
        p->p_rlimit[which] = lim;
        if (which == RLIMIT_CPU)
                p->p_cpucheck = lim.rlim_cur != RLIM_INFINITY ||
                    lim.rlim_max != RLIM_INFINITY ? lbolt + 1 : 0;
        mutex_unlock(&p->p_lock);
        return 0;
}

static void
ticks_to_rt(uint64_t ticks, struct rusage_time *rt)
{
        rt->tv_sec = (int64_t)(ticks / HZ);
        rt->tv_usec = (int64_t)(ticks % HZ) * (1000000 / HZ);
}

/*
 * getrusage(who, struct rusage *), Belka's own: the processor time of
 * the process (RUSAGE_SELF) or of its reaped children
 * (RUSAGE_CHILDREN), user and system; the other fields are zero.
 */
int
sys_getrusage(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct rusage ru;
        uint64_t all, user;

        switch ((int)args[0]) {
        case RUSAGE_SELF:
                all = proc_ticks(p, &user);
                break;
        case RUSAGE_CHILDREN:
                mutex_lock(&proctree_lock);
                all = p->p_cticks;
                user = p->p_cuticks;
                mutex_unlock(&proctree_lock);
                break;
        default:
                return EINVAL;
        }
        memset(&ru, 0, sizeof(ru));
        ticks_to_rt(user, &ru.ru_utime);
        ticks_to_rt(all - user, &ru.ru_stime);
        return copyout(&ru, (void *)(uintptr_t)args[1], sizeof(ru));
}

/*
 * nice(incr), UnixWare's 34: add incr to the process's nice value, kept
 * from 0 to 2 * NZERO - 1 (only the superuser may lower it), and give
 * its LWPs the matching time-share weight. Returns the new value less
 * NZERO, as System V's does.
 */
int
sys_nice(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        int incr = (int)args[0], n;

        if (incr < 0 && !CRED_ISROOT(CRED()))
                return EPERM;
        mutex_lock(&p->p_lock);
        n = p->p_nice + (incr < -2 * NZERO ? -2 * NZERO :
            incr > 2 * NZERO ? 2 * NZERO : incr);
        if (n < 0)
                n = 0;
        if (n > 2 * NZERO - 1)
                n = 2 * NZERO - 1;
        p->p_nice = n;
        mutex_unlock(&p->p_lock);
        /* The LWPs share one job: setting the weight through one is enough. */
        {
                int s = spin_lock(&p->p_siglock);

                if (p->p_lwps != NULL && p->p_lwps->l_kthread != NULL)
                        (void)ts_setweight(p->p_lwps->l_kthread,
                            TS_NICEWEIGHT(n));
                spin_unlock(&p->p_siglock, s);
        }
        rv[0] = (uint64_t)(int64_t)(n - NZERO);
        return 0;
}
