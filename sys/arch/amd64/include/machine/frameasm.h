/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Byte offsets of struct trapframe members, for assembly code. This
 * header contains only macros so that both C and assembly can include
 * it. trap.c asserts that each offset matches <machine/frame.h>.
 */

#ifndef _MACHINE_FRAMEASM_H_
#define _MACHINE_FRAMEASM_H_

#define TF_RDI          0
#define TF_RSI          8
#define TF_RDX          16
#define TF_RCX          24
#define TF_R8           32
#define TF_R9           40
#define TF_RAX          48
#define TF_RBX          56
#define TF_RBP          64
#define TF_R10          72
#define TF_R11          80
#define TF_R12          88
#define TF_R13          96
#define TF_R14          104
#define TF_R15          112
#define TF_TRAPNO       120
#define TF_ERR          128
#define TF_RIP          136
#define TF_CS           144
#define TF_RFLAGS       152
#define TF_RSP          160
#define TF_SS           168
#define TF_SIZE         176

#endif /* _MACHINE_FRAMEASM_H_ */
