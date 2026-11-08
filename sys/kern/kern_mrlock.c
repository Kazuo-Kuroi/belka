/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Multi-reader locks, after the IRIX mrlock.
 *
 * Any number of readers or one writer. A waiting writer stops new
 * readers from entering, so writers do not starve. Ownership is handed
 * over on release: when a writer leaves, every waiting reader is let in
 * at once (IRIX admits a batch of readers together); when the last
 * reader leaves, the first waiting writer gets the lock.
 */

#include <stddef.h>

#include <sys/kthread.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/intr.h>

static struct kthread *mr_admit_readers(struct mrlock *);

void
mrinit(struct mrlock *mr, const char *name)
{
        spinlock_init(&mr->mr_lock, "mrlock");
        mr->mr_readers = 0;
        mr->mr_writer = 0;
        sleepq_init(&mr->mr_rwait);
        sleepq_init(&mr->mr_wwait);
        mr->mr_name = name;
}

void
mraccess(struct mrlock *mr)
{
        int s = spin_lock(&mr->mr_lock);

        if (!mr->mr_writer && mr->mr_wwait.sq_count == 0) {
                mr->mr_readers++;
                spin_unlock(&mr->mr_lock, s);
                return;
        }
        sleepq_insert(&mr->mr_rwait, curthread, SQ_FIFO);
        spin_unlock(&mr->mr_lock, s);
        sleep_finish();         /* the releaser counted us in */
}

void
mrupdate(struct mrlock *mr)
{
        int s = spin_lock(&mr->mr_lock);

        if (!mr->mr_writer && mr->mr_readers == 0) {
                mr->mr_writer = 1;
                spin_unlock(&mr->mr_lock, s);
                return;
        }
        sleepq_insert(&mr->mr_wwait, curthread, SQ_FIFO);
        spin_unlock(&mr->mr_lock, s);
        sleep_finish();         /* the releaser made us the writer */
}

int
mrtryaccess(struct mrlock *mr)
{
        int s = spin_lock(&mr->mr_lock);
        int got = 0;

        if (!mr->mr_writer && mr->mr_wwait.sq_count == 0) {
                mr->mr_readers++;
                got = 1;
        }
        spin_unlock(&mr->mr_lock, s);
        return got;
}

int
mrtryupdate(struct mrlock *mr)
{
        int s = spin_lock(&mr->mr_lock);
        int got = 0;

        if (!mr->mr_writer && mr->mr_readers == 0) {
                mr->mr_writer = 1;
                got = 1;
        }
        spin_unlock(&mr->mr_lock, s);
        return got;
}

/*
 * Let every waiting reader in. mr_lock is held. Returns the readers for
 * wakeup_threads() once it is dropped.
 */
static struct kthread *
mr_admit_readers(struct mrlock *mr)
{
        mr->mr_readers += mr->mr_rwait.sq_count;
        return sleepq_remove_all(&mr->mr_rwait);
}

/* Release a read or write hold. */
void
mrunlock(struct mrlock *mr)
{
        int s = spin_lock(&mr->mr_lock);
        int was_writer = mr->mr_writer;
        struct kthread *readers = NULL, *writer = NULL;

        if (was_writer) {
                mr->mr_writer = 0;
        } else {
                if (mr->mr_readers <= 0)
                        panic("mrunlock: %s is not held", mr->mr_name);
                mr->mr_readers--;
        }

        if (mr->mr_readers == 0) {
                if (was_writer && mr->mr_rwait.sq_count != 0) {
                        readers = mr_admit_readers(mr);
                } else if (mr->mr_wwait.sq_count != 0) {
                        mr->mr_writer = 1;
                        writer = sleepq_remove_first(&mr->mr_wwait);
                } else {
                        readers = mr_admit_readers(mr);
                }
        }
        spin_unlock(&mr->mr_lock, 0);
        if (writer != NULL)
                wakeup_thread(writer);
        (void)wakeup_threads(readers);
        splx(s);
}
