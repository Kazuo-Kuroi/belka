/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * User contexts: what getcontext() saves and setcontext() restores, and
 * what a signal handler is given about the interrupted code. The layout
 * is the native amd64 one.
 *
 * gregs follow the kernel's trap frame order, then the LWP private
 * pointer (%fs base). fpregs holds the floating-point and vector state
 * exactly as the processor saves it: an FXSAVE image, or an XSAVE image
 * whose first 512 bytes are the FXSAVE layout. setcontext() checks it
 * before loading it.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_UCONTEXT_H_
#define _SYS_UCONTEXT_H_

#include <stdint.h>

#include <sys/signal.h>

#define REG_RDI         0
#define REG_RSI         1
#define REG_RDX         2
#define REG_RCX         3
#define REG_R8          4
#define REG_R9          5
#define REG_RAX         6
#define REG_RBX         7
#define REG_RBP         8
#define REG_R10         9
#define REG_R11         10
#define REG_R12         11
#define REG_R13         12
#define REG_R14         13
#define REG_R15         14
#define REG_TRAPNO      15
#define REG_ERR         16
#define REG_RIP         17
#define REG_CS          18
#define REG_RFL         19
#define REG_RSP         20
#define REG_SS          21
#define REG_FSBASE      22
#define NGREG           23

typedef int64_t greg_t;
typedef greg_t gregset_t[NGREG];

#define FPREGS_SIZE     1024

typedef struct {
        uint8_t         fp_state[FPREGS_SIZE];
} fpregset_t;

typedef struct {
        gregset_t       gregs;
        fpregset_t      fpregs;
} mcontext_t;

typedef struct ucontext {
        uint64_t        uc_flags;
        struct ucontext *uc_link;
        sigset_t        uc_sigmask;
        stack_t         uc_stack;
        mcontext_t      uc_mcontext;
        int64_t         uc_filler[4];
} ucontext_t;

#define UC_SIGMASK      0x0001
#define UC_STACK        0x0002
#define UC_CPU          0x0004
#define UC_FP           0x0008
#define UC_MCONTEXT     (UC_CPU | UC_FP)
#define UC_ALL          (UC_SIGMASK | UC_STACK | UC_MCONTEXT)

/* context() commands (system call 100). */
#define GETCONTEXT      0
#define SETCONTEXT      1

#endif /* _SYS_UCONTEXT_H_ */
