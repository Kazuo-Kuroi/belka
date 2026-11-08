/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Spin locks.
 *
 *      int s = spin_lock(&lock);
 *      ...
 *      spin_unlock(&lock, s);
 *
 * Acquiring a spin lock raises the processor to splhi and returns the
 * previous level as a cookie, which spin_unlock() restores. Interrupts
 * stay enabled while waiting for a contended lock, if they were enabled
 * on entry, so a long wait does not delay interrupt delivery.
 *
 * Rules:
 * - Hold spin locks briefly. Never sleep or block while holding one.
 * - A spin lock is not recursive. Taking one already held by this
 *   processor panics.
 * - Locks may be released in any order, but each cookie must go back
 *   to the spin_unlock() that matches its spin_lock().
 * - Nested spin locks follow the lock order in doc/design/lock-order.md.
 */

#ifndef _SYS_SPINLOCK_H_
#define _SYS_SPINLOCK_H_

#include <stdint.h>

#define SPIN_NOCPU      UINT32_MAX

struct spinlock {
        volatile uint32_t sl_lock;      /* 0 free, 1 held */
        volatile uint32_t sl_cpu;       /* holder, or SPIN_NOCPU */
        const char      *sl_name;
};

#define SPINLOCK_INITIALIZER(name)      { 0, SPIN_NOCPU, (name) }

void spinlock_init(struct spinlock *, const char *);
int spin_lock(struct spinlock *);
int spin_trylock(struct spinlock *, int *);
void spin_unlock(struct spinlock *, int);
int spin_held(const struct spinlock *);

#endif /* _SYS_SPINLOCK_H_ */
