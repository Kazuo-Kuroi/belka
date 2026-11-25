/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ext2fs file data in the page cache.
 *
 * Regular files keep their data in the page cache of their vnode; the
 * buffer cache holds only metadata, directories, and symbolic links. A
 * page covers PAGESIZE / block size file blocks (a file system block is
 * never larger than a page). Pages are read and written straight to the
 * device, a block at a time, coalescing blocks that are adjacent on
 * disk.
 *
 * Blocks past the end of the file, and holes, read as zeroes. Write-back
 * writes only blocks inside the file that have disk blocks: the write
 * path allocates a block before it stores into it, and truncation zeroes
 * the tail of the last page and tosses the pages past it before it
 * frees any block, so a page never reaches a block that is no longer
 * the file's.
 *
 * A freed directory or indirect block may still sit in the buffer cache
 * when it is reallocated as file data; the write path discards it
 * (binvalblk) so that a delayed write cannot land on the new data.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <machine/param.h>

#include <vm/page.h>
#include <vm/pvn.h>

#include "fs/ext2fs/ext2fs.h"

/*
 * Run blocks [first, first + n) of page pp, which map to consecutive
 * disk blocks from pbn, through read or write.
 */
static int
page_io(struct ext2inode *ip, struct page *pp, uint32_t first, uint32_t n,
    uint32_t pbn, int writing)
{
        struct ext2mount *em = ip->i_mnt;
        char *va = (char *)page_to_kva(pp) + (size_t)first * em->em_bsize;
        uint64_t sector = (uint64_t)pbn * em->em_devsects;
        size_t nsec = (size_t)n * em->em_devsects;

        if (writing)
                return blkdev_write(em->em_dev, sector, va, nsec);
        return blkdev_read(em->em_dev, sector, va, nsec);
}

/*
 * Read or write every block of pp that is inside the file and has a
 * disk block; on reading, zero the rest. i_rwlock is held.
 */
static int
page_transfer(struct ext2inode *ip, struct page *pp, int writing)
{
        struct ext2mount *em = ip->i_mnt;
        uint32_t bpp = PAGESIZE / em->em_bsize;
        uint64_t lbn0 = pp->p_index * bpp;
        uint64_t eofblk = (ip->i_size + em->em_bsize - 1) / em->em_bsize;
        uint32_t run = 0, runpbn = 0, runfirst = 0;
        int error = 0;

        for (uint32_t i = 0; i <= bpp && error == 0; i++) {
                uint32_t pbn = 0;

                if (i < bpp && lbn0 + i < eofblk &&
                    (error = ext2fs_bmap(ip, lbn0 + i, &pbn, 0, NULL)) != 0)
                        break;
                if (run != 0 && (pbn == 0 || pbn != runpbn + run)) {
                        error = page_io(ip, pp, runfirst, run, runpbn,
                            writing);
                        run = 0;
                }
                if (i == bpp)
                        break;
                if (pbn == 0) {
                        if (!writing)
                                memset((char *)page_to_kva(pp) +
                                    (size_t)i * em->em_bsize, 0, em->em_bsize);
                        continue;
                }
                if (run == 0) {
                        runfirst = i;
                        runpbn = pbn;
                }
                run++;
        }
        return error;
}

/*
 * Return page idx of ip's file, held and filled. With overwrite, the
 * caller is about to store into every byte of a new page, so it is not
 * read. i_rwlock is held.
 */
int
ext2fs_getpage(struct ext2inode *ip, uint64_t idx, int overwrite,
    struct page **ppp)
{
        struct page *pp;
        int isnew, error;

        if ((pp = page_lookup_create(ITOV(ip), idx, &isnew)) == NULL)
                return ENOMEM;
        if (isnew) {
                if (!overwrite &&
                    (error = page_transfer(ip, pp, 0)) != 0) {
                        page_abort(pp);
                        return error;
                }
                page_unbusy(pp);
        }
        *ppp = pp;
        return 0;
}

/* Write every dirty page of ip back. i_rwlock is held. */
int
ext2fs_flushpages(struct ext2inode *ip)
{
        struct page *pp;
        int error = 0;

        if (ip->i_mnt->em_ronly)
                return 0;
        while ((pp = pvn_getdirty(ITOV(ip))) != NULL) {
                int e = page_transfer(ip, pp, 1);

                page_write_done(pp, e);
                if (e != 0) {
                        /* It is dirty again; leave it for a later pass. */
                        error = e;
                        break;
                }
        }
        return error;
}
