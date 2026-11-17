/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-15
 * SPDX-License-Identifier: MIT
 */

/*
 * Locks on files (kern/vfs_reclock.c): fcntl's record locks, as System
 * V has them, and BSD's flock.
 *
 * A record lock (F_GETLK, F_SETLK, F_SETLKW) covers a range of bytes of
 * a file, which may run on past its end, and is a shared (read) or an
 * exclusive (write) one. It belongs to a process, not to an open file:
 * whatever descriptor the process uses, its locks on the file are one
 * set, replaced range by range as it sets new ones; they are not
 * inherited across fork, and every one of them on a file goes when the
 * process closes any descriptor for that file, and so when it exits.
 *
 * A flock lock covers the whole file and belongs to the open file: the
 * descriptors that share it (by dup, or across fork) share the lock, and
 * it goes with LOCK_UN or when the last of them is closed.
 *
 * The two kinds conflict with each other, as any two owners' locks do.
 * All are advisory: reads and writes do not look at them.
 *
 * struct flock is Belka's native layout (the C library's, without
 * System V's l_sysid, which only remote locks use).
 */

#ifndef _SYS_FLOCK_H_
#define _SYS_FLOCK_H_

#include <stdint.h>

#include <sys/types.h>

#define F_SETLK         6       /* set or clear; EAGAIN if it conflicts */
#define F_SETLKW        7       /* set, waiting while it conflicts */
#define F_GETLK         14      /* the first lock that would conflict */

#define F_RDLCK         1       /* shared */
#define F_WRLCK         2       /* exclusive */
#define F_UNLCK         3       /* none: clear */

/* flock(fd, op). */
#define LOCK_SH         1       /* shared */
#define LOCK_EX         2       /* exclusive */
#define LOCK_NB         4       /* or with them: EWOULDBLOCK, not a wait */
#define LOCK_UN         8       /* none */

#define RECLOCK_MAX     8192    /* locks in all, the default */

struct flock {
        int16_t         l_type;         /* F_RDLCK, F_WRLCK, F_UNLCK */
        int16_t         l_whence;       /* SEEK_SET, SEEK_CUR, SEEK_END */
        int64_t         l_start;        /* from l_whence */
        int64_t         l_len;          /* 0: to the end, and beyond */
        int32_t         l_pid;          /* F_GETLK: the owner; -1: flock */
};

struct file;
struct vnode;

extern unsigned int reclock_max;

void reclock_init(void);
int reclock_fcntl(struct file *, int, struct flock *);
int reclock_flock(struct file *, int);
void reclock_release(struct vnode *, const void *);

#endif /* _SYS_FLOCK_H_ */
