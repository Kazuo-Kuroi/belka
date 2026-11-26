/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * The system console. Machine-dependent code installs a polled device
 * and can add an output mirror before secondary CPUs start.
 */

#ifndef _SYS_CONS_H_
#define _SYS_CONS_H_

#include <stddef.h>

struct consdev {
        void    (*cn_putc)(struct consdev *, int);
        void    (*cn_flush)(struct consdev *);  /* may be NULL */
        void    *cn_private;
        const char *cn_name;
};

void cninit(struct consdev *);
void cnmirror(struct consdev *);
void cnputc(int);
void cnflush(void);
void cnputc_raw(int);
void cnwrite(const char *, size_t);
void cons_input(int);
void cons_disconnect(void);

struct vnode;
struct vnode *cons_vnode(void);
struct vnode *cons_open_vnode(void);

#endif /* _SYS_CONS_H_ */
