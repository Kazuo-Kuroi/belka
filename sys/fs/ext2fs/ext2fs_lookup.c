/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ext2fs directories.
 *
 * A directory is a file of whole blocks, each a chain of variable-length
 * entries whose record lengths add up to the block. An entry with inode
 * 0 is free. A new entry goes into a free entry or the slack at the end
 * of a used one if either is large enough, else into a new block at the
 * end. A removed entry is merged into the one before it in its block,
 * or freed in place if it is the first.
 *
 * A directory changed here loses its hash index flag: the index blocks
 * read as empty entries, so the directory stays a valid linear one.
 *
 * Offsets given to and returned from VOP_READDIR are byte offsets into
 * the directory, so they stay meaningful across changes.
 *
 * Everything here runs with em_lock held.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include "fs/ext2fs/ext2fs.h"

static int dir_entry_ok(const struct ext2mount *, const struct ext2fs_direct *,
    uint32_t);
static int dir_block(struct ext2inode *, uint64_t, struct buf **);
static int dir_find(struct ext2inode *, const char *, struct buf **,
    uint32_t *, uint32_t *, int *);
static void dir_changed(struct ext2inode *);

/* Nonzero if the entry at offset off of a block is well formed. */
static int
dir_entry_ok(const struct ext2mount *em, const struct ext2fs_direct *d,
    uint32_t off)
{
        return d->e2d_reclen >= EXT2_DIRENT_HDR &&
            (d->e2d_reclen & 3) == 0 &&
            off + d->e2d_reclen <= em->em_bsize &&
            (uint32_t)d->e2d_namlen + EXT2_DIRENT_HDR <= d->e2d_reclen;
}

/* Read directory block lbn; a hole is a corrupt directory. */
static int
dir_block(struct ext2inode *dp, uint64_t lbn, struct buf **bpp)
{
        uint32_t pbn;
        int error;

        if ((error = ext2fs_bmap(dp, lbn, &pbn, 0, NULL)) != 0)
                return error;
        if (pbn == 0) {
                printf("ext2fs: directory %u has a hole\n", dp->i_number);
                return EIO;
        }
        return ext2fs_bread(dp->i_mnt, pbn, bpp);
}

/*
 * Find name in dp. On success the block's buffer is returned busy, with
 * the entry's offset, the offset of the entry before it in the block
 * (or UINT32_MAX), and the logical block number.
 */
static int
dir_find(struct ext2inode *dp, const char *name, struct buf **bpp,
    uint32_t *offp, uint32_t *prevp, int *lbnp)
{
        struct ext2mount *em = dp->i_mnt;
        size_t len = strlen(name);
        uint64_t nblocks = dp->i_size / em->em_bsize;

        for (uint64_t lbn = 0; lbn < nblocks; lbn++) {
                struct buf *bp;
                uint32_t off = 0, prev = UINT32_MAX;
                int error;

                if ((error = dir_block(dp, lbn, &bp)) != 0)
                        return error;
                while (off < em->em_bsize) {
                        const struct ext2fs_direct *d =
                            (const void *)((const char *)bp->b_data + off);

                        if (!dir_entry_ok(em, d, off)) {
                                printf("ext2fs: directory %u is corrupt at "
                                    "block %ju offset %u\n", dp->i_number,
                                    (uintmax_t)lbn, off);
                                brelse(bp);
                                return EIO;
                        }
                        if (d->e2d_ino != 0 && d->e2d_namlen == len &&
                            memcmp(d->e2d_name, name, len) == 0) {
                                *bpp = bp;
                                *offp = off;
                                *prevp = prev;
                                *lbnp = (int)lbn;
                                return 0;
                        }
                        prev = off;
                        off += d->e2d_reclen;
                }
                brelse(bp);
        }
        return ENOENT;
}

int
ext2fs_dirlook(struct ext2inode *dp, const char *name, uint32_t *inop)
{
        struct buf *bp;
        uint32_t off, prev;
        int lbn, error;

        if ((error = dir_find(dp, name, &bp, &off, &prev, &lbn)) != 0)
                return error;
        *inop = ((const struct ext2fs_direct *)(const void *)
            ((const char *)bp->b_data + off))->e2d_ino;
        brelse(bp);
        return 0;
}

static void
dir_changed(struct ext2inode *dp)
{
        dp->i_din.e2di_flags &= ~(uint32_t)EXT2_INDEX_FL;
        dp->i_din.e2di_mtime = dp->i_din.e2di_ctime = ext2fs_now();
}

/* Add name -> ino of type ftype; the caller knows it is not there. */
int
ext2fs_direnter(struct ext2inode *dp, const char *name, uint32_t ino,
    int ftype)
{
        struct ext2mount *em = dp->i_mnt;
        size_t len = strlen(name);
        uint32_t need = EXT2_DIR_REC_LEN(len);
        uint64_t nblocks = dp->i_size / em->em_bsize;
        struct ext2fs_direct *d;
        struct buf *bp;
        uint32_t pbn;
        int error, isnew;

        if (len == 0 || len > EXT2_MAXNAMLEN)
                return len == 0 ? ENOENT : ENAMETOOLONG;
        if (!em->em_filetype)
                ftype = 0;

        for (uint64_t lbn = 0; lbn < nblocks; lbn++) {
                uint32_t off = 0;

                if ((error = dir_block(dp, lbn, &bp)) != 0)
                        return error;
                while (off < em->em_bsize) {
                        uint32_t used;

                        d = (void *)((char *)bp->b_data + off);
                        if (!dir_entry_ok(em, d, off)) {
                                brelse(bp);
                                return EIO;
                        }
                        used = d->e2d_ino != 0 ?
                            EXT2_DIR_REC_LEN(d->e2d_namlen) : 0;
                        if (d->e2d_reclen - used >= need) {
                                if (used != 0) {
                                        /* Split the slack off. */
                                        uint32_t rest = d->e2d_reclen - used;

                                        d->e2d_reclen = (uint16_t)used;
                                        d = (void *)((char *)d + used);
                                        d->e2d_reclen = (uint16_t)rest;
                                }
                                d->e2d_ino = ino;
                                d->e2d_namlen = (uint8_t)len;
                                d->e2d_type = (uint8_t)ftype;
                                memcpy(d->e2d_name, name, len);
                                bdwrite(bp);
                                dir_changed(dp);
                                return ext2fs_iupdate(dp);
                        }
                        off += d->e2d_reclen;
                }
                brelse(bp);
        }

        /* No room: a new block at the end. */
        if ((error = ext2fs_bmap(dp, nblocks, &pbn, 1, &isnew)) != 0)
                return error;
        bp = ext2fs_getblk(em, pbn);
        bclrbuf(bp);
        d = bp->b_data;
        d->e2d_ino = ino;
        d->e2d_reclen = (uint16_t)em->em_bsize;
        d->e2d_namlen = (uint8_t)len;
        d->e2d_type = (uint8_t)ftype;
        memcpy(d->e2d_name, name, len);
        bdwrite(bp);
        ext2fs_setsize(dp, (nblocks + 1) * em->em_bsize);
        dir_changed(dp);
        return ext2fs_iupdate(dp);
}

int
ext2fs_dirremove(struct ext2inode *dp, const char *name)
{
        struct ext2fs_direct *d;
        struct buf *bp;
        uint32_t off, prev;
        int lbn, error;

        if ((error = dir_find(dp, name, &bp, &off, &prev, &lbn)) != 0)
                return error;
        d = (void *)((char *)bp->b_data + off);
        if (prev != UINT32_MAX) {
                struct ext2fs_direct *p = (void *)((char *)bp->b_data + prev);

                p->e2d_reclen = (uint16_t)(p->e2d_reclen + d->e2d_reclen);
        } else {
                d->e2d_ino = 0;
        }
        bdwrite(bp);
        dir_changed(dp);
        return ext2fs_iupdate(dp);
}

/* 1 if dp holds nothing but "." and "..", 0 if not, or -errno. */
int
ext2fs_dirempty(struct ext2inode *dp)
{
        struct ext2mount *em = dp->i_mnt;
        uint64_t nblocks = dp->i_size / em->em_bsize;

        for (uint64_t lbn = 0; lbn < nblocks; lbn++) {
                struct buf *bp;
                uint32_t off = 0;
                int error;

                if ((error = dir_block(dp, lbn, &bp)) != 0)
                        return -error;
                while (off < em->em_bsize) {
                        const struct ext2fs_direct *d =
                            (const void *)((const char *)bp->b_data + off);

                        if (!dir_entry_ok(em, d, off)) {
                                brelse(bp);
                                return -EIO;
                        }
                        if (d->e2d_ino != 0 &&
                            !(d->e2d_namlen == 1 && d->e2d_name[0] == '.') &&
                            !(d->e2d_namlen == 2 && d->e2d_name[0] == '.' &&
                            d->e2d_name[1] == '.')) {
                                brelse(bp);
                                return 0;
                        }
                        off += d->e2d_reclen;
                }
                brelse(bp);
        }
        return 1;
}

/* Give the new directory dp its first block, with "." and "..". */
int
ext2fs_dirinit(struct ext2inode *dp, uint32_t parent)
{
        struct ext2mount *em = dp->i_mnt;
        struct ext2fs_direct *d;
        struct buf *bp;
        uint32_t pbn;
        int error, isnew;
        uint8_t ft = em->em_filetype ? EXT2_FT_DIR : 0;

        if ((error = ext2fs_bmap(dp, 0, &pbn, 1, &isnew)) != 0)
                return error;
        bp = ext2fs_getblk(em, pbn);
        bclrbuf(bp);
        d = bp->b_data;
        d->e2d_ino = dp->i_number;
        d->e2d_reclen = 12;
        d->e2d_namlen = 1;
        d->e2d_type = ft;
        d->e2d_name[0] = '.';
        d = (void *)((char *)bp->b_data + 12);
        d->e2d_ino = parent;
        d->e2d_reclen = (uint16_t)(em->em_bsize - 12);
        d->e2d_namlen = 2;
        d->e2d_type = ft;
        d->e2d_name[0] = d->e2d_name[1] = '.';
        bdwrite(bp);
        ext2fs_setsize(dp, em->em_bsize);
        return 0;
}

/* Point dp's ".." at parent, after a rename to another directory. */
int
ext2fs_setdotdot(struct ext2inode *dp, uint32_t parent)
{
        struct ext2fs_direct *d;
        struct buf *bp;
        uint32_t off, prev;
        int lbn, error;

        if ((error = dir_find(dp, "..", &bp, &off, &prev, &lbn)) != 0)
                return error;
        d = (void *)((char *)bp->b_data + off);
        d->e2d_ino = parent;
        bdwrite(bp);
        return 0;
}

/* Copy entries from uio_offset into uio as struct dirent records. */
int
ext2fs_readdir_uio(struct ext2inode *dp, struct uio *uio, int *eofp)
{
        struct ext2mount *em = dp->i_mnt;
        char rec[DIRENT_RECLEN(EXT2_MAXNAMLEN)];
        uint64_t pos = (uint64_t)uio->uio_offset;
        int error = 0;

        *eofp = 0;
        while (pos < dp->i_size) {
                uint64_t lbn = pos / em->em_bsize;
                uint32_t boff = (uint32_t)(pos % em->em_bsize), off = 0;
                struct buf *bp;

                if ((error = dir_block(dp, lbn, &bp)) != 0)
                        return error;
                while (off < em->em_bsize) {
                        const struct ext2fs_direct *d =
                            (const void *)((const char *)bp->b_data + off);
                        struct dirent *de = (struct dirent *)(void *)rec;
                        size_t reclen;

                        if (!dir_entry_ok(em, d, off)) {
                                brelse(bp);
                                return EIO;
                        }
                        if (off < boff || d->e2d_ino == 0) {
                                off += d->e2d_reclen;
                                continue;
                        }
                        reclen = DIRENT_RECLEN(d->e2d_namlen);
                        if ((ssize_t)reclen > uio->uio_resid) {
                                brelse(bp);
                                uio->uio_offset = (off_t)(lbn * em->em_bsize +
                                    off);
                                return 0;
                        }
                        memset(rec, 0, reclen);
                        de->d_ino = d->e2d_ino;
                        de->d_off = (off_t)(lbn * em->em_bsize + off +
                            d->e2d_reclen);
                        de->d_reclen = (uint16_t)reclen;
                        memcpy(de->d_name, d->e2d_name, d->e2d_namlen);
                        de->d_name[d->e2d_namlen] = '\0';
                        if ((error = uiomove(rec, reclen, UIO_READ, uio)) !=
                            0) {
                                brelse(bp);
                                return error;
                        }
                        off += d->e2d_reclen;
                }
                brelse(bp);
                pos = (lbn + 1) * em->em_bsize;
                uio->uio_offset = (off_t)pos;
        }
        *eofp = 1;
        uio->uio_offset = (off_t)pos;
        return 0;
}
