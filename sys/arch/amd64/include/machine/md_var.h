/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Declarations shared within the amd64 machine-dependent code.
 */

#ifndef _MACHINE_MD_VAR_H_
#define _MACHINE_MD_VAR_H_

#include <sys/compiler.h>

struct trapframe;

/* machdep.c: called from locore.S on the bootstrap processor. */
void amd64_init(void) __dead;

/* trap.c */
void trap(struct trapframe *);
extern volatile int trap_expect;
extern volatile int trap_expect_skip;
extern volatile int trap_expect_seen;

/* Symbols defined by the linker script and locore.S. */
extern char kernel_start[];
extern char kernel_end[];
extern char boot_stack[];
extern char boot_stack_top[];

#endif /* _MACHINE_MD_VAR_H_ */
