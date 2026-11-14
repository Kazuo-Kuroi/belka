/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Pseudo-terminals (kern/tty_pty.c), as devfs reaches them: opening
 * /dev/ptmx makes a pair and opens its master; /dev/pts/N names slave
 * N while its master is open. The ioctls are System V's.
 */

#ifndef _SYS_PTY_H_
#define _SYS_PTY_H_

#define PTY_MAX         64

#define ISPTM           (('P' << 8) | 1)        /* is this a master? */
#define UNLKPT          (('P' << 8) | 2)        /* let the slave be opened */

struct cred;
struct vfs;
struct vnode;

int     ptm_clone(struct vnode **, int, struct cred *);
int     pty_lookup(unsigned int, struct vnode **);
int     pty_next(unsigned int, unsigned int *);
void    pty_setvfs(struct vfs *);

#endif /* _SYS_PTY_H_ */
