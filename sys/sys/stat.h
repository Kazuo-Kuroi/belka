/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * File mode bits, with the System V values.
 */

#ifndef _SYS_STAT_H_
#define _SYS_STAT_H_

#include <stdint.h>

#include <sys/types.h>

#define S_IFMT          0170000
#define S_IFIFO         0010000
#define S_IFCHR         0020000
#define S_IFDIR         0040000
#define S_IFBLK         0060000
#define S_IFREG         0100000
#define S_IFLNK         0120000
#define S_IFSOCK        0140000

#define S_ISUID         04000
#define S_ISGID         02000
#define S_ISVTX         01000
#define S_IRWXU         00700
#define S_IRUSR         00400
#define S_IWUSR         00200
#define S_IXUSR         00100
#define S_IRWXG         00070
#define S_IRWXO         00007

/*
 * The native stat structure, for stat(), lstat(), and fstat(). The
 * UnixWare i386 layouts (stat, xstat) belong to the i386 personality.
 */
struct stat {
        dev_t           st_dev;
        ino_t           st_ino;
        mode_t          st_mode;
        nlink_t         st_nlink;
        uid_t           st_uid;
        gid_t           st_gid;
        dev_t           st_rdev;
        off_t           st_size;
        int64_t         st_atime_sec;
        int64_t         st_atime_nsec;
        int64_t         st_mtime_sec;
        int64_t         st_mtime_nsec;
        int64_t         st_ctime_sec;
        int64_t         st_ctime_nsec;
        int64_t         st_blksize;
        int64_t         st_blocks;
        char            st_fstype[16];
        int64_t         st_spare[4];
};

#endif /* _SYS_STAT_H_ */
