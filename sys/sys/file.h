/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * File access flags, and open files.
 *
 * A struct file is one open of a vnode: its access mode, offset, and
 * credentials, shared by every descriptor that dup or fork made from
 * it. f_offlock serialises reads and writes through it, so that the
 * offset moves atomically.
 */

#ifndef _SYS_FILE_H_
#define _SYS_FILE_H_

#include <stdint.h>

#include <sys/synch.h>
#include <sys/types.h>

/* Open flags, as the kernel sees them (System V values). */
#define FREAD           0x0001
#define FWRITE          0x0002
#define FNDELAY         0x0004
#define FAPPEND         0x0008
#define FSYNC           0x0010
#define FNONBLOCK       0x0080
#define FCREAT          0x0100
#define FTRUNC          0x0200
#define FEXCL           0x0400
#define FNOCTTY         0x0800

/* What fcntl(F_SETFL) may change. */
#define FMASK_SETFL     (FNDELAY | FAPPEND | FSYNC | FNONBLOCK)

struct cred;
#define FD_AIO          0x02    /* libc's private close-on-exec AIO reference */
struct vnode;

struct file {
        volatile uint32_t f_count;
        int             f_flag;
        struct mutex    f_offlock;
        off_t           f_offset;
        struct vnode    *f_vnode;
        struct cred     *f_cred;
};

struct file *falloc(struct vnode *, int);
void fhold(struct file *);
void frele(struct file *);

#endif /* _SYS_FILE_H_ */
