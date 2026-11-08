/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Callouts: timeout(), untimeout(), and delay(), with the System V DDI
 * interface.
 *
 * Pending callouts sit on one list sorted by expiry tick. clock() runs
 * the expired ones from the bootstrap processor's timer interrupt, one
 * at a time, with callout_lock released while each runs. A callout
 * function therefore runs in interrupt context: it may wake threads but
 * never sleep.
 *
 * IRIX keeps callouts per processor; a single list is enough until
 * there are many of them.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>

struct callout {
        struct callout  *c_next;
        uint64_t        c_when;         /* lbolt at which it runs */
        void            (*c_fn)(void *);
        void            *c_arg;
        toid_t          c_id;
};

static struct spinlock callout_lock = SPINLOCK_INITIALIZER("callout");
static struct callout *callouts;
static struct kmem_zone *callout_zone;
static toid_t next_toid = 1;
static volatile toid_t callout_running;  /* id running now, or 0 */

static void delay_wakeup(void *);

void
callout_init(void)
{
        callout_zone = kmem_zone_init(sizeof(struct callout), "callout");
}

/*
 * Run fn(arg) from the clock interrupt after at least ticks ticks.
 * Returns an id for untimeout(). May be called from interrupt context.
 */
toid_t
timeout(void (*fn)(void *), void *arg, long ticks)
{
        struct callout *c = kmem_zone_alloc(callout_zone, KM_NOSLEEP);
        struct callout **link;
        toid_t id;
        int s;

        if (c == NULL)
                panic("timeout: out of memory");
        if (ticks < 1)
                ticks = 1;
        c->c_fn = fn;
        c->c_arg = arg;

        s = spin_lock(&callout_lock);
        id = next_toid++;
        c->c_id = id;
        c->c_when = lbolt + (uint64_t)ticks;
        for (link = &callouts; *link != NULL && (*link)->c_when <= c->c_when;
            link = &(*link)->c_next)
                continue;
        c->c_next = *link;
        *link = c;
        spin_unlock(&callout_lock, s);
        return id;
}

/*
 * Cancel callout id. Returns 1 if it was still pending. If it is running
 * on another processor, wait for it to finish, so that on return the
 * function is not running and will not run.
 */
int
untimeout(toid_t id)
{
        struct callout **link, *c = NULL;
        int s;

        s = spin_lock(&callout_lock);
        for (link = &callouts; *link != NULL; link = &(*link)->c_next) {
                if ((*link)->c_id == id) {
                        c = *link;
                        *link = c->c_next;
                        break;
                }
        }
        spin_unlock(&callout_lock, s);

        if (c != NULL) {
                kmem_zone_free(callout_zone, c);
                return 1;
        }
        while (callout_running == id && curcpu() != &cpu_info_primary)
                cpu_spinwait();
        return 0;
}

/* From clock(), on the bootstrap processor, in the timer interrupt. */
void
callout_tick(void)
{
        uint64_t now = lbolt;
        int s;

        if (callout_zone == NULL)
                return;
        s = spin_lock(&callout_lock);
        while (callouts != NULL && callouts->c_when <= now) {
                struct callout *c = callouts;

                callouts = c->c_next;
                callout_running = c->c_id;
                spin_unlock(&callout_lock, 0);
                c->c_fn(c->c_arg);
                kmem_zone_free(callout_zone, c);
                (void)spin_lock(&callout_lock);
                callout_running = 0;
        }
        spin_unlock(&callout_lock, s);
}

static void
delay_wakeup(void *arg)
{
        vsema(arg);
}

/* Sleep for at least ticks clock ticks. */
void
delay(long ticks)
{
        struct sema sm;

        if (ticks <= 0)
                return;
        sema_init(&sm, 0, "delay");
        (void)timeout(delay_wakeup, &sm, ticks);
        psema(&sm);
}
