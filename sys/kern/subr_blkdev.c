/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Block device registry and range-checked I/O. See <sys/blkdev.h>.
 * Requests larger than a device's bd_maxio are split.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/errno.h>
#include <sys/synch.h>
#include <sys/systm.h>

static struct blkdev *blkdevs[BLKDEV_MAX];
static int nblkdevs;
static struct mutex blkdev_lock;
static int blkdev_ready;

int
blkdev_register(struct blkdev *bd, const char *prefix)
{
        int unit = 0;

        if (!blkdev_ready) {
                mutex_init(&blkdev_lock, "blkdev");
                blkdev_ready = 1;
        }
        mutex_lock(&blkdev_lock);
        if (nblkdevs == BLKDEV_MAX) {
                mutex_unlock(&blkdev_lock);
                return ENOSPC;
        }
        for (int i = 0; i < nblkdevs; i++) {
                if (strncmp(blkdevs[i]->bd_name, prefix, strlen(prefix)) == 0)
                        unit++;
        }
        snprintf(bd->bd_name, sizeof(bd->bd_name), "%s%d", prefix, unit);
        blkdevs[nblkdevs++] = bd;
        mutex_unlock(&blkdev_lock);
        printf("%s: %ju sectors of %u bytes (%ju MB)%s\n", bd->bd_name,
            (uintmax_t)bd->bd_nsectors, bd->bd_secsize,
            (uintmax_t)(bd->bd_nsectors * bd->bd_secsize / (1024 * 1024)),
            bd->bd_readonly ? ", read-only" : "");
        return 0;
}

struct blkdev *
blkdev_lookup(const char *name)
{
        struct blkdev *bd = NULL;

        if (!blkdev_ready)
                return NULL;
        mutex_lock(&blkdev_lock);
        for (int i = 0; i < nblkdevs; i++) {
                if (strcmp(blkdevs[i]->bd_name, name) == 0) {
                        bd = blkdevs[i];
                        break;
                }
        }
        mutex_unlock(&blkdev_lock);
        return bd;
}

static int
blkdev_check(const struct blkdev *bd, uint64_t sector, size_t nsec)
{
        if (nsec == 0)
                return 0;
        if (sector >= bd->bd_nsectors || nsec > bd->bd_nsectors - sector)
                return ENXIO;
        return 0;
}

int
blkdev_read(struct blkdev *bd, uint64_t sector, void *buf, size_t nsec)
{
        size_t per = bd->bd_maxio / bd->bd_secsize;
        int error = blkdev_check(bd, sector, nsec);

        while (error == 0 && nsec > 0) {
                size_t n = nsec < per ? nsec : per;

                error = bd->bd_ops->bo_read(bd, sector, buf, n);
                sector += n;
                buf = (char *)buf + n * bd->bd_secsize;
                nsec -= n;
        }
        return error;
}

int
blkdev_write(struct blkdev *bd, uint64_t sector, const void *buf, size_t nsec)
{
        size_t per = bd->bd_maxio / bd->bd_secsize;
        int error = blkdev_check(bd, sector, nsec);

        if (error == 0 && bd->bd_readonly)
                error = EROFS;
        while (error == 0 && nsec > 0) {
                size_t n = nsec < per ? nsec : per;

                error = bd->bd_ops->bo_write(bd, sector, buf, n);
                sector += n;
                buf = (const char *)buf + n * bd->bd_secsize;
                nsec -= n;
        }
        return error;
}

int
blkdev_flush(struct blkdev *bd)
{
        return bd->bd_ops->bo_flush != NULL ? bd->bd_ops->bo_flush(bd) : 0;
}
