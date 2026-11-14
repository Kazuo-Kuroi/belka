/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Interval timers: setitimer() and getitimer() (UnixWare's 169 and
 * 170), and alarm() (27), which is ITIMER_REAL without an interval.
 * Each process has three, kept in clock ticks:
 *
 *   ITIMER_REAL     real time, a callout; SIGALRM
 *   ITIMER_VIRTUAL  the process's user time, counted down at the clock
 *                   tick (sched_tick); SIGVTALRM
 *   ITIMER_PROF     its user and system time, likewise; SIGPROF
 *
 * A timer with an interval is reloaded with it when it expires. The
 * timers are not inherited by fork, last across exec, and the real one
 * is cancelled at exit.
 *
 * The real timer's state is under p_siglock, which the callout takes;
 * cancelling it zeroes p_alarm first, so a callout already on its way
 * does nothing, and untimeout() then waits for it to finish. The other
 * two are counted down by compare-and-swap from any processor's tick.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/spinlock.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/time.h>

#define USEC            1000000

/* A time from user mode in ticks, rounded up; EINVAL if malformed. */
static int
tv_to_ticks(const struct timeval *tv, uint64_t *ticks)
{
        if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= USEC ||
            tv->tv_sec > (int64_t)(UINT32_MAX / HZ))
                return EINVAL;
        *ticks = (uint64_t)tv->tv_sec * HZ +
            ((uint64_t)tv->tv_usec * HZ + USEC - 1) / USEC;
        return 0;
}

static void
ticks_to_tv(uint64_t ticks, struct timeval *tv)
{
        tv->tv_sec = (int64_t)(ticks / HZ);
        tv->tv_usec = (int64_t)(ticks % HZ) * (USEC / HZ);
}

/* --- ITIMER_REAL --- */

static void
real_fire(void *arg)
{
        struct proc *p = arg;
        int s = spin_lock(&p->p_siglock);

        if (p->p_alarm == 0) {          /* cancelled on the way */
                spin_unlock(&p->p_siglock, s);
                return;
        }
        if (p->p_alarm_ival != 0) {
                p->p_alarm_when = lbolt + p->p_alarm_ival;
                p->p_alarm = timeout(real_fire, p, (long)p->p_alarm_ival);
        } else {
                p->p_alarm = 0;
        }
        spin_unlock(&p->p_siglock, s);
        psignal(p, SIGALRM);
}

/*
 * Cancel p's real timer. *leftp, if not NULL, gets the ticks that were
 * left (0 if it was not set or is firing), *ivalp its interval.
 */
static void
real_cancel(struct proc *p, uint64_t *leftp, uint64_t *ivalp)
{
        uint64_t when, ival;
        toid_t id;
        int s = spin_lock(&p->p_siglock);

        id = p->p_alarm;
        when = p->p_alarm_when;
        ival = p->p_alarm_ival;
        p->p_alarm = 0;
        p->p_alarm_ival = 0;
        spin_unlock(&p->p_siglock, s);
        if (id != 0 && !untimeout(id))
                id = 0;
        if (leftp != NULL)
                *leftp = id == 0 ? 0 : when > lbolt ? when - lbolt : 1;
        if (ivalp != NULL)
                *ivalp = id != 0 ? ival : 0;
}

/*
 * Set p's real timer to expire in ticks ticks (none if 0), then every
 * ival ticks. Already cancelled by the caller. The callout is set a
 * tick later than the expiry recorded, since it runs at a tick
 * boundary, which may come at once: the time asked is the least that
 * passes, and the time left reads as asked.
 */
static void
real_arm(struct proc *p, uint64_t ticks, uint64_t ival)
{
        int s;

        if (ticks == 0)
                return;
        s = spin_lock(&p->p_siglock);
        p->p_alarm_ival = ival;
        p->p_alarm_when = lbolt + ticks;
        p->p_alarm = timeout(real_fire, p, (long)ticks + 1);
        spin_unlock(&p->p_siglock, s);
}

/* --- ITIMER_VIRTUAL and ITIMER_PROF --- */

/* Count one tick off timer t; returns 1 if it expired. */
static int
cpu_timer_tick(struct proc *p, int t)
{
        uint64_t v = __atomic_load_n(&p->p_itvalue[t], __ATOMIC_RELAXED);

        while (v != 0) {
                uint64_t n = v > 1 ? v - 1 :
                    __atomic_load_n(&p->p_itival[t], __ATOMIC_RELAXED);

                if (__atomic_compare_exchange_n(&p->p_itvalue[t], &v, n, 0,
                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                        return v == 1;
        }
        return 0;
}

/*
 * From sched_tick(), in the clock interrupt: a tick of kt's, in user
 * mode if user is set, counts against its process's processor timers.
 */
void
itimer_tick(struct kthread *kt, int user)
{
        struct proc *p;

        if (kt->kt_lwp == NULL)
                return;
        p = kt->kt_lwp->l_proc;
        if (user && cpu_timer_tick(p, ITIMER_VIRTUAL - 1))
                psignal(p, SIGVTALRM);
        if (cpu_timer_tick(p, ITIMER_PROF - 1))
                psignal(p, SIGPROF);
}

/* --- The calls. --- */

/* At exit, and when the process's timers go. */
void
itimer_exit(struct proc *p)
{
        real_cancel(p, NULL, NULL);
}

static void
itimer_get(struct proc *p, int which, struct itimerval *itv)
{
        uint64_t left, ival;
        int s;

        if (which == ITIMER_REAL) {
                s = spin_lock(&p->p_siglock);
                left = p->p_alarm == 0 ? 0 : p->p_alarm_when > lbolt ?
                    p->p_alarm_when - lbolt : 1;     /* due */
                ival = p->p_alarm != 0 ? p->p_alarm_ival : 0;
                spin_unlock(&p->p_siglock, s);
        } else {
                left = __atomic_load_n(&p->p_itvalue[which - 1],
                    __ATOMIC_RELAXED);
                ival = __atomic_load_n(&p->p_itival[which - 1],
                    __ATOMIC_RELAXED);
        }
        ticks_to_tv(left, &itv->it_value);
        ticks_to_tv(ival, &itv->it_interval);
}

/* getitimer(which, struct itimerval *) */
int
sys_getitimer(const uint64_t *args, uint64_t *rv __unused)
{
        struct itimerval itv;
        int which = (int)args[0];

        if (which != ITIMER_REAL && which != ITIMER_VIRTUAL &&
            which != ITIMER_PROF)
                return EINVAL;
        itimer_get(curproc, which, &itv);
        return copyout(&itv, (void *)(uintptr_t)args[1], sizeof(itv));
}

/*
 * setitimer(which, const struct itimerval *, struct itimerval *old):
 * a zero value disarms the timer.
 */
int
sys_setitimer(const uint64_t *args, uint64_t *rv __unused)
{
        struct proc *p = curproc;
        struct itimerval itv, old;
        uint64_t value, ival, left, oival;
        int which = (int)args[0], error;

        if (which != ITIMER_REAL && which != ITIMER_VIRTUAL &&
            which != ITIMER_PROF)
                return EINVAL;
        if ((error = copyin((const void *)(uintptr_t)args[1], &itv,
            sizeof(itv))) != 0)
                return error;
        if ((error = tv_to_ticks(&itv.it_value, &value)) != 0 ||
            (error = tv_to_ticks(&itv.it_interval, &ival)) != 0)
                return error;
        if (which == ITIMER_REAL) {
                real_cancel(p, &left, &oival);
                ticks_to_tv(left, &old.it_value);
                ticks_to_tv(oival, &old.it_interval);
                real_arm(p, value, ival);
        } else {
                /* The interval first, so an expiry reloads the new one. */
                itimer_get(p, which, &old);
                __atomic_store_n(&p->p_itival[which - 1], ival,
                    __ATOMIC_RELAXED);
                __atomic_store_n(&p->p_itvalue[which - 1], value,
                    __ATOMIC_RELAXED);
        }
        if (args[2] == 0)
                return 0;
        return copyout(&old, (void *)(uintptr_t)args[2], sizeof(old));
}

/* alarm(secs): the real timer in whole seconds; returns those left. */
int
sys_alarm(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;
        uint64_t secs = (uint32_t)args[0], left;

        real_cancel(p, &left, NULL);
        rv[0] = (left + HZ - 1) / HZ;
        real_arm(p, secs * HZ, 0);
        return 0;
}
