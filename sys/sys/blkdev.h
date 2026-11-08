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
#define BLKDEV_MAX      16

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
};

/* Register bd under a name built from prefix and a unit number. */
int blkdev_register(struct blkdev *, const char *);
struct blkdev *blkdev_lookup(const char *);

/* Range-checked operations; nsec sectors starting at sector. */
int blkdev_read(struct blkdev *, uint64_t, void *, size_t);
int blkdev_write(struct blkdev *, uint64_t, const void *, size_t);
int blkdev_flush(struct blkdev *);

#endif /* _SYS_BLKDEV_H_ */
