/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ext2fs block and inode allocation, with the policies NetBSD's ext2fs
 * uses:
 *
 * - A block goes at the caller's preference, normally just after the
 *   file's previous block, else the next free block after it in that
 *   group, else the first free block of a later group.
 * - A directory's inode goes in a group with at least the average number
 *   of free inodes, choosing the one with the most free blocks, which
 *   spreads directories out. Any other inode goes in its parent
 *   directory's group, else the next group with a free inode.
 *
 * Bitmaps are read and written through the buffer cache. The group
 * descriptors and superblock counts are updated in core and written by
 * ext2fs_sbupdate(). Every function here is called with em_lock held.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/systm.h>

#include "fs/ext2fs/ext2fs.h"

static int bitmap_find(const uint8_t *, uint32_t, uint32_t);
static uint32_t group_nblocks(const struct ext2mount *, uint32_t);
static uint32_t dirpref(struct ext2mount *);

/* The first block of group g. */
uint32_t
ext2fs_group_first_block(const struct ext2mount *em, uint32_t g)
{
        return em->em_sb->s_first_data_block +
            g * em->em_sb->s_blocks_per_group;
}

/* The number of blocks in group g; the last may be short. */
static uint32_t
group_nblocks(const struct ext2mount *em, uint32_t g)
{
        uint32_t first = ext2fs_group_first_block(em, g);
        uint32_t n = em->em_sb->s_blocks_per_group;

        if (first + n > em->em_sb->s_blocks_count)
                n = em->em_sb->s_blocks_count - first;
        return n;
}

/* A clear bit in [start, n), else in [0, start), or -1. */
static int
bitmap_find(const uint8_t *map, uint32_t n, uint32_t start)
{
        for (uint32_t pass = 0; pass < 2; pass++) {
                uint32_t from = pass == 0 ? start : 0;
                uint32_t to = pass == 0 ? n : start;

                for (uint32_t i = from; i < to; i++) {
                        if ((i & 7) == 0 && map[i >> 3] == 0xff && i + 8 <= to) {
                                i += 7;
                                continue;
                        }
                        if ((map[i >> 3] & (1u << (i & 7))) == 0)
                                return (int)i;
                }
        }
        return -1;
}

/* Allocate a block near pref; *bnop gets it. ENOSPC if the disk is full. */
int
ext2fs_balloc_block(struct ext2mount *em, uint32_t pref, uint32_t *bnop)
{
        struct ext2fs_sb *sb = em->em_sb;
        uint32_t g0;

        if (sb->s_free_blocks_count == 0)
                return ENOSPC;
        if (pref < sb->s_first_data_block || pref >= sb->s_blocks_count)
                pref = sb->s_first_data_block;
        g0 = (pref - sb->s_first_data_block) / sb->s_blocks_per_group;

        for (uint32_t k = 0; k < em->em_ngroups; k++) {
                uint32_t g = (g0 + k) % em->em_ngroups;
                struct ext2fs_gd *gd = &em->em_gd[g];
                uint32_t start = k == 0 ?
                    pref - ext2fs_group_first_block(em, g) : 0;
                struct buf *bp;
                int bit, error;

                if (gd->bg_free_blocks_count == 0)
                        continue;
                if ((error = ext2fs_bread(em, gd->bg_block_bitmap, &bp)) != 0)
                        return error;
                bit = bitmap_find(bp->b_data, group_nblocks(em, g), start);
                if (bit < 0) {
                        /* The count said otherwise; trust the bitmap. */
                        brelse(bp);
                        continue;
                }
                ((uint8_t *)bp->b_data)[bit >> 3] |= (uint8_t)(1u << (bit & 7));
                bdwrite(bp);
                gd->bg_free_blocks_count--;
                sb->s_free_blocks_count--;
                ext2fs_sbdirty(em);
                *bnop = ext2fs_group_first_block(em, g) + (uint32_t)bit;
                return 0;
        }
        return ENOSPC;
}

void
ext2fs_bfree(struct ext2mount *em, uint32_t bno)
{
        struct ext2fs_sb *sb = em->em_sb;
        uint32_t g, bit;
        struct buf *bp;
        uint8_t *map;

        if (bno < sb->s_first_data_block || bno >= sb->s_blocks_count) {
                printf("ext2fs: freeing bad block %u\n", bno);
                return;
        }
        g = (bno - sb->s_first_data_block) / sb->s_blocks_per_group;
        bit = (bno - sb->s_first_data_block) % sb->s_blocks_per_group;
        if (ext2fs_bread(em, em->em_gd[g].bg_block_bitmap, &bp) != 0)
                return;
        map = bp->b_data;
        if ((map[bit >> 3] & (1u << (bit & 7))) == 0) {
                printf("ext2fs: block %u freed twice\n", bno);
                brelse(bp);
                return;
        }
        map[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
        bdwrite(bp);
        em->em_gd[g].bg_free_blocks_count++;
        sb->s_free_blocks_count++;
        ext2fs_sbdirty(em);
}

/* The group a new directory should go in (NetBSD ext2fs_dirpref). */
static uint32_t
dirpref(struct ext2mount *em)
{
        uint32_t avgifree = em->em_sb->s_free_inodes_count / em->em_ngroups;
        uint32_t best = 0, bestfree = 0;
        int found = 0;

        for (uint32_t g = 0; g < em->em_ngroups; g++) {
                const struct ext2fs_gd *gd = &em->em_gd[g];

                if (gd->bg_free_inodes_count >= avgifree &&
                    gd->bg_free_inodes_count > 0 &&
                    (!found || gd->bg_free_blocks_count > bestfree)) {
                        best = g;
                        bestfree = gd->bg_free_blocks_count;
                        found = 1;
                }
        }
        return best;
}

/*
 * Allocate an inode for a new file in directory pip (a directory if
 * isdir). *inop gets its number. ENOSPC if none is free.
 */
int
ext2fs_ialloc(struct ext2mount *em, struct ext2inode *pip, int isdir,
    uint32_t *inop)
{
        struct ext2fs_sb *sb = em->em_sb;
        uint32_t ipg = sb->s_inodes_per_group;
        uint32_t g0;

        if (sb->s_free_inodes_count == 0)
                return ENOSPC;
        g0 = isdir ? dirpref(em) : (pip->i_number - 1) / ipg;

        for (uint32_t k = 0; k < em->em_ngroups; k++) {
                uint32_t g = (g0 + k) % em->em_ngroups;
                struct ext2fs_gd *gd = &em->em_gd[g];
                struct buf *bp;
                int bit, error;

                if (gd->bg_free_inodes_count == 0)
                        continue;
                if ((error = ext2fs_bread(em, gd->bg_inode_bitmap, &bp)) != 0)
                        return error;
                bit = bitmap_find(bp->b_data, ipg, 0);
                if (bit < 0 || g * ipg + (uint32_t)bit + 1 < em->em_first_ino) {
                        brelse(bp);
                        continue;
                }
                ((uint8_t *)bp->b_data)[bit >> 3] |= (uint8_t)(1u << (bit & 7));
                bdwrite(bp);
                gd->bg_free_inodes_count--;
                sb->s_free_inodes_count--;
                if (isdir)
                        gd->bg_used_dirs_count++;
                ext2fs_sbdirty(em);
                *inop = g * ipg + (uint32_t)bit + 1;
                return 0;
        }
        return ENOSPC;
}

void
ext2fs_ifree(struct ext2mount *em, uint32_t ino, int isdir)
{
        struct ext2fs_sb *sb = em->em_sb;
        uint32_t g = (ino - 1) / sb->s_inodes_per_group;
        uint32_t bit = (ino - 1) % sb->s_inodes_per_group;
        struct buf *bp;
        uint8_t *map;

        if (ino < em->em_first_ino || ino > sb->s_inodes_count) {
                printf("ext2fs: freeing bad inode %u\n", ino);
                return;
        }
        if (ext2fs_bread(em, em->em_gd[g].bg_inode_bitmap, &bp) != 0)
                return;
        map = bp->b_data;
        if ((map[bit >> 3] & (1u << (bit & 7))) == 0) {
                printf("ext2fs: inode %u freed twice\n", ino);
                brelse(bp);
                return;
        }
        map[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
        bdwrite(bp);
        em->em_gd[g].bg_free_inodes_count++;
        sb->s_free_inodes_count++;
        if (isdir)
                em->em_gd[g].bg_used_dirs_count--;
        ext2fs_sbdirty(em);
}
