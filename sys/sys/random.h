/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-23
 * SPDX-License-Identifier: MIT
 */

/* The kernel's random numbers (kern/kern_random.c). */

#ifndef _SYS_RANDOM_H_
#define _SYS_RANDOM_H_

#ifdef _KERNEL

#include <stddef.h>

struct uio;

void    rnd_init(void);
void    rnd_intr(int);
void    rnd_add(const void *, size_t);
void    rnd_get(void *, size_t);
int     rnd_read(struct uio *);
int     rnd_write(struct uio *);
int     rnd_selftest(void);

#endif /* _KERNEL */

#endif /* _SYS_RANDOM_H_ */
