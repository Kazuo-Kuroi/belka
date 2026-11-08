/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ext2fs helpers: block I/O by file system block, type conversion, and
 * checks that the on-disk structures have the sizes the format gives.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/buf.h>
#include <sys/compiler.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/vnode.h>

#include "fs/ext2fs/ext2fs.h"

CTASSERT(offsetof(struct ext2fs_sb, s_magic) == 56);
CTASSERT(offsetof(struct ext2fs_sb, s_first_ino) == 84);
CTASSERT(offsetof(struct ext2fs_sb, s_feature_ro_compat) == 100);
CTASSERT(offsetof(struct ext2fs_sb, s_volume_name) == 120);
CTASSERT(sizeof(struct ext2fs_gd) == 32);
CTASSERT(sizeof(struct ext2fs_dinode) == EXT2_GOOD_OLD_INODE_SIZE);
CTASSERT(offsetof(struct ext2fs_dinode, e2di_blocks) == 40);
CTASSERT(offsetof(struct ext2fs_dinode, e2di_size_hi) == 108);
CTASSERT(offsetof(struct ext2fs_direct, e2d_name) == EXT2_DIRENT_HDR);

/* Read file system block bno. */
int
ext2fs_bread(struct ext2mount *em, uint32_t bno, struct buf **bpp)
{
        return bread(em->em_dev, (daddr_t)bno * em->em_bsects, em->em_bsize,
            bpp);
}

/* The buffer for block bno, busy, without reading it. */
struct buf *
ext2fs_getblk(struct ext2mount *em, uint32_t bno)
{
        return getblk(em->em_dev, (daddr_t)bno * em->em_bsects, em->em_bsize);
}

enum vtype
ext2fs_vtype(uint16_t mode)
{
        switch (mode & S_IFMT) {
        case S_IFREG:
                return VREG;
        case S_IFDIR:
                return VDIR;
        case S_IFLNK:
                return VLNK;
        case S_IFCHR:
                return VCHR;
        case S_IFBLK:
                return VBLK;
        case S_IFIFO:
                return VFIFO;
        case S_IFSOCK:
                return VSOCK;
        default:
                return VNON;
        }
}

int
ext2fs_ftype(enum vtype type)
{
        switch (type) {
        case VREG:
                return EXT2_FT_REG_FILE;
        case VDIR:
                return EXT2_FT_DIR;
        case VLNK:
                return EXT2_FT_SYMLINK;
        case VCHR:
                return EXT2_FT_CHRDEV;
        case VBLK:
                return EXT2_FT_BLKDEV;
        case VFIFO:
                return EXT2_FT_FIFO;
        case VSOCK:
                return EXT2_FT_SOCK;
        default:
                return EXT2_FT_UNKNOWN;
        }
}

/*
 * Set the size in core and on disk. Regular files past 2 GB keep the
 * high half in e2di_size_hi, which needs the LARGE_FILE feature; the
 * caller has checked.
 */
void
ext2fs_setsize(struct ext2inode *ip, uint64_t size)
{
        ip->i_size = size;
        ip->i_din.e2di_size = (uint32_t)size;
        if (ext2fs_vtype(ip->i_din.e2di_mode) == VREG)
                ip->i_din.e2di_size_hi = (uint32_t)(size >> 32);
}

uint32_t
ext2fs_now(void)
{
        struct timespec ts;

        gethrestime(&ts);
        return (uint32_t)ts.tv_sec;
}
