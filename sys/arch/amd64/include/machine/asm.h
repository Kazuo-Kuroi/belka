/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Macros for amd64 assembly sources.
 */

#ifndef _MACHINE_ASM_H_
#define _MACHINE_ASM_H_

#define _ALIGN_TEXT     .p2align 4

/* Start a global function. */
#define ENTRY(name)                     \
        .text;                          \
        _ALIGN_TEXT;                    \
        .globl name;                    \
        .type name, @function;          \
name:

/* End a function started with ENTRY, recording its size. */
#define END(name)                       \
        .size name, . - name

#endif /* _MACHINE_ASM_H_ */
