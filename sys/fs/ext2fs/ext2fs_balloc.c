/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ext2fs block maps: translating a file's logical blocks to disk blocks
 * through the twelve direct pointers and the single, double, and triple
 * indirect blocks, allocating on demand; and truncation, which frees
 * data blocks past the new end and indirect blocks left empty.
 *
 * Allocation (ext2fs_bmap with alloc) and truncation are called with
 * em_lock held, and for regular files with i_rwlock held for update.
 * The caller writes the inode back afterwards.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/systm.h>

#include "fs/ext2fs/ext2fs.h"

static int alloc_near(struct ext2inode *, uint32_t *);
static int trunc_indir(struct ext2inode *, uint32_t, int, uint64_t,
    uint64_t, int *);

/* Allocate a block after the inode's last one, or in its group. */
static int
alloc_near(struct ext2inode *ip, uint32_t *bnop)
{
        struct ext2mount *em = ip->i_mnt;
        uint32_t pref = ip->i_lastblk != 0 ? ip->i_lastblk + 1 :
            ext2fs_group_first_block(em,
            (ip->i_number - 1) / em->em_sb->s_inodes_per_group);
        int error = ext2fs_balloc_block(em, pref, bnop);

        if (error == 0) {
                ip->i_lastblk = *bnop;
                ip->i_din.e2di_nblock += em->em_bsects;
        }
        return error;
}

/*
 * Translate logical block lbn of ip. *pbnp gets the disk block, or 0
 * for a hole. With alloc, holes (and missing indirect blocks) are
 * filled, and *newp says whether the data block is new and unwritten.
 */
int
ext2fs_bmap(struct ext2inode *ip, uint64_t lbn, uint32_t *pbnp, int alloc,
    int *newp)
{
        struct ext2mount *em = ip->i_mnt;
        uint64_t per = em->em_nindir;
        uint32_t idx[EXT2_NIADDR];
        uint32_t *slot, bno;
        int level, error;

        if (newp != NULL)
                *newp = 0;
        if (lbn < EXT2_NDADDR) {
                slot = &ip->i_din.e2di_blocks[lbn];
                if (*slot == 0 && alloc) {
                        if ((error = alloc_near(ip, slot)) != 0)
                                return error;
                        if (newp != NULL)
                                *newp = 1;
                }
                *pbnp = *slot;
                return 0;
        }

        /* Which indirect tree, and the index at each of its levels. */
        lbn -= EXT2_NDADDR;
        if (lbn < per) {
                level = 1;
        } else if ((lbn -= per) < per * per) {
                level = 2;
        } else if ((lbn -= per * per) < per * per * per) {
                level = 3;
        } else {
                return EFBIG;
        }
        for (int l = level - 1; l >= 0; l--) {
                idx[l] = (uint32_t)(lbn % per);
                lbn /= per;
        }

        slot = &ip->i_din.e2di_blocks[EXT2_NDADDR + level - 1];
        if (*slot == 0) {
                struct buf *bp;

                if (!alloc) {
                        *pbnp = 0;
                        return 0;
                }
                if ((error = alloc_near(ip, slot)) != 0)
                        return error;
                bp = ext2fs_getblk(em, *slot);
                bclrbuf(bp);
                bdwrite(bp);
        }
        bno = *slot;

        for (int l = 0; l < level; l++) {
                struct buf *bp;
                uint32_t *ent;
                int last = l == level - 1;

                if ((error = ext2fs_bread(em, bno, &bp)) != 0)
                        return error;
                ent = (uint32_t *)bp->b_data + idx[l];
                if (*ent == 0) {
                        if (!alloc) {
                                brelse(bp);
                                *pbnp = 0;
                                return 0;
                        }
                        if ((error = alloc_near(ip, ent)) != 0) {
                                brelse(bp);
                                return error;
                        }
                        if (!last) {
                                struct buf *nbp = ext2fs_getblk(em, *ent);

                                bclrbuf(nbp);
                                bdwrite(nbp);
                        } else if (newp != NULL) {
                                *newp = 1;
                        }
                        bno = *ent;
                        bdwrite(bp);
                } else {
                        bno = *ent;
                        brelse(bp);
                }
        }
        *pbnp = bno;
        return 0;
}

/*
 * Free what lies at or past logical block keep in the level-high tree
 * rooted at bno, whose first logical block is base. *emptyp says whether
 * nothing is left in it, so the caller can free bno itself.
 */
static int
trunc_indir(struct ext2inode *ip, uint32_t bno, int level, uint64_t base,
    uint64_t keep, int *emptyp)
{
        struct ext2mount *em = ip->i_mnt;
        uint64_t span = 1;
        struct buf *bp;
        uint32_t *ent;
        int error, modified = 0;

        for (int l = 1; l < level; l++)
                span *= em->em_nindir;
        if ((error = ext2fs_bread(em, bno, &bp)) != 0)
                return error;
        ent = bp->b_data;
        *emptyp = 1;
        for (uint32_t i = 0; i < em->em_nindir; i++) {
                uint64_t cbase = base + i * span;
                int cempty;

                if (ent[i] == 0)
                        continue;
                if (cbase + span <= keep) {
                        *emptyp = 0;            /* wholly kept */
                        continue;
                }
                if (level == 1) {
                        cempty = 1;
                } else if ((error = trunc_indir(ip, ent[i], level - 1, cbase,
                    keep, &cempty)) != 0) {
                        brelse(bp);
                        return error;
                }
                if (cempty) {
                        ext2fs_bfree(em, ent[i]);
                        ip->i_din.e2di_nblock -= em->em_bsects;
                        ent[i] = 0;
                        modified = 1;
                } else {
                        *emptyp = 0;
                }
        }
        if (modified)
                bdwrite(bp);
        else
                brelse(bp);
        return 0;
}

/*
 * Shorten (or lengthen, with a hole) ip to length bytes, freeing blocks
 * wholly past the end and zeroing the rest of a partial last block.
 */
int
ext2fs_truncate(struct ext2inode *ip, uint64_t length)
{
        struct ext2mount *em = ip->i_mnt;
        uint64_t per = em->em_nindir;
        uint64_t keep = (length + em->em_bsize - 1) / em->em_bsize;
        uint64_t base = EXT2_NDADDR, span = per;
        int error;

        if (length < ip->i_size && length % em->em_bsize != 0) {
                uint32_t pbn;

                if ((error = ext2fs_bmap(ip, length / em->em_bsize, &pbn, 0,
                    NULL)) != 0)
                        return error;
                if (pbn != 0) {
                        struct buf *bp;
                        size_t off = (size_t)(length % em->em_bsize);

                        if ((error = ext2fs_bread(em, pbn, &bp)) != 0)
                                return error;
                        memset((char *)bp->b_data + off, 0,
                            em->em_bsize - off);
                        bdwrite(bp);
                }
        }

        for (uint64_t i = keep; i < EXT2_NDADDR; i++) {
                if (ip->i_din.e2di_blocks[i] != 0) {
                        ext2fs_bfree(em, ip->i_din.e2di_blocks[i]);
                        ip->i_din.e2di_nblock -= em->em_bsects;
                        ip->i_din.e2di_blocks[i] = 0;
                }
        }
        for (int level = 1; level <= EXT2_NIADDR; level++) {
                uint32_t *slot = &ip->i_din.e2di_blocks[EXT2_NDADDR +
                    level - 1];
                int empty;

                if (*slot != 0 && keep < base + span) {
                        if ((error = trunc_indir(ip, *slot, level, base, keep,
                            &empty)) != 0)
                                return error;
                        if (empty) {
                                ext2fs_bfree(em, *slot);
                                ip->i_din.e2di_nblock -= em->em_bsects;
                                *slot = 0;
                        }
                }
                base += span;
                span *= per;
        }
        if (ip->i_lastblk != 0 && keep == 0)
                ip->i_lastblk = 0;
        ext2fs_setsize(ip, length);
        return 0;
}
