/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 *
 * Start-up and system call stubs for the kernel's user-mode test
 * programs. They stand in for the C library until mlibc is ported.
 *
 * A system call puts its number in %rax and its arguments in %rdi,
 * %rsi, %rdx, %r10, %r8, and %r9. On return the carry flag says whether
 * it failed; if so %rax is the error number, otherwise %rax and %rdx are
 * the two results.
 */

        .text
        .globl  _start
        .type   _start, @function
_start:
        xorl    %ebp, %ebp
        movq    %rsp, %rdi              /* argc, argv, envp, auxv */
        call    __libt_start
        hlt
        .size   _start, . - _start

/* long __sysc(long n, long a1, long a2, long a3, long a4, long a5, long a6) */
        .globl  __sysc
        .type   __sysc, @function
__sysc:
        movq    %rdi, %rax
        movq    %rsi, %rdi
        movq    %rdx, %rsi
        movq    %rcx, %rdx
        movq    %r8, %r10
        movq    %r9, %r8
        movq    8(%rsp), %r9
        syscall
        jc      1f
        ret
1:
        movl    %eax, errno(%rip)
        movq    $-1, %rax
        ret
        .size   __sysc, . - __sysc

/* long __sysc2(long n, long a1, long a2, long a3, long *rv2): also %rdx */
        .globl  __sysc2
        .type   __sysc2, @function
__sysc2:
        pushq   %r8
        movq    %rdi, %rax
        movq    %rsi, %rdi
        movq    %rdx, %rsi
        movq    %rcx, %rdx
        syscall
        popq    %r8
        jc      1f
        movq    %rdx, (%r8)
        ret
1:
        movl    %eax, errno(%rip)
        movq    $-1, %rax
        ret
        .size   __sysc2, . - __sysc2

/* long __sysc_cf(long n, long a1, long *cf): the raw %rax and carry */
        .globl  __sysc_cf
        .type   __sysc_cf, @function
__sysc_cf:
        pushq   %rdx
        movq    %rdi, %rax
        movq    %rsi, %rdi
        syscall
        popq    %rdx
        setc    %cl
        movzbl  %cl, %ecx
        movq    %rcx, (%rdx)
        ret
        .size   __sysc_cf, . - __sysc_cf

        .section .note.GNU-stack,"",@progbits
