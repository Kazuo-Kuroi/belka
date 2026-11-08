/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The second extended file system: on-disk format and in-core state.
 *
 * Written from the public description of the ext2 format, organised
 * after NetBSD's ext2fs (vfsops, vnops, lookup, alloc, balloc, inode)
 * and using its allocation policies; no code is taken from NetBSD or
 * Linux.
 *
 * Supported: revisions 0 and 1; block sizes 1, 2, and 4 KB; inodes of
 * 128 bytes or more; the FILETYPE incompatible feature; SPARSE_SUPER and
 * LARGE_FILE read-only-compatible features; and any compatible feature,
 * of which dir_index, resize_inode, and ext_attr need care: a directory
 * changed here loses its hash index flag (it stays valid as a linear
 * directory), reserved group descriptor blocks are already marked in
 * the block bitmaps, and an inode with an extended attribute block is
 * never freed here. Other incompatible features refuse the mount; other
 * read-only-compatible features force a read-only mount.
 *
 * All on-disk structures are naturally aligned, so plain C structures
 * describe them exactly on little-endian amd64; ext2fs_subr.c checks
 * their sizes and offsets.
 *
 * Locking:
 * - em_lock (a mutex, one per mount) serialises the name space, block
 *   and inode allocation, the superblock and group descriptors, and the
 *   in-core inode table.
 * - i_rwlock (an mrlock, one per inode) guards a regular file's data
 *   and block map: readers share it, writers and truncation take it
 *   alone. Order: i_rwlock, then em_lock.
 * - Directories are changed and searched only under em_lock.
 */

#ifndef _FS_EXT2FS_EXT2FS_H_
#define _FS_EXT2FS_EXT2FS_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/synch.h>
#include <sys/types.h>
#include <sys/vnode.h>

struct blkdev;
struct buf;
struct vfs;

#define EXT2_SBOFF              1024    /* byte offset of the superblock */
#define EXT2_SBSIZE             1024
#define EXT2_MAGIC              0xef53
#define EXT2_ROOTINO            2
#define EXT2_GOOD_OLD_FIRST_INO 11
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_NDADDR             12      /* direct blocks */
#define EXT2_NIADDR             3       /* single, double, triple indirect */
#define EXT2_MAXSYMLINKLEN      60      /* fast symlinks live in i_block */
#define EXT2_LINK_MAX           32000

/* s_state */
#define EXT2_VALID_FS           0x0001
#define EXT2_ERROR_FS           0x0002

/* Features. */
#define EXT2F_COMPAT_DIRHASHINDEX       0x0020
#define EXT2F_INCOMPAT_FILETYPE         0x0002
#define EXT2F_ROCOMPAT_SPARSESUPER      0x0001
#define EXT2F_ROCOMPAT_LARGEFILE        0x0002

#define EXT2F_INCOMPAT_SUPP     EXT2F_INCOMPAT_FILETYPE
#define EXT2F_ROCOMPAT_SUPP     (EXT2F_ROCOMPAT_SPARSESUPER | \
                                 EXT2F_ROCOMPAT_LARGEFILE)

/* The superblock, as far as Belka uses it; 1024 bytes on disk. */
struct ext2fs_sb {
        uint32_t        s_inodes_count;
        uint32_t        s_blocks_count;
        uint32_t        s_r_blocks_count;
        uint32_t        s_free_blocks_count;
        uint32_t        s_free_inodes_count;
        uint32_t        s_first_data_block;
        uint32_t        s_log_block_size;
        uint32_t        s_log_frag_size;
        uint32_t        s_blocks_per_group;
        uint32_t        s_frags_per_group;
        uint32_t        s_inodes_per_group;
        uint32_t        s_mtime;
        uint32_t        s_wtime;
        uint16_t        s_mnt_count;
        int16_t         s_max_mnt_count;
        uint16_t        s_magic;
        uint16_t        s_state;
        uint16_t        s_errors;
        uint16_t        s_minor_rev_level;
        uint32_t        s_lastcheck;
        uint32_t        s_checkinterval;
        uint32_t        s_creator_os;
        uint32_t        s_rev_level;
        uint16_t        s_def_resuid;
        uint16_t        s_def_resgid;
        /* Revision 1 (dynamic) only. */
        uint32_t        s_first_ino;
        uint16_t        s_inode_size;
        uint16_t        s_block_group_nr;
        uint32_t        s_feature_compat;
        uint32_t        s_feature_incompat;
        uint32_t        s_feature_ro_compat;
        uint8_t         s_uuid[16];
        char            s_volume_name[16];
};

/* A block group descriptor; 32 bytes on disk. */
struct ext2fs_gd {
        uint32_t        bg_block_bitmap;
        uint32_t        bg_inode_bitmap;
        uint32_t        bg_inode_table;
        uint16_t        bg_free_blocks_count;
        uint16_t        bg_free_inodes_count;
        uint16_t        bg_used_dirs_count;
        uint16_t        bg_pad;
        uint32_t        bg_reserved[3];
};

/* The first 128 bytes of an inode. */
struct ext2fs_dinode {
        uint16_t        e2di_mode;
        uint16_t        e2di_uid;
        uint32_t        e2di_size;
        uint32_t        e2di_atime;
        uint32_t        e2di_ctime;
        uint32_t        e2di_mtime;
        uint32_t        e2di_dtime;
        uint16_t        e2di_gid;
        uint16_t        e2di_nlink;
        uint32_t        e2di_nblock;    /* 512-byte sectors held */
        uint32_t        e2di_flags;
        uint32_t        e2di_version;
        uint32_t        e2di_blocks[EXT2_NDADDR + EXT2_NIADDR];
        uint32_t        e2di_gen;
        uint32_t        e2di_facl;      /* extended attribute block */
        uint32_t        e2di_size_hi;   /* large files */
        uint32_t        e2di_faddr;
        uint8_t         e2di_frag;
        uint8_t         e2di_fsize;
        uint16_t        e2di_nblock_hi;
        uint16_t        e2di_uid_high;
        uint16_t        e2di_gid_high;
        uint32_t        e2di_linux_reserved3;
};

#define EXT2_INDEX_FL           0x00001000  /* hashed directory */

/* A directory entry: header, then the name, padded to 4 bytes. */
struct ext2fs_direct {
        uint32_t        e2d_ino;
        uint16_t        e2d_reclen;
        uint8_t         e2d_namlen;
        uint8_t         e2d_type;
        char            e2d_name[];
};

#define EXT2_DIRENT_HDR         8
#define EXT2_DIR_REC_LEN(n)     ((EXT2_DIRENT_HDR + (n) + 3) & ~3u)
#define EXT2_MAXNAMLEN          255

/* e2d_type, with the FILETYPE feature. */
#define EXT2_FT_UNKNOWN         0
#define EXT2_FT_REG_FILE        1
#define EXT2_FT_DIR             2
#define EXT2_FT_CHRDEV          3
#define EXT2_FT_BLKDEV          4
#define EXT2_FT_FIFO            5
#define EXT2_FT_SOCK            6
#define EXT2_FT_SYMLINK         7

/* Mount argument: the block device by name. */
struct ext2fs_args {
        const char      *ea_dev;
        int             ea_rdonly;
};

/* One mounted file system. */
struct ext2mount {
        struct mutex    em_lock;
        struct vfs      *em_vfs;
        struct blkdev   *em_dev;
        uint8_t         em_sbraw[EXT2_SBSIZE];  /* the superblock */
        struct ext2fs_sb *em_sb;                /* overlays em_sbraw */
        struct ext2fs_gd *em_gd;                /* all descriptors */
        uint32_t        em_ngroups;
        uint32_t        em_gdblocks;            /* blocks of descriptors */
        uint32_t        em_bsize;
        uint32_t        em_bsects;              /* sectors per block */
        uint32_t        em_ipb;                 /* inodes per block */
        uint32_t        em_inodesize;
        uint32_t        em_nindir;              /* pointers per block */
        uint32_t        em_first_ino;
        int             em_ronly;
        int             em_filetype;            /* FILETYPE feature */
        int             em_largefile;
        int             em_dirty;               /* sb or gd changed */
        struct ext2inode *em_ihash[64];
        uint32_t        em_ninodes;             /* in core */
};

/* One in-core inode, with its vnode. */
struct ext2inode {
        struct vnode    i_vnode;
        struct ext2inode *i_hnext;
        struct ext2mount *i_mnt;
        uint32_t        i_number;
        struct mrlock   i_rwlock;
        struct ext2fs_dinode i_din;
        uint64_t        i_size;
        uint32_t        i_lastblk;      /* allocation hint */
};

#define VTOI(vp)        ((struct ext2inode *)(vp)->v_data)
#define ITOV(ip)        (&(ip)->i_vnode)

extern const struct vnodeops ext2fs_vnodeops;

/* ext2fs_vfsops.c */
int ext2fs_vget(struct ext2mount *, uint32_t, struct ext2inode **);
int ext2fs_newinode(struct ext2mount *, struct ext2inode *, uint16_t,
    struct ext2inode **);
void ext2fs_rele_locked(struct ext2inode *);
void ext2fs_inactive_locked(struct ext2inode *);
int ext2fs_iupdate(struct ext2inode *);
void ext2fs_sbdirty(struct ext2mount *);
int ext2fs_sbupdate(struct ext2mount *);

/* ext2fs_alloc.c */
int ext2fs_balloc_block(struct ext2mount *, uint32_t, uint32_t *);
void ext2fs_bfree(struct ext2mount *, uint32_t);
int ext2fs_ialloc(struct ext2mount *, struct ext2inode *, int, uint32_t *);
void ext2fs_ifree(struct ext2mount *, uint32_t, int);
uint32_t ext2fs_group_first_block(const struct ext2mount *, uint32_t);

/* ext2fs_balloc.c */
int ext2fs_bmap(struct ext2inode *, uint64_t, uint32_t *, int, int *);
int ext2fs_truncate(struct ext2inode *, uint64_t);

/* ext2fs_lookup.c */
int ext2fs_dirlook(struct ext2inode *, const char *, uint32_t *);
int ext2fs_direnter(struct ext2inode *, const char *, uint32_t, int);
int ext2fs_dirremove(struct ext2inode *, const char *);
int ext2fs_dirempty(struct ext2inode *);
int ext2fs_dirinit(struct ext2inode *, uint32_t);
int ext2fs_setdotdot(struct ext2inode *, uint32_t);
int ext2fs_readdir_uio(struct ext2inode *, struct uio *, int *);

/* ext2fs_subr.c */
int ext2fs_bread(struct ext2mount *, uint32_t, struct buf **);
struct buf *ext2fs_getblk(struct ext2mount *, uint32_t);
enum vtype ext2fs_vtype(uint16_t);
int ext2fs_ftype(enum vtype);
void ext2fs_setsize(struct ext2inode *, uint64_t);
uint32_t ext2fs_now(void);

#endif /* _FS_EXT2FS_EXT2FS_H_ */
