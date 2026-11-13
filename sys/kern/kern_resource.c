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
 * Every process is the superuser so far, so any process may raise its
 * hard limits.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/resource.h>
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
        secs = proc_ticks(p) / HZ;
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
        p->p_rlimit[which] = lim;
        if (which == RLIMIT_CPU)
                p->p_cpucheck = lim.rlim_cur != RLIM_INFINITY ||
                    lim.rlim_max != RLIM_INFINITY ? lbolt + 1 : 0;
        mutex_unlock(&p->p_lock);
        return 0;
}

/*
 * getrusage(who, struct rusage *), Belka's own: the processor time of
 * the process (RUSAGE_SELF) or of its reaped children
 * (RUSAGE_CHILDREN), all as user time; the other fields are zero.
 */
int
sys_getrusage(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct rusage ru;
        uint64_t ticks;

        switch ((int)args[0]) {
        case RUSAGE_SELF:
                ticks = proc_ticks(p);
                break;
        case RUSAGE_CHILDREN:
                ticks = p->p_cticks;
                break;
        default:
                return EINVAL;
        }
        memset(&ru, 0, sizeof(ru));
        ru.ru_utime.tv_sec = (int64_t)(ticks / HZ);
        ru.ru_utime.tv_usec = (int64_t)(ticks % HZ) * (1000000 / HZ);
        return copyout(&ru, (void *)(uintptr_t)args[1], sizeof(ru));
}
