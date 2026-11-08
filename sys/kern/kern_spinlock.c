/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Spin locks: test and test-and-set, with interrupts enabled between
 * attempts. See <sys/spinlock.h> for the rules.
 *
 * Fairness is not guaranteed. A queued (ticket or MCS) lock would be,
 * but it cannot let interrupts in while a processor waits for its turn,
 * because the turn might come while the processor is in an interrupt
 * handler. Contended locks are a design problem to fix at the caller.
 */

#include <stdint.h>

#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/intr.h>

/*
 * Waiting this many polls for one lock means a deadlock or a lost
 * release; panic rather than hang silently.
 */
#define SPIN_LIMIT      (UINT64_C(1) << 32)

void
spinlock_init(struct spinlock *lk, const char *name)
{
        lk->sl_lock = 0;
        lk->sl_cpu = SPIN_NOCPU;
        lk->sl_name = name;
}

/* Try once with interrupts blocked; on success leave them blocked. */
static int
spin_try(struct spinlock *lk, int *sp)
{
        int s = splhi();

        if (atomic_cas_32(&lk->sl_lock, 0, 1) == 0) {
                membar_enter();
                lk->sl_cpu = cpu_number();
                *sp = s;
                return 1;
        }
        splx(s);
        return 0;
}

int
spin_lock(struct spinlock *lk)
{
        uint64_t spins = 0;
        int s;

        if (lk->sl_cpu == cpu_number() && lk->sl_lock != 0)
                panic("spin_lock: %s already held by this cpu", lk->sl_name);

        while (!spin_try(lk, &s)) {
                while (atomic_load_32(&lk->sl_lock) != 0) {
                        cpu_spinwait();
                        if (++spins == SPIN_LIMIT)
                                panic("spin_lock: %s stuck, held by cpu %u",
                                    lk->sl_name, lk->sl_cpu);
                }
        }
        return s;
}

/*
 * Take the lock only if it is free. Returns 1 and stores the cookie in
 * *sp on success, or returns 0 with the interrupt level unchanged.
 */
int
spin_trylock(struct spinlock *lk, int *sp)
{
        return spin_try(lk, sp);
}

void
spin_unlock(struct spinlock *lk, int s)
{
        if (lk->sl_lock == 0 || lk->sl_cpu != cpu_number())
                panic("spin_unlock: %s not held by this cpu", lk->sl_name);
        lk->sl_cpu = SPIN_NOCPU;
        membar_exit();
        atomic_store_32(&lk->sl_lock, 0);
        splx(s);
}

/* Nonzero if this processor holds the lock; for assertions. */
int
spin_held(const struct spinlock *lk)
{
        return lk->sl_lock != 0 && lk->sl_cpu == cpu_number();
}
