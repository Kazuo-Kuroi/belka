/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Block devices, as file systems will see them: a named array of
 * fixed-size sectors with synchronous read, write, and flush.
 *
 * This is the simple interface for bring-up. Asynchronous buffers and
 * the buffer cache come with the VFS work; drivers will keep this
 * registration and gain a strategy routine.
 *
 * Every operation returns 0 or an errno value. Buffers may be anywhere
 * in kernel virtual memory. Thread context only.
 */

#ifndef _SYS_BLKDEV_H_
#define _SYS_BLKDEV_H_

#include <stddef.h>
#include <stdint.h>

#define BLKDEV_NAMELEN  16
#define BLKDEV_MAX      64              /* disks and their partitions */

struct blkdev;

struct blkdev_ops {
        int     (*bo_read)(struct blkdev *, uint64_t, void *, size_t);
        int     (*bo_write)(struct blkdev *, uint64_t, const void *, size_t);
        int     (*bo_flush)(struct blkdev *);
};

struct blkdev {
        char                    bd_name[BLKDEV_NAMELEN];
        uint32_t                bd_secsize;
        uint64_t                bd_nsectors;
        size_t                  bd_maxio;       /* bytes per request */
        int                     bd_readonly;
        const struct blkdev_ops *bd_ops;
        void                    *bd_softc;
        struct blkdev           *bd_parent;     /* a partition's disk, or NULL */
        uint64_t                bd_start;       /* its first sector there */
};

/* Register bd under a name built from prefix and a unit number. */
int blkdev_register(struct blkdev *, const char *);
/* Register bd under the name it already has (bd_name). */
int blkdev_register_name(struct blkdev *);

/*
 * Partitions (kern/subr_gpt.c): read each disk's GUID partition table
 * and register its partitions as disk p N (nvd0p1, vd3p2), after
 * every disk, once the drivers have attached.
 */
void blkdev_scan_partitions(void);

/*
 * The partitions disk's table has, without registering them, or -1 if
 * it has no good table; *backup says whether the backup was read.
 */
int blkdev_gpt_probe(struct blkdev *, int *);
struct blkdev *blkdev_lookup(const char *);
struct blkdev *blkdev_index(int);
struct vnode;
struct blkdev *devfs_blkdev(struct vnode *);  /* fs/devfs */

/*
 * How long a driver waits for one command, in clock ticks (30 s), before
 * it gives up and recovers the device; the self-tests shorten it.
 */
extern long blkdev_timeout;

/*
 * For the self-tests of that recovery: while nonzero, a driver loses the
 * next completion it sees (counting this down), as if the device had
 * never sent it.
 */
extern volatile uint32_t blkdev_lose;
int blkdev_lose_one(void);

/* Range-checked operations; nsec sectors starting at sector. */
int blkdev_read(struct blkdev *, uint64_t, void *, size_t);
int blkdev_write(struct blkdev *, uint64_t, const void *, size_t);
int blkdev_flush(struct blkdev *);

#endif /* _SYS_BLKDEV_H_ */
