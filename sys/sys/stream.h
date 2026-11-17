/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_STREAM_H_
#define _SYS_STREAM_H_

#include <stdint.h>

struct cred;
struct proc;
struct stdata;
struct vnode;

int strloop_clone(struct vnode **, struct cred *);
int strsig_ioctl(struct proc *, int, unsigned long, uint64_t);
void strsig_close(struct stdata *, struct proc *);

#endif /* _SYS_STREAM_H_ */
