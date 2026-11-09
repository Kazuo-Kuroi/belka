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
 * the two results. A call that fails with ERESTART (91) was interrupted
 * by a signal whose handler asked for restarts, and is issued again with
 * the same arguments, which the kernel leaves in their registers.
 *
 * __sigtramp is where a signal handler returns: the kernel leaves the
 * handler's ucontext_t in %rbx, which the handler preserves, and the
 * trampoline resumes it with setcontext.
 */

#define ERESTART        91

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
        pushq   %rdi                    /* the number, for a restart */
        movq    %rsi, %rdi
        movq    %rdx, %rsi
        movq    %rcx, %rdx
        movq    %r8, %r10
        movq    %r9, %r8
        movq    16(%rsp), %r9
1:
        movq    (%rsp), %rax
        syscall
        jnc     2f
        cmpq    $ERESTART, %rax
        je      1b
        movl    %eax, errno(%rip)
        movq    $-1, %rax
2:
        addq    $8, %rsp
        ret
        .size   __sysc, . - __sysc

/* long __sysc2(long n, long a1, long a2, long a3, long *rv2): also %rdx */
        .globl  __sysc2
        .type   __sysc2, @function
__sysc2:
        pushq   %r8
        pushq   %rdi
        movq    %rsi, %rdi
        movq    %rdx, %rsi
        movq    %rcx, %rdx
1:
        movq    (%rsp), %rax
        syscall
        jnc     2f
        cmpq    $ERESTART, %rax
        je      1b
        addq    $8, %rsp
        popq    %r8
        movl    %eax, errno(%rip)
        movq    $-1, %rax
        ret
2:
        addq    $8, %rsp
        popq    %r8
        movq    %rdx, (%r8)
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

/*
 * int getcontext(ucontext_t *uc): the kernel saves the registers as at
 * the system call; the context is then made to resume as a return from
 * this function, with our caller's %rip and %rsp and 0 in %rax, so that
 * it stays valid after this frame is gone. The offsets are those of
 * uc_mcontext.gregs[REG_RIP], [REG_RSP], and [REG_RAX]; libt.c checks
 * them.
 */
#define UC_RIP          192
#define UC_RSP          216
#define UC_RAX          104

        .globl  getcontext
        .type   getcontext, @function
getcontext:
        pushq   %rdi
        movq    %rdi, %rsi
        xorl    %edi, %edi              /* GETCONTEXT */
        movl    $100, %eax
        syscall
        popq    %rdi
        jc      1f
        movq    (%rsp), %rax
        movq    %rax, UC_RIP(%rdi)
        leaq    8(%rsp), %rax
        movq    %rax, UC_RSP(%rdi)
        movq    $0, UC_RAX(%rdi)
        xorl    %eax, %eax
        ret
1:
        movl    %eax, errno(%rip)
        movl    $-1, %eax
        ret
        .size   getcontext, . - getcontext

/* Where signal handlers return: setcontext(SETCONTEXT, %rbx). */
        .globl  __sigtramp
        .type   __sigtramp, @function
__sigtramp:
        movq    %rbx, %rsi
        movl    $1, %edi
        movl    $100, %eax
        syscall
        hlt                             /* setcontext does not return */
        .size   __sigtramp, . - __sigtramp

        .section .note.GNU-stack,"",@progbits
