/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * The system console. During bring-up the console is a single
 * polled output device that machine-dependent code chooses and
 * registers with cninit(). Kernel output goes through cnputc().
 */

#ifndef _SYS_CONS_H_
#define _SYS_CONS_H_

struct consdev {
        void    (*cn_putc)(struct consdev *, int);
        void    (*cn_flush)(struct consdev *);  /* may be NULL */
        void    *cn_private;
        const char *cn_name;
};

void cninit(struct consdev *);
void cnputc(int);
void cnflush(void);

#endif /* _SYS_CONS_H_ */
