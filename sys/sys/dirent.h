/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Directory entries.
 */

#ifndef _SYS_DIRENT_H_
#define _SYS_DIRENT_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

/*
 * A directory entry as VOP_READDIR returns it. Records are padded to
 * 8 bytes; d_reclen gives the length of each, d_off the offset of the
 * next one, to be passed back as the uio offset.
 */
struct dirent {
        ino_t           d_ino;
        off_t           d_off;
        uint16_t        d_reclen;
        char            d_name[];
};

#define DIRENT_RECLEN(namelen)                                          \
        ((offsetof(struct dirent, d_name) + (namelen) + 1 + 7) & ~(size_t)7)

#endif /* _SYS_DIRENT_H_ */
