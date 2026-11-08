/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Processor exception handling.
 *
 * Nothing can be recovered yet: there is no user mode, no demand
 * paging, and no fault-catching for copyin/copyout. Every exception
 * except those a self-test expects therefore ends in a panic, after a
 * register dump and a frame-pointer backtrace of the interrupted code.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/compiler.h>
#include <sys/systm.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/frame.h>
#include <machine/frameasm.h>
#include <machine/md_var.h>
#include <machine/segments.h>
#include <machine/trap.h>

/* The assembly offsets must match the structure. */
CTASSERT(offsetof(struct trapframe, tf_rdi) == TF_RDI);
CTASSERT(offsetof(struct trapframe, tf_rbp) == TF_RBP);
CTASSERT(offsetof(struct trapframe, tf_r15) == TF_R15);
CTASSERT(offsetof(struct trapframe, tf_trapno) == TF_TRAPNO);
CTASSERT(offsetof(struct trapframe, tf_err) == TF_ERR);
CTASSERT(offsetof(struct trapframe, tf_rip) == TF_RIP);
CTASSERT(offsetof(struct trapframe, tf_cs) == TF_CS);
CTASSERT(offsetof(struct trapframe, tf_ss) == TF_SS);
CTASSERT(sizeof(struct trapframe) == TF_SIZE);
CTASSERT(TF_SIZE % 16 == 0);

#define BACKTRACE_DEPTH 16

/* The kernel half of the address space starts here. */
#define KERNEL_BASE     UINT64_C(0xffff800000000000)

static const char *const trap_names[NEXCEPTIONS] = {
        "divide error",
        "debug exception",
        "non-maskable interrupt",
        "breakpoint",
        "overflow",
        "bound range exceeded",
        "invalid opcode",
        "device not available",
        "double fault",
        "coprocessor segment overrun",
        "invalid TSS",
        "segment not present",
        "stack fault",
        "general protection fault",
        "page fault",
        "reserved (15)",
        "x87 floating-point error",
        "alignment check",
        "machine check",
        "SIMD floating-point exception",
        "virtualization exception",
        "control protection fault",
        "reserved (22)",
        "reserved (23)",
        "reserved (24)",
        "reserved (25)",
        "reserved (26)",
        "reserved (27)",
        "hypervisor injection",
        "VMM communication",
        "security exception",
        "reserved (31)"
};

void
trap_expect(int vector, int skip)
{
        struct cpu_info *ci = curcpu();

        ci->ci_texp_hit = 0;
        ci->ci_texp_err = 0;
        ci->ci_texp_skip = skip;
        ci->ci_texp_vec = vector + 1;
}

int
trap_expect_done(uint64_t *errp)
{
        struct cpu_info *ci = curcpu();

        ci->ci_texp_vec = 0;
        if (errp != NULL)
                *errp = ci->ci_texp_err;
        return ci->ci_texp_hit;
}

static void trap_dump(const struct trapframe *);
static void trap_backtrace(uint64_t, uint64_t);

static void
trap_dump(const struct trapframe *tf)
{
        printf("rip %016jx  cs %04jx  rflags %08jx\n",
            (uintmax_t)tf->tf_rip, (uintmax_t)tf->tf_cs,
            (uintmax_t)tf->tf_rflags);
        printf("rsp %016jx  ss %04jx  err %jx\n",
            (uintmax_t)tf->tf_rsp, (uintmax_t)tf->tf_ss,
            (uintmax_t)tf->tf_err);
        printf("rax %016jx  rbx %016jx  rcx %016jx\n",
            (uintmax_t)tf->tf_rax, (uintmax_t)tf->tf_rbx,
            (uintmax_t)tf->tf_rcx);
        printf("rdx %016jx  rsi %016jx  rdi %016jx\n",
            (uintmax_t)tf->tf_rdx, (uintmax_t)tf->tf_rsi,
            (uintmax_t)tf->tf_rdi);
        printf("rbp %016jx  r8  %016jx  r9  %016jx\n",
            (uintmax_t)tf->tf_rbp, (uintmax_t)tf->tf_r8,
            (uintmax_t)tf->tf_r9);
        printf("r10 %016jx  r11 %016jx  r12 %016jx\n",
            (uintmax_t)tf->tf_r10, (uintmax_t)tf->tf_r11,
            (uintmax_t)tf->tf_r12);
        printf("r13 %016jx  r14 %016jx  r15 %016jx\n",
            (uintmax_t)tf->tf_r13, (uintmax_t)tf->tf_r14,
            (uintmax_t)tf->tf_r15);
        printf("cr0 %016jx  cr2 %016jx  cr3 %016jx  cr4 %016jx\n",
            (uintmax_t)rcr0(), (uintmax_t)rcr2(), (uintmax_t)rcr3(),
            (uintmax_t)rcr4());
}

/*
 * Walk the saved frame-pointer chain starting at rbp. The kernel is
 * built with frame pointers, so each frame begins with the caller's
 * %rbp followed by the return address. The walk stops at the first
 * frame that is not an aligned kernel address or does not move up the
 * stack, so a corrupt chain cannot fault.
 */
static void
trap_backtrace(uint64_t rip, uint64_t rbp)
{
        printf("backtrace:\n  %016jx\n", (uintmax_t)rip);
        for (int depth = 0; depth < BACKTRACE_DEPTH; depth++) {
                const uint64_t *frame;
                uint64_t next;

                if (rbp < KERNEL_BASE || (rbp & 7) != 0)
                        break;
                frame = (const uint64_t *)(uintptr_t)rbp;
                if (frame[1] == 0)
                        break;
                printf("  %016jx\n", (uintmax_t)frame[1]);
                next = frame[0];
                if (next <= rbp)
                        break;
                rbp = next;
        }
}

/*
 * Common C entry for all exceptions, called from alltraps with
 * interrupts disabled.
 */
void
trap(struct trapframe *tf)
{
        struct cpu_info *ci = curcpu();
        uint64_t vector = tf->tf_trapno;
        int user = (tf->tf_cs & SEL_RPL_MASK) != 0;
        const char *name;

        if (!user && ci->ci_texp_vec != 0 &&
            vector == (uint64_t)(ci->ci_texp_vec - 1)) {
                ci->ci_texp_vec = 0;
                ci->ci_texp_hit = 1;
                ci->ci_texp_err = tf->tf_err;
                if (ci->ci_texp_skip == TRAP_RESUME_RETURN) {
                        const uint64_t *sp =
                            (const uint64_t *)(uintptr_t)tf->tf_rsp;

                        tf->tf_rip = sp[0];
                        tf->tf_rsp += sizeof(uint64_t);
                } else {
                        tf->tf_rip += (uint64_t)ci->ci_texp_skip;
                }
                return;
        }

        name = (vector < NEXCEPTIONS) ? trap_names[vector] : "unknown trap";
        printf("\ncpu%u: fatal %s trap: %s (vector %ju)\n", cpu_number(),
            user ? "user" : "kernel", name, (uintmax_t)vector);
        if (vector == PGFLT) {
                printf("fault address %016jx: %s %s, %s\n",
                    (uintmax_t)rcr2(),
                    (tf->tf_err & PGEX_I) != 0 ? "fetch" :
                    (tf->tf_err & PGEX_W) != 0 ? "write" : "read",
                    (tf->tf_err & PGEX_U) != 0 ? "user" : "kernel",
                    (tf->tf_err & PGEX_P) != 0 ? "protection violation" :
                    "page not present");
        }
        trap_dump(tf);
        if (!user)
                trap_backtrace(tf->tf_rip, tf->tf_rbp);
        panic("%s", name);
}
