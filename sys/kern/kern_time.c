/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-10
 * SPDX-License-Identifier: MIT
 */

/*
 * Clocks and processor time from user mode: clock_gettime() and
 * clock_getres() (Belka's, 210 and 211: UnixWare 2 has no POSIX
 * clocks), times() (UnixWare's 43), and sched_yield() (212).
 *
 * Processor time is counted in clock ticks, all of it as user time: a
 * process's is the ticks of the LWPs that have exited (p_ticks) and of
 * those still running (their kt_ticks).
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/spinlock.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/times.h>

#include <machine/md_var.h>

/* The processor time of p's LWPs, exited and live, in ticks. */
uint64_t
proc_ticks(struct proc *p)
{
        uint64_t t;
        int s = spin_lock(&p->p_siglock);

        t = p->p_ticks;
        for (struct lwp *l = p->p_lwps; l != NULL; l = l->l_next) {
                if (l->l_kthread != NULL)
                        t += l->l_kthread->kt_ticks;
        }
        spin_unlock(&p->p_siglock, s);
        return t;
}

static void
ns_to_ts(uint64_t ns, struct timespec *ts)
{
        ts->tv_sec = (int64_t)(ns / NANOSEC);
        ts->tv_nsec = (long)(ns % NANOSEC);
}

/* clock_gettime(clockid, struct timespec *) */
int
sys_clock_gettime(const uint64_t *args, uint64_t *rv __unused)
{
        struct timespec ts;

        switch ((int)args[0]) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
                gethrestime(&ts);
                break;
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME:
                ns_to_ts(gethrtime(), &ts);
                break;
        case CLOCK_PROCESS_CPUTIME_ID:
                ns_to_ts(proc_ticks(curproc) * (NANOSEC / HZ), &ts);
                break;
        case CLOCK_THREAD_CPUTIME_ID:
                ns_to_ts(curthread->kt_ticks * (NANOSEC / HZ), &ts);
                break;
        default:
                return EINVAL;
        }
        return copyout(&ts, (void *)(uintptr_t)args[1], sizeof(ts));
}

/* clock_getres(clockid, struct timespec *): the clock's resolution. */
int
sys_clock_getres(const uint64_t *args, uint64_t *rv __unused)
{
        struct timespec ts;
        uint64_t tick = NANOSEC / HZ;

        switch ((int)args[0]) {
        case CLOCK_REALTIME:
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_BOOTTIME:
                ns_to_ts(md_hrtime_ok ? 1 : tick, &ts);
                break;
        case CLOCK_REALTIME_COARSE:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_PROCESS_CPUTIME_ID:
        case CLOCK_THREAD_CPUTIME_ID:
                ns_to_ts(tick, &ts);
                break;
        default:
                return EINVAL;
        }
        if (args[1] == 0)
                return 0;
        return copyout(&ts, (void *)(uintptr_t)args[1], sizeof(ts));
}

/* times(struct tms *): returns the ticks since boot. */
int
sys_times(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        struct tms t;
        int error;

        t.tms_utime = (int64_t)proc_ticks(p);
        t.tms_stime = 0;
        t.tms_cutime = (int64_t)p->p_cticks;
        t.tms_cstime = 0;
        if ((error = copyout(&t, (void *)(uintptr_t)args[0], sizeof(t))) != 0)
                return error;
        rv[0] = lbolt;
        return 0;
}

/* sched_yield(): let other threads of the same priority run first. */
int
sys_yield(const uint64_t *args __unused, uint64_t *rv __unused)
{
        kthread_yield();
        return 0;
}
