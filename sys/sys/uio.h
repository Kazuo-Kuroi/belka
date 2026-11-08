/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Scatter/gather I/O descriptions, as in System V. Only kernel
 * (UIO_SYSSPACE) addresses exist until there are user processes.
 */

#ifndef _SYS_UIO_H_
#define _SYS_UIO_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

struct iovec {
        void            *iov_base;
        size_t          iov_len;
};

enum uio_rw { UIO_READ, UIO_WRITE };
enum uio_seg { UIO_SYSSPACE, UIO_USERSPACE };

struct uio {
        struct iovec    *uio_iov;
        int             uio_iovcnt;
        off_t           uio_offset;
        ssize_t         uio_resid;      /* bytes still to move */
        enum uio_seg    uio_segflg;
        enum uio_rw     uio_rw;
};

/* I/O flags to VOP_READ and VOP_WRITE. */
#define IO_APPEND       0x0001
#define IO_SYNC         0x0002

/*
 * Move up to n bytes between the kernel buffer cp and the area uio
 * describes, in direction rw, advancing uio. Returns 0 or an errno.
 */
int uiomove(void *, size_t, enum uio_rw, struct uio *);

#endif /* _SYS_UIO_H_ */
