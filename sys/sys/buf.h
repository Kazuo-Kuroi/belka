/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The buffer cache: blocks of block devices, after the System V and
 * IRIX bio interface.
 *
 * A buffer caches b_bcount bytes starting at sector b_blkno of b_dev.
 * getblk() and bread() return it busy (B_BUSY), owned by the caller
 * alone until brelse(), bwrite(), or bdwrite() gives it back. B_DONE
 * means the data is valid; B_DELWRI means it is newer than the disk and
 * must be written before the buffer is reused.
 *
 * bdwrite() is a delayed write: the bdflush thread writes such buffers
 * once they are BDFLUSH_AGE ticks old, and bflush() writes them at once.
 * The block devices are synchronous for now, so bawrite() is bwrite().
 *
 * Locking: bc_lock (a spin lock) protects the hash chains, the free
 * list, and every buffer's flags; it is never held during I/O. Busy
 * buffers are waited for on one sync variable.
 */

#ifndef _SYS_BUF_H_
#define _SYS_BUF_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

struct blkdev;

struct buf {
        struct buf      *b_hnext;       /* hash chain */
        struct buf      *b_hprev;
        struct buf      *b_fnext;       /* free list, least recent first */
        struct buf      *b_fprev;
        struct blkdev   *b_dev;
        daddr_t         b_blkno;
        size_t          b_bcount;       /* bytes of valid range */
        size_t          b_bufsize;      /* bytes allocated */
        void            *b_data;
        volatile uint32_t b_flags;
        int             b_error;
        uint64_t        b_dirtied;      /* lbolt when B_DELWRI was set */
};

#define B_BUSY          0x0001
#define B_DONE          0x0002  /* data valid */
#define B_DELWRI        0x0004  /* must be written before reuse */
#define B_ERROR         0x0008
#define B_INVAL         0x0010  /* contents meaningless; reuse first */
#define B_ONFREE        0x0020  /* on the free list */

#define BDFLUSH_AGE     300     /* ticks a delayed write may wait */

struct bc_stats {
        uint64_t        bs_hits;
        uint64_t        bs_misses;
        uint64_t        bs_reads;
        uint64_t        bs_writes;
        uint64_t        bs_delwri_flushed; /* written by bdflush */
        uint64_t        bs_invalidated; /* blocks dropped by binvalblk */
        uint32_t        bs_nbuf;
        uint32_t        bs_ndirty;
};

void binit(void);
struct buf *getblk(struct blkdev *, daddr_t, size_t);
int bread(struct blkdev *, daddr_t, size_t, struct buf **);
int bwrite(struct buf *);
int bawrite(struct buf *);
void bdwrite(struct buf *);
void brelse(struct buf *);
void bclrbuf(struct buf *);

/* Write every delayed buffer of dev (all devices if NULL). */
int bflush(struct blkdev *);
/* Forget every buffer of dev, which must have none busy or dirty. */
void binval(struct blkdev *);
void binvalblk(struct blkdev *, daddr_t);
void bc_get_stats(struct bc_stats *);

#endif /* _SYS_BUF_H_ */
