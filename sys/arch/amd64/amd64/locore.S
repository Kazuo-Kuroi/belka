/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 *
 * Kernel entry point. Limine enters here on the bootstrap processor in
 * 64-bit mode with paging enabled, interrupts disabled, and the kernel
 * mapped at its link address. Switch to a stack the kernel owns, then
 * continue in C. amd64_init() does not return.
 */

#include <machine/asm.h>

#define BOOT_STACK_SIZE 16384

ENTRY(start)
        cli
        cld
        leaq    boot_stack_top(%rip), %rsp
        xorl    %ebp, %ebp
        call    amd64_init
1:
        cli
        hlt
        jmp     1b
END(start)

        .bss
        .p2align 12
        .globl  boot_stack
boot_stack:
        .skip   BOOT_STACK_SIZE
        .globl  boot_stack_top
boot_stack_top:

/*
 * void stack_switch_call(uint64_t top, void (*fn)(void *), void *arg)
 *
 * Switch to the stack whose top is given and call fn(arg) there, with
 * no frame to return to. Used to move a processor off the boot
 * loader's stack.
 */
ENTRY(stack_switch_call)
        movq    %rdi, %rsp
        xorl    %ebp, %ebp
        movq    %rdx, %rdi
        call    *%rsi
2:
        cli
        hlt
        jmp     2b
END(stack_switch_call)

/*
 * void cpu_switch(uint64_t *old_rsp, uint64_t new_rsp)
 *
 * Save the callee-saved registers on the current stack, store the stack
 * pointer in *old_rsp, load new_rsp, and restore the registers saved
 * there. The return then resumes the other thread where it last called
 * cpu_switch, or, for a new thread, at thread_start.
 */
ENTRY(cpu_switch)
        pushq   %rbp
        pushq   %rbx
        pushq   %r12
        pushq   %r13
        pushq   %r14
        pushq   %r15
        movq    %rsp, (%rdi)
        movq    %rsi, %rsp
        popq    %r15
        popq    %r14
        popq    %r13
        popq    %r12
        popq    %rbx
        popq    %rbp
        ret
END(cpu_switch)

/*
 * First code of a new thread, reached by cpu_switch's ret. %r12 holds
 * the kthread; cpu_thread_setup() aligned the stack so that %rsp is a
 * multiple of 16 here.
 */
ENTRY(thread_start)
        xorl    %ebp, %ebp
        movq    %r12, %rdi
        call    kthread_start
1:
        cli
        hlt
        jmp     1b
END(thread_start)
