/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ext2fs mount operations and the in-core inode table.
 *
 * An in-core inode exists while its vnode keeps its identity. When the
 * last reference goes, an inode with links stays cached, and the global
 * vnode cache reclaims it later (ext2fs_reclaim); an inode with no links
 * left has its blocks and itself freed at once.
 *
 * The superblock is read and written through its own 1 KB buffer at
 * sector 2, never through the file system block that contains it, so
 * the two can never alias in the buffer cache.
 *
 * A mount read-write clears EXT2_VALID_FS on disk at once; unmount sets
 * it again after writing everything back, so a host fsck sees a clean
 * file system after an orderly unmount.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/buf.h>
#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/stat.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include "fs/ext2fs/ext2fs.h"

#define EXT2_SB_SECTOR  (EXT2_SBOFF / 512)

static int ext2fs_mount(struct vfs *, const void *, struct cred *);
static int ext2fs_unmount(struct vfs *, struct cred *);
static int ext2fs_root(struct vfs *, struct vnode **);
static int ext2fs_statvfs(struct vfs *, struct statvfs *);
static int ext2fs_sync(struct vfs *, struct cred *);
static unsigned int ihash(uint32_t);
static void ihash_remove(struct ext2mount *, struct ext2inode *);
static void ifree_incore(struct ext2mount *, struct ext2inode *);

const struct vfsops ext2fs_vfsops = {
        ext2fs_mount, ext2fs_unmount, ext2fs_root, ext2fs_statvfs, ext2fs_sync
};

static unsigned int
ihash(uint32_t ino)
{
        return ino % 64;
}

static void
ihash_remove(struct ext2mount *em, struct ext2inode *ip)
{
        for (struct ext2inode **l = &em->em_ihash[ihash(ip->i_number)];
            *l != NULL; l = &(*l)->i_hnext) {
                if (*l == ip) {
                        *l = ip->i_hnext;
                        break;
                }
        }
}

static void
ifree_incore(struct ext2mount *em, struct ext2inode *ip)
{
        ihash_remove(em, ip);
        em->em_ninodes--;
        kmem_free(ip, sizeof(*ip));
}

/* The disk block holding inode ino, and its byte offset there. */
static void
inode_location(struct ext2mount *em, uint32_t ino, uint32_t *bnop,
    uint32_t *offp)
{
        uint32_t g = (ino - 1) / em->em_sb->s_inodes_per_group;
        uint32_t idx = (ino - 1) % em->em_sb->s_inodes_per_group;

        *bnop = em->em_gd[g].bg_inode_table + idx / em->em_ipb;
        *offp = (idx % em->em_ipb) * em->em_inodesize;
}

static struct ext2inode *
inode_incore(struct ext2mount *em, uint32_t ino)
{
        struct ext2inode *ip = kmem_zalloc(sizeof(*ip), KM_SLEEP);

        ip->i_mnt = em;
        ip->i_number = ino;
        mrinit(&ip->i_rwlock, "ext2inode");
        ip->i_hnext = em->em_ihash[ihash(ino)];
        em->em_ihash[ihash(ino)] = ip;
        em->em_ninodes++;
        return ip;
}

/* Return inode ino, held, reading it if not in core. em_lock is held. */
int
ext2fs_vget(struct ext2mount *em, uint32_t ino, struct ext2inode **ipp)
{
        struct ext2inode *ip;
        uint32_t bno, off;
        struct buf *bp;
        int error;

        if (ino < 1 || ino > em->em_sb->s_inodes_count)
                return EINVAL;
        for (ip = em->em_ihash[ihash(ino)]; ip != NULL; ip = ip->i_hnext) {
                if (ip->i_number == ino) {
                        VN_HOLD(ITOV(ip));
                        *ipp = ip;
                        return 0;
                }
        }
        inode_location(em, ino, &bno, &off);
        if ((error = ext2fs_bread(em, bno, &bp)) != 0)
                return error;
        ip = inode_incore(em, ino);
        memcpy(&ip->i_din, (char *)bp->b_data + off, sizeof(ip->i_din));
        brelse(bp);
        ip->i_size = ip->i_din.e2di_size;
        if (ext2fs_vtype(ip->i_din.e2di_mode) == VREG)
                ip->i_size |= (uint64_t)ip->i_din.e2di_size_hi << 32;
        /* May reclaim other inodes, but never one of ours: we hold em_lock. */
        ip->i_vnode = vn_alloc(em->em_vfs, ext2fs_vtype(ip->i_din.e2di_mode),
            &ext2fs_vnodeops, ip);
        if (ino == EXT2_ROOTINO)
                ITOV(ip)->v_flag |= VROOT;
        *ipp = ip;
        return 0;
}

/*
 * Allocate and initialise a new inode of mode mode for a file in pip,
 * owned by va_uid and va_gid if va gives them (else by the superuser).
 * Returned held, with one link for a file and two for a directory.
 * em_lock is held.
 */
int
ext2fs_newinode(struct ext2mount *em, struct ext2inode *pip, uint16_t mode,
    const struct vattr *va, struct ext2inode **ipp)
{
        int isdir = (mode & S_IFMT) == S_IFDIR;
        struct ext2inode *ip;
        uint32_t ino, now = ext2fs_now();
        uint32_t uid, gid;
        int error;

        if ((error = ext2fs_ialloc(em, pip, isdir, &ino)) != 0)
                return error;
        ip = inode_incore(em, ino);
        ip->i_din.e2di_mode = mode;
        uid = (va->va_mask & AT_UID) != 0 ? va->va_uid : 0;
        gid = (va->va_mask & AT_GID) != 0 ? va->va_gid : 0;
        ip->i_din.e2di_uid = (uint16_t)uid;
        ip->i_din.e2di_uid_high = (uint16_t)(uid >> 16);
        ip->i_din.e2di_gid = (uint16_t)gid;
        ip->i_din.e2di_gid_high = (uint16_t)(gid >> 16);
        ip->i_din.e2di_atime = ip->i_din.e2di_ctime =
            ip->i_din.e2di_mtime = now;
        ip->i_din.e2di_nlink = isdir ? 2 : 1;
        ip->i_din.e2di_gen = now ^ ino;
        ip->i_vnode = vn_alloc(em->em_vfs, ext2fs_vtype(mode),
            &ext2fs_vnodeops, ip);
        if ((error = ext2fs_iupdate(ip)) != 0) {
                ip->i_din.e2di_nlink = 0;
                ext2fs_rele_locked(ip);
                return error;
        }
        *ipp = ip;
        return 0;
}

/* Write the in-core inode into its disk block (delayed). */
int
ext2fs_iupdate(struct ext2inode *ip)
{
        struct ext2mount *em = ip->i_mnt;
        uint32_t bno, off;
        struct buf *bp;
        int error;

        if (em->em_ronly)
                return 0;
        inode_location(em, ip->i_number, &bno, &off);
        if ((error = ext2fs_bread(em, bno, &bp)) != 0)
                return error;
        memcpy((char *)bp->b_data + off, &ip->i_din, sizeof(ip->i_din));
        bdwrite(bp);
        return 0;
}

/*
 * The last reference went; em_lock is held. An inode with links stays
 * cached. A removed one has its blocks and itself freed, and leaves
 * core. Returns VN_INACTIVE_CACHE or VN_INACTIVE_NOCACHE.
 */
int
ext2fs_inactive_locked(struct ext2inode *ip)
{
        struct ext2mount *em = ip->i_mnt;
        struct vnode *vp = ITOV(ip);

        if (vp->v_count != 0)
                return VN_INACTIVE_CACHE;       /* found again meanwhile */
        if (ip->i_din.e2di_nlink != 0)
                return VN_INACTIVE_CACHE;
        if (!em->em_ronly && ip->i_din.e2di_mode != 0) {
                int isdir = ext2fs_vtype(ip->i_din.e2di_mode) == VDIR;
                int fastlink = ext2fs_vtype(ip->i_din.e2di_mode) == VLNK &&
                    ip->i_din.e2di_nblock == 0;

                if (ip->i_din.e2di_facl != 0) {
                        /* Extended attributes are not handled; keep it. */
                        printf("ext2fs: inode %u has an attribute block; "
                            "not freed\n", ip->i_number);
                } else {
                        if (fastlink) {
                                memset(ip->i_din.e2di_blocks, 0,
                                    sizeof(ip->i_din.e2di_blocks));
                                ext2fs_setsize(ip, 0);
                        } else {
                                (void)ext2fs_truncate(ip, 0);
                        }
                        ip->i_din.e2di_dtime = ext2fs_now();
                        (void)ext2fs_iupdate(ip);
                        ext2fs_ifree(em, ip->i_number, isdir);
                }
        }
        ifree_incore(em, ip);
        vp->v_data = NULL;
        return VN_INACTIVE_NOCACHE;
}

static int
inactive_locked_vp(struct vnode *vp)
{
        return ext2fs_inactive_locked(VTOI(vp));
}

/* VN_RELE for callers that hold em_lock. */
void
ext2fs_rele_locked(struct ext2inode *ip)
{
        vn_rele_inactive(ITOV(ip), inactive_locked_vp);
}

/*
 * The vnode cache wants vp for another file: drop the inode from core.
 * Refused if vp was found again meanwhile, or, with VRC_NOWAIT, if
 * another thread holds em_lock.
 *
 * Our own vget and newinode allocate vnodes with em_lock held, and must
 * be able to reuse this mount's cached vnodes, or a busy file system
 * would only ever grow. So when the caller already owns em_lock, the
 * reclaim goes ahead under it: an unreferenced inode is not in use by
 * any operation, and nothing else can reach it without em_lock.
 */
int
ext2fs_reclaim(struct vnode *vp, int flags)
{
        struct ext2inode *ip = VTOI(vp);
        struct ext2mount *em = ip->i_mnt;
        int mine = mutex_owned(&em->em_lock);

        if (!mine) {
                if ((flags & VRC_NOWAIT) != 0) {
                        if (!mutex_trylock(&em->em_lock))
                                return EBUSY;
                } else {
                        mutex_lock(&em->em_lock);
                }
        }
        if (vp->v_count != 0) {
                if (!mine)
                        mutex_unlock(&em->em_lock);
                return EBUSY;
        }
        ifree_incore(em, ip);
        vp->v_data = NULL;
        if (!mine)
                mutex_unlock(&em->em_lock);
        return 0;
}

void
ext2fs_sbdirty(struct ext2mount *em)
{
        em->em_dirty = 1;
}

/* Write the group descriptors and the superblock. em_lock is held. */
int
ext2fs_sbupdate(struct ext2mount *em)
{
        size_t gdbytes = (size_t)em->em_ngroups * sizeof(struct ext2fs_gd);
        uint32_t gdstart = em->em_sb->s_first_data_block + 1;
        struct buf *bp;
        int error;

        if (em->em_ronly)
                return 0;
        for (uint32_t i = 0; i < em->em_gdblocks; i++) {
                size_t off = (size_t)i * em->em_bsize;
                size_t n = gdbytes - off < em->em_bsize ? gdbytes - off :
                    em->em_bsize;

                if ((error = ext2fs_bread(em, gdstart + i, &bp)) != 0)
                        return error;
                memcpy(bp->b_data, (char *)em->em_gd + off, n);
                bdwrite(bp);
        }
        em->em_sb->s_wtime = ext2fs_now();
        if ((error = bread(em->em_dev, EXT2_SB_SECTOR, EXT2_SBSIZE, &bp)) != 0)
                return error;
        memcpy(bp->b_data, em->em_sbraw, EXT2_SBSIZE);
        bdwrite(bp);
        em->em_dirty = 0;
        return 0;
}

static int
ext2fs_mount(struct vfs *vfsp, const void *data, struct cred *cr __unused)
{
        const struct ext2fs_args *args = data;
        struct ext2mount *em;
        struct ext2fs_sb *sb;
        struct ext2inode *root;
        struct blkdev *dev;
        struct buf *bp;
        uint32_t gdstart;
        int error;

        if (args == NULL || args->ea_dev == NULL)
                return EINVAL;
        if ((dev = blkdev_lookup(args->ea_dev)) == NULL)
                return ENXIO;
        /*
         * A disk holds one mounted file system: a second would keep its
         * own copy of the superblock, bitmaps and inodes. vfs_mutex is
         * held (vfs_mount_named), or nothing else is mounted yet.
         */
        for (struct vfs *v = rootvfs; v != NULL; v = v->vfs_next)
                if (v->vfs_op == &ext2fs_vfsops &&
                    ((struct ext2mount *)v->vfs_data)->em_dev == dev)
                        return EBUSY;
        if ((error = bread(dev, EXT2_SB_SECTOR, EXT2_SBSIZE, &bp)) != 0)
                return error;

        em = kmem_zalloc(sizeof(*em), KM_SLEEP);
        memcpy(em->em_sbraw, bp->b_data, EXT2_SBSIZE);
        brelse(bp);
        em->em_sb = sb = (struct ext2fs_sb *)(void *)em->em_sbraw;
        em->em_dev = dev;
        em->em_vfs = vfsp;
        em->em_ronly = args->ea_rdonly || dev->bd_readonly;
        mutex_init(&em->em_lock, "ext2fs");

        error = EINVAL;
        if (sb->s_magic != EXT2_MAGIC) {
                printf("ext2fs: %s: not an ext2 file system\n", dev->bd_name);
                goto fail;
        }
        if (sb->s_log_block_size > 2) {
                printf("ext2fs: %s: block size %u is not supported\n",
                    dev->bd_name, 1024u << sb->s_log_block_size);
                goto fail;
        }
        if (sb->s_rev_level >= 1 &&
            (sb->s_feature_incompat & ~(uint32_t)EXT2F_INCOMPAT_SUPP) != 0) {
                printf("ext2fs: %s: unsupported features %#x\n", dev->bd_name,
                    sb->s_feature_incompat & ~(uint32_t)EXT2F_INCOMPAT_SUPP);
                goto fail;
        }
        if (sb->s_rev_level >= 1 && !em->em_ronly &&
            (sb->s_feature_ro_compat & ~(uint32_t)EXT2F_ROCOMPAT_SUPP) != 0) {
                printf("ext2fs: %s: read-only features %#x; mounting "
                    "read-only\n", dev->bd_name, sb->s_feature_ro_compat &
                    ~(uint32_t)EXT2F_ROCOMPAT_SUPP);
                em->em_ronly = 1;
        }
        if (!em->em_ronly && (sb->s_state & EXT2_VALID_FS) == 0) {
                printf("ext2fs: %s: not cleanly unmounted; mounting "
                    "read-only\n", dev->bd_name);
                em->em_ronly = 1;
        }

        em->em_bsize = 1024u << sb->s_log_block_size;
        em->em_bsects = em->em_bsize / 512;
        em->em_nindir = em->em_bsize / 4;
        em->em_inodesize = sb->s_rev_level >= 1 ? sb->s_inode_size :
            EXT2_GOOD_OLD_INODE_SIZE;
        em->em_first_ino = sb->s_rev_level >= 1 ? sb->s_first_ino :
            EXT2_GOOD_OLD_FIRST_INO;
        if (em->em_inodesize < EXT2_GOOD_OLD_INODE_SIZE ||
            em->em_inodesize > em->em_bsize || sb->s_blocks_per_group == 0 ||
            sb->s_inodes_per_group == 0)
                goto fail;
        em->em_ipb = em->em_bsize / em->em_inodesize;
        em->em_filetype = sb->s_rev_level >= 1 &&
            (sb->s_feature_incompat & EXT2F_INCOMPAT_FILETYPE) != 0;
        em->em_largefile = sb->s_rev_level >= 1 &&
            (sb->s_feature_ro_compat & EXT2F_ROCOMPAT_LARGEFILE) != 0;
        em->em_ngroups = (sb->s_blocks_count - sb->s_first_data_block +
            sb->s_blocks_per_group - 1) / sb->s_blocks_per_group;
        em->em_gdblocks = (uint32_t)(((size_t)em->em_ngroups *
            sizeof(struct ext2fs_gd) + em->em_bsize - 1) / em->em_bsize);

        em->em_gd = kmem_zalloc((size_t)em->em_gdblocks * em->em_bsize,
            KM_SLEEP);
        gdstart = sb->s_first_data_block + 1;
        for (uint32_t i = 0; i < em->em_gdblocks; i++) {
                if ((error = ext2fs_bread(em, gdstart + i, &bp)) != 0)
                        goto fail_gd;
                memcpy((char *)em->em_gd + (size_t)i * em->em_bsize,
                    bp->b_data, em->em_bsize);
                brelse(bp);
        }

        if (!em->em_ronly) {
                sb->s_state &= (uint16_t)~EXT2_VALID_FS;
                sb->s_mnt_count++;
                sb->s_mtime = ext2fs_now();
                mutex_lock(&em->em_lock);
                error = ext2fs_sbupdate(em);
                mutex_unlock(&em->em_lock);
                if (error != 0 || (error = bflush(dev)) != 0)
                        goto fail_gd;
        }

        mutex_lock(&em->em_lock);
        error = ext2fs_vget(em, EXT2_ROOTINO, &root);
        mutex_unlock(&em->em_lock);
        if (error != 0)
                goto fail_gd;
        if (ITOV(root)->v_type != VDIR) {
                printf("ext2fs: %s: root inode is not a directory\n",
                    dev->bd_name);
                error = EINVAL;
                goto fail_gd;
        }
        vfsp->vfs_data = em;
        vfsp->vfs_bsize = em->em_bsize;
        snprintf(vfsp->vfs_special, sizeof(vfsp->vfs_special), "/dev/%s",
            dev->bd_name);       /* its devfs node */
        if (em->em_ronly)
                vfsp->vfs_flag |= VFS_RDONLY;
        printf("ext2fs: %s: %u blocks of %u bytes, %u groups, %u inodes of "
            "%u bytes%s\n", dev->bd_name, sb->s_blocks_count, em->em_bsize,
            em->em_ngroups, sb->s_inodes_count, em->em_inodesize,
            em->em_ronly ? ", read-only" : "");
        return 0;

fail_gd:
        kmem_free(em->em_gd, (size_t)em->em_gdblocks * em->em_bsize);
fail:
        (void)bflush(dev);
        binval(dev);
        kmem_free(em, sizeof(*em));
        return error;
}

static int
ext2fs_unmount(struct vfs *vfsp, struct cred *cr __unused)
{
        struct ext2mount *em = vfsp->vfs_data;
        struct ext2inode *root = NULL;
        int error = 0;

        /*
         * The generic layer has written back our pages and reclaimed our
         * cached vnodes; anything left in core but the root is in use.
         */
        mutex_lock(&em->em_lock);
        for (int h = 0; h < 64; h++) {
                for (struct ext2inode *ip = em->em_ihash[h]; ip != NULL;
                    ip = ip->i_hnext) {
                        if (ip->i_number != EXT2_ROOTINO ||
                            ITOV(ip)->v_count != 1) {
                                mutex_unlock(&em->em_lock);
                                return EBUSY;
                        }
                        root = ip;
                }
        }
        if (root != NULL) {
                struct vnode *vp = ITOV(root);

                ifree_incore(em, root);
                vp->v_data = NULL;
                vn_gone(vp);
        }
        if (em->em_ninodes != 0)
                panic("ext2fs_unmount: %u inodes left in core",
                    em->em_ninodes);
        if (!em->em_ronly) {
                em->em_sb->s_state |= EXT2_VALID_FS;
                error = ext2fs_sbupdate(em);
        }
        mutex_unlock(&em->em_lock);
        if (error == 0)
                error = bflush(em->em_dev);
        binval(em->em_dev);
        kmem_free(em->em_gd, (size_t)em->em_gdblocks * em->em_bsize);
        kmem_free(em, sizeof(*em));
        return error;
}

static int
ext2fs_root(struct vfs *vfsp, struct vnode **vpp)
{
        struct ext2mount *em = vfsp->vfs_data;
        struct ext2inode *ip;
        int error;

        mutex_lock(&em->em_lock);
        error = ext2fs_vget(em, EXT2_ROOTINO, &ip);
        mutex_unlock(&em->em_lock);
        if (error == 0)
                *vpp = ITOV(ip);
        return error;
}

static int
ext2fs_statvfs(struct vfs *vfsp, struct statvfs *sp)
{
        struct ext2mount *em = vfsp->vfs_data;
        const struct ext2fs_sb *sb = em->em_sb;

        memset(sp, 0, sizeof(*sp));
        mutex_lock(&em->em_lock);
        sp->f_bsize = sp->f_frsize = em->em_bsize;
        sp->f_blocks = sb->s_blocks_count;
        sp->f_bfree = sb->s_free_blocks_count;
        sp->f_bavail = sb->s_free_blocks_count > sb->s_r_blocks_count ?
            sb->s_free_blocks_count - sb->s_r_blocks_count : 0;
        sp->f_files = sb->s_inodes_count;
        sp->f_ffree = sp->f_favail = sb->s_free_inodes_count;
        mutex_unlock(&em->em_lock);
        sp->f_fsid = vfsp->vfs_fsid;
        memcpy(sp->f_basetype, "ext2fs", 7);
        sp->f_namemax = EXT2_MAXNAMLEN;
        return 0;
}

static int
ext2fs_sync(struct vfs *vfsp, struct cred *cr __unused)
{
        struct ext2mount *em = vfsp->vfs_data;
        int error = 0;

        vn_flush_vfs(vfsp);
        mutex_lock(&em->em_lock);
        /* The system is stopping: clean, as on unmount. */
        if (vfs_shutting_down && !em->em_ronly) {
                em->em_sb->s_state |= EXT2_VALID_FS;
                error = ext2fs_sbupdate(em);
        } else if (em->em_dirty) {
                error = ext2fs_sbupdate(em);
        }
        mutex_unlock(&em->em_lock);
        if (error == 0)
                error = bflush(em->em_dev);
        return error;
}
