/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 *
 * Exception entry. Each vector has a stub that makes the stack look the
 * same whether or not the processor pushed an error code, pushes the
 * vector number, and jumps to alltraps. alltraps saves the general
 * registers to complete a struct trapframe and calls trap().
 *
 * On entry from user mode the GS base is exchanged with swapgs, so that
 * the kernel always finds cpu_info through %gs. The decision is made
 * from the saved CS.
 *
 * KNOWN LIMITATION: deciding by CS is wrong for NMI, machine check, and
 * double fault, which can arrive in the kernel between a swapgs and the
 * iretq that follows it. Before user mode is enabled those three must
 * instead read MSR_GSBASE to decide. There is no user mode yet.
 */

#include <machine/asm.h>
#include <machine/frameasm.h>
#include <machine/trap.h>

/* A vector for which the processor pushes no error code. */
#define TRAP(num)                       \
ENTRY(Xtrap##num);                      \
        pushq   $0;                     \
        pushq   $num;                   \
        jmp     alltraps;               \
END(Xtrap##num)

/* A vector for which the processor pushes an error code. */
#define TRAP_ERR(num)                   \
ENTRY(Xtrap##num);                      \
        pushq   $num;                   \
        jmp     alltraps;               \
END(Xtrap##num)

TRAP(0)
TRAP(1)
TRAP(2)
TRAP(3)
TRAP(4)
TRAP(5)
TRAP(6)
TRAP(7)
TRAP_ERR(8)
TRAP(9)
TRAP_ERR(10)
TRAP_ERR(11)
TRAP_ERR(12)
TRAP_ERR(13)
TRAP_ERR(14)
TRAP(15)
TRAP(16)
TRAP_ERR(17)
TRAP(18)
TRAP(19)
TRAP(20)
TRAP_ERR(21)
TRAP(22)
TRAP(23)
TRAP(24)
TRAP(25)
TRAP(26)
TRAP(27)
TRAP(28)
TRAP_ERR(29)
TRAP_ERR(30)
TRAP(31)

/*
 * The stack holds the vector number, error code, and hardware frame.
 * The hardware aligned %rsp to 16 bytes before pushing, and the frame
 * is a multiple of 16 bytes, so %rsp is aligned again at the call.
 */
ENTRY(alltraps)
        subq    $TF_TRAPNO, %rsp
        movq    %rdi, TF_RDI(%rsp)
        movq    %rsi, TF_RSI(%rsp)
        movq    %rdx, TF_RDX(%rsp)
        movq    %rcx, TF_RCX(%rsp)
        movq    %r8, TF_R8(%rsp)
        movq    %r9, TF_R9(%rsp)
        movq    %rax, TF_RAX(%rsp)
        movq    %rbx, TF_RBX(%rsp)
        movq    %rbp, TF_RBP(%rsp)
        movq    %r10, TF_R10(%rsp)
        movq    %r11, TF_R11(%rsp)
        movq    %r12, TF_R12(%rsp)
        movq    %r13, TF_R13(%rsp)
        movq    %r14, TF_R14(%rsp)
        movq    %r15, TF_R15(%rsp)

        testb   $3, TF_CS(%rsp)
        jz      1f
        swapgs
1:
        cld
        movq    %rsp, %rdi
        call    trap

        testb   $3, TF_CS(%rsp)
        jz      2f
        swapgs
2:
        movq    TF_RDI(%rsp), %rdi
        movq    TF_RSI(%rsp), %rsi
        movq    TF_RDX(%rsp), %rdx
        movq    TF_RCX(%rsp), %rcx
        movq    TF_R8(%rsp), %r8
        movq    TF_R9(%rsp), %r9
        movq    TF_RAX(%rsp), %rax
        movq    TF_RBX(%rsp), %rbx
        movq    TF_RBP(%rsp), %rbp
        movq    TF_R10(%rsp), %r10
        movq    TF_R11(%rsp), %r11
        movq    TF_R12(%rsp), %r12
        movq    TF_R13(%rsp), %r13
        movq    TF_R14(%rsp), %r14
        movq    TF_R15(%rsp), %r15
        addq    $TF_RIP, %rsp
        iretq
END(alltraps)

        .section .rodata
        .p2align 3
        .globl  trap_vector_table
trap_vector_table:
        .quad   Xtrap0, Xtrap1, Xtrap2, Xtrap3
        .quad   Xtrap4, Xtrap5, Xtrap6, Xtrap7
        .quad   Xtrap8, Xtrap9, Xtrap10, Xtrap11
        .quad   Xtrap12, Xtrap13, Xtrap14, Xtrap15
        .quad   Xtrap16, Xtrap17, Xtrap18, Xtrap19
        .quad   Xtrap20, Xtrap21, Xtrap22, Xtrap23
        .quad   Xtrap24, Xtrap25, Xtrap26, Xtrap27
        .quad   Xtrap28, Xtrap29, Xtrap30, Xtrap31

/*
 * Stubs for vectors 32 to 255, each aligned to 16 bytes so that the
 * stub for vector v is at intr_stubs + 16 * (v - 32). idt_init() checks
 * that the assembler kept every stub within its slot.
 */
        .text
        .p2align 4
        .globl  intr_stubs
intr_stubs:
        .set    intr_vec, 32
        .rept   224
        .p2align 4
        pushq   $0
        pushq   $intr_vec
        jmp     alltraps
        .set    intr_vec, intr_vec + 1
        .endr
        .p2align 4
        .globl  intr_stubs_end
intr_stubs_end:
