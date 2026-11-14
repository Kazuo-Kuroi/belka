/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * File system statistics, as statvfs() and fstatvfs() (UnixWare's 103
 * and 104) report them: System V's struct (with f_basetype), its
 * fields 64 bits wide, as POSIX's unsigned long are here.
 * Counts of blocks are in f_frsize units.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_STATVFS_H_
#define _SYS_STATVFS_H_

#include <stdint.h>

#define FSTYPSZ         16

struct statvfs {
        uint64_t        f_bsize;        /* preferred I/O size */
        uint64_t        f_frsize;       /* fundamental block size */
        uint64_t        f_blocks;
        uint64_t        f_bfree;
        uint64_t        f_bavail;       /* free to the unprivileged */
        uint64_t        f_files;
        uint64_t        f_ffree;
        uint64_t        f_favail;
        uint64_t        f_fsid;
        char            f_basetype[FSTYPSZ];    /* "ext2fs", "tmpfs", ... */
        uint64_t        f_flag;         /* ST_* */
        uint64_t        f_namemax;
        uint64_t        f_spare[4];
};

/* f_flag */
#define ST_RDONLY       0x01
#define ST_NOSUID       0x02

#endif /* _SYS_STATVFS_H_ */
