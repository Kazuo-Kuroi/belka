/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The buffer cache. See <sys/buf.h>.
 *
 * Buffers are allocated on demand up to bc_maxbuf; past that, getblk()
 * takes the least recently used free buffer, writing it first if it is
 * a delayed write. Buffers that held an error or were invalidated go to
 * the front of the free list so they are reused first; others go to the
 * back.
 *
 * Data comes from kmem and is resized when a buffer is reused for a
 * different size. The bdflush thread wakes once a second and writes
 * delayed buffers older than BDFLUSH_AGE ticks.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#define BC_HASHSIZE     256
#define BC_MAXBUF       1024

static struct spinlock bc_lock = SPINLOCK_INITIALIZER("bcache");
static struct sv bc_wait;               /* a buffer was released */
static struct buf *bc_hash[BC_HASHSIZE];
static struct buf *bc_free_head, *bc_free_tail;
static struct kmem_zone *buf_zone;
static unsigned int bc_maxbuf = BC_MAXBUF;
static struct bc_stats bc_st;

static unsigned int bc_hashfn(const struct blkdev *, daddr_t);
static struct buf *bc_find(const struct blkdev *, daddr_t);
static void bc_hash_insert(struct buf *);
static void bc_hash_remove(struct buf *);
static void bc_free_remove(struct buf *);
static void bc_free_insert(struct buf *, int);
static int bc_io(struct buf *, int);
static struct buf *bc_take_dirty(struct blkdev *, uint64_t);
static void bdflush(void *);

static unsigned int
bc_hashfn(const struct blkdev *dev, daddr_t blkno)
{
        return (unsigned int)(((uintptr_t)dev >> 6) + (uint64_t)blkno) %
            BC_HASHSIZE;
}

static struct buf *
bc_find(const struct blkdev *dev, daddr_t blkno)
{
        for (struct buf *bp = bc_hash[bc_hashfn(dev, blkno)]; bp != NULL;
            bp = bp->b_hnext) {
                if (bp->b_dev == dev && bp->b_blkno == blkno)
                        return bp;
        }
        return NULL;
}

static void
bc_hash_insert(struct buf *bp)
{
        unsigned int h = bc_hashfn(bp->b_dev, bp->b_blkno);

        bp->b_hprev = NULL;
        bp->b_hnext = bc_hash[h];
        if (bc_hash[h] != NULL)
                bc_hash[h]->b_hprev = bp;
        bc_hash[h] = bp;
}

static void
bc_hash_remove(struct buf *bp)
{
        if (bp->b_hprev != NULL)
                bp->b_hprev->b_hnext = bp->b_hnext;
        else if (bp->b_dev != NULL)
                bc_hash[bc_hashfn(bp->b_dev, bp->b_blkno)] = bp->b_hnext;
        if (bp->b_hnext != NULL)
                bp->b_hnext->b_hprev = bp->b_hprev;
        bp->b_hnext = bp->b_hprev = NULL;
}

static void
bc_free_remove(struct buf *bp)
{
        if (bp->b_fprev != NULL)
                bp->b_fprev->b_fnext = bp->b_fnext;
        else
                bc_free_head = bp->b_fnext;
        if (bp->b_fnext != NULL)
                bp->b_fnext->b_fprev = bp->b_fprev;
        else
                bc_free_tail = bp->b_fprev;
        bp->b_fnext = bp->b_fprev = NULL;
        bp->b_flags &= ~(uint32_t)B_ONFREE;
}

static void
bc_free_insert(struct buf *bp, int front)
{
        if (front) {
                bp->b_fprev = NULL;
                bp->b_fnext = bc_free_head;
                if (bc_free_head != NULL)
                        bc_free_head->b_fprev = bp;
                else
                        bc_free_tail = bp;
                bc_free_head = bp;
        } else {
                bp->b_fnext = NULL;
                bp->b_fprev = bc_free_tail;
                if (bc_free_tail != NULL)
                        bc_free_tail->b_fnext = bp;
                else
                        bc_free_head = bp;
                bc_free_tail = bp;
        }
        bp->b_flags |= B_ONFREE;
}

/* Read or write a busy buffer's range. Called without bc_lock. */
static int
bc_io(struct buf *bp, int write)
{
        size_t nsec = bp->b_bcount / bp->b_dev->bd_secsize;
        int error;

        if (write)
                error = blkdev_write(bp->b_dev, (uint64_t)bp->b_blkno,
                    bp->b_data, nsec);
        else
                error = blkdev_read(bp->b_dev, (uint64_t)bp->b_blkno,
                    bp->b_data, nsec);
        return error;
}

void
binit(void)
{
        sv_init(&bc_wait, "bcache");
        buf_zone = kmem_zone_init(sizeof(struct buf), "buf");
        (void)kthread_create(bdflush, NULL, PRI_KERNEL, "bdflush");
}

/*
 * Return the buffer for size bytes at blkno of dev, busy. Its data is
 * valid only if B_DONE is set.
 */
struct buf *
getblk(struct blkdev *dev, daddr_t blkno, size_t size)
{
        struct buf *bp;
        int s;

        if (size == 0 || size % dev->bd_secsize != 0)
                panic("getblk: bad size %zu", size);
again:
        s = spin_lock(&bc_lock);
        if ((bp = bc_find(dev, blkno)) != NULL) {
                if ((bp->b_flags & B_BUSY) != 0) {
                        sv_wait(&bc_wait, &bc_lock, s);
                        goto again;
                }
                bp->b_flags |= B_BUSY;
                bc_free_remove(bp);
                bc_st.bs_hits++;
                spin_unlock(&bc_lock, s);
                if (bp->b_bcount != size) {
                        /* Same block at another size: start afresh. */
                        if ((bp->b_flags & B_DELWRI) != 0) {
                                (void)bc_io(bp, 1);
                                s = spin_lock(&bc_lock);
                                bc_st.bs_writes++;
                                bc_st.bs_ndirty--;
                                spin_unlock(&bc_lock, s);
                        }
                        bp->b_flags &= ~(uint32_t)(B_DONE | B_DELWRI);
                        if (bp->b_bufsize < size) {
                                kmem_free(bp->b_data, bp->b_bufsize);
                                bp->b_data = kmem_alloc(size, KM_SLEEP);
                                bp->b_bufsize = size;
                        }
                        bp->b_bcount = size;
                }
                return bp;
        }
        bc_st.bs_misses++;

        if (bc_st.bs_nbuf < bc_maxbuf || bc_free_head == NULL) {
                bc_st.bs_nbuf++;
                spin_unlock(&bc_lock, s);
                bp = kmem_zone_zalloc(buf_zone, KM_SLEEP);
                bp->b_data = kmem_alloc(size, KM_SLEEP);
                bp->b_bufsize = size;
        } else {
                bp = bc_free_head;
                bc_free_remove(bp);
                bp->b_flags |= B_BUSY;
                bc_hash_remove(bp);
                spin_unlock(&bc_lock, s);
                if ((bp->b_flags & B_DELWRI) != 0) {
                        (void)bc_io(bp, 1);
                        bc_st.bs_writes++;
                        s = spin_lock(&bc_lock);
                        bc_st.bs_ndirty--;
                        spin_unlock(&bc_lock, s);
                }
                if (bp->b_bufsize < size) {
                        kmem_free(bp->b_data, bp->b_bufsize);
                        bp->b_data = kmem_alloc(size, KM_SLEEP);
                        bp->b_bufsize = size;
                }
        }

        bp->b_dev = dev;
        bp->b_blkno = blkno;
        bp->b_bcount = size;
        bp->b_error = 0;
        bp->b_flags = B_BUSY;

        /* Another thread may have brought the block in meanwhile. */
        s = spin_lock(&bc_lock);
        if (bc_find(dev, blkno) != NULL) {
                bp->b_dev = NULL;
                bp->b_flags = B_INVAL | B_ONFREE;
                bc_free_insert(bp, 1);
                bp->b_flags &= ~(uint32_t)B_BUSY;
                spin_unlock(&bc_lock, s);
                goto again;
        }
        bc_hash_insert(bp);
        spin_unlock(&bc_lock, s);
        return bp;
}

/* Return the block, reading it if not cached. */
int
bread(struct blkdev *dev, daddr_t blkno, size_t size, struct buf **bpp)
{
        struct buf *bp = getblk(dev, blkno, size);
        int error = 0;

        if ((bp->b_flags & B_DONE) == 0) {
                bc_st.bs_reads++;
                if ((error = bc_io(bp, 0)) != 0) {
                        bp->b_flags |= B_ERROR;
                        bp->b_error = error;
                        brelse(bp);
                        *bpp = NULL;
                        return error;
                }
                bp->b_flags |= B_DONE;
        }
        *bpp = bp;
        return 0;
}

/* Fill a buffer with zeros and mark its contents valid. */
void
bclrbuf(struct buf *bp)
{
        memset(bp->b_data, 0, bp->b_bcount);
        bp->b_flags |= B_DONE;
}

/* Write a buffer now and release it. */
int
bwrite(struct buf *bp)
{
        int error = bc_io(bp, 1);
        int s;

        s = spin_lock(&bc_lock);
        bc_st.bs_writes++;
        if ((bp->b_flags & B_DELWRI) != 0)
                bc_st.bs_ndirty--;
        bp->b_flags &= ~(uint32_t)B_DELWRI;
        if (error != 0) {
                bp->b_flags |= B_ERROR;
                bp->b_error = error;
        } else {
                bp->b_flags |= B_DONE;
        }
        spin_unlock(&bc_lock, s);
        brelse(bp);
        return error;
}

int
bawrite(struct buf *bp)
{
        return bwrite(bp);
}

/* Mark a buffer modified and release it; it is written later. */
void
bdwrite(struct buf *bp)
{
        int s = spin_lock(&bc_lock);

        if ((bp->b_flags & B_DELWRI) == 0) {
                bp->b_flags |= B_DELWRI;
                bp->b_dirtied = lbolt;
                bc_st.bs_ndirty++;
        }
        bp->b_flags |= B_DONE;
        spin_unlock(&bc_lock, s);
        brelse(bp);
}

void
brelse(struct buf *bp)
{
        int s = spin_lock(&bc_lock);

        if ((bp->b_flags & (B_ERROR | B_INVAL)) != 0 &&
            (bp->b_flags & B_DELWRI) == 0) {
                bp->b_flags &= ~(uint32_t)(B_DONE | B_ERROR);
                bc_free_insert(bp, 1);
        } else {
                bc_free_insert(bp, 0);
        }
        bp->b_flags &= ~(uint32_t)B_BUSY;
        spin_unlock(&bc_lock, s);
        (void)sv_broadcast(&bc_wait);
}

/*
 * Take one delayed-write buffer of dev (any device if NULL) older than
 * age ticks, busy, or return NULL.
 */
static struct buf *
bc_take_dirty(struct blkdev *dev, uint64_t age)
{
        struct buf *found = NULL;
        int s = spin_lock(&bc_lock);

        for (struct buf *bp = bc_free_head; bp != NULL; bp = bp->b_fnext) {
                if ((bp->b_flags & B_DELWRI) != 0 &&
                    (dev == NULL || bp->b_dev == dev) &&
                    lbolt - bp->b_dirtied >= age) {
                        bc_free_remove(bp);
                        bp->b_flags |= B_BUSY;
                        found = bp;
                        break;
                }
        }
        spin_unlock(&bc_lock, s);
        return found;
}

int
bflush(struct blkdev *dev)
{
        struct buf *bp;
        int error = 0;

        while ((bp = bc_take_dirty(dev, 0)) != NULL) {
                int e = bwrite(bp);

                if (e != 0 && error == 0)
                        error = e;
        }
        return error;
}

void
binval(struct blkdev *dev)
{
        int s = spin_lock(&bc_lock);

        for (struct buf *bp = bc_free_head; bp != NULL; bp = bp->b_fnext) {
                if (bp->b_dev != dev)
                        continue;
                if ((bp->b_flags & (B_BUSY | B_DELWRI)) != 0)
                        panic("binval: buffer for %s still in use",
                            dev->bd_name);
                bc_hash_remove(bp);
                bp->b_dev = NULL;
                bp->b_flags = B_INVAL | B_ONFREE;
        }
        spin_unlock(&bc_lock, s);
}

/*
 * Forget any cached copy of block blkno of dev, discarding a delayed
 * write: the block has a new owner that does not go through the buffer
 * cache (file data, in the page cache).
 */
void
binvalblk(struct blkdev *dev, daddr_t blkno)
{
        struct buf *bp;
        int s;

again:
        s = spin_lock(&bc_lock);
        if ((bp = bc_find(dev, blkno)) == NULL) {
                spin_unlock(&bc_lock, s);
                return;
        }
        if ((bp->b_flags & B_BUSY) != 0) {
                sv_wait(&bc_wait, &bc_lock, s);
                goto again;
        }
        if ((bp->b_flags & B_DELWRI) != 0)
                bc_st.bs_ndirty--;
        bc_hash_remove(bp);
        bp->b_dev = NULL;
        bp->b_flags = B_INVAL | B_ONFREE;
        bc_st.bs_invalidated++;
        spin_unlock(&bc_lock, s);
}

/*
 * Write delayed buffers once they are BDFLUSH_AGE ticks old, and file
 * pages once their vnode has been dirty that long.
 */
static void
bdflush(void *arg __unused)
{
        for (;;) {
                struct buf *bp;

                delay(HZ);
                while ((bp = bc_take_dirty(NULL, BDFLUSH_AGE)) != NULL) {
                        (void)bwrite(bp);
                        bc_st.bs_delwri_flushed++;
                }
                vn_flush_aged(BDFLUSH_AGE);
        }
}

void
bc_get_stats(struct bc_stats *st)
{
        int s = spin_lock(&bc_lock);

        *st = bc_st;
        spin_unlock(&bc_lock, s);
}
