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
