/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * The trap frame: the register state saved on the kernel stack by the
 * hardware and by alltraps in vector.S. The assembly code uses the
 * offsets in <machine/frameasm.h>; trap.c checks at compile time that
 * they match this structure.
 */

#ifndef _MACHINE_FRAME_H_
#define _MACHINE_FRAME_H_

#include <stdint.h>

struct trapframe {
        /* Saved by alltraps. */
        uint64_t        tf_rdi;
        uint64_t        tf_rsi;
        uint64_t        tf_rdx;
        uint64_t        tf_rcx;
        uint64_t        tf_r8;
        uint64_t        tf_r9;
        uint64_t        tf_rax;
        uint64_t        tf_rbx;
        uint64_t        tf_rbp;
        uint64_t        tf_r10;
        uint64_t        tf_r11;
        uint64_t        tf_r12;
        uint64_t        tf_r13;
        uint64_t        tf_r14;
        uint64_t        tf_r15;

        /* Pushed by the vector stub. */
        uint64_t        tf_trapno;
        uint64_t        tf_err;         /* hardware error code, or 0 */

        /* Pushed by the processor. */
        uint64_t        tf_rip;
        uint64_t        tf_cs;
        uint64_t        tf_rflags;
        uint64_t        tf_rsp;
        uint64_t        tf_ss;
};

#endif /* _MACHINE_FRAME_H_ */
