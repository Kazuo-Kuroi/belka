/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-13
 * SPDX-License-Identifier: MIT
 */

/*
 * The kernel's own interfaces for the tests built against mlibc, which
 * check the system calls themselves where the C library has no call or
 * does more than make it: LWPs and their semaphores and address waits,
 * fork1 and forkall, contexts, the break, the old signal(), sigwait,
 * and the raw result of a call.
 *
 * Every call here is a system call made inline, returning an error
 * number rather than setting errno, so that LWPs started with
 * _lwp_create, which have no thread block of the C library's (their
 * %fs base is 0), may use them; such an LWP must not call into mlibc.
 * A call that fails with ERESTART is made again, as the library does.
 *
 * Include this in one source file of a program only: it defines
 * rawsys_getcontext in assembly. mlibc gives the REG_ names only with
 * _GNU_SOURCE.
 */

#ifndef _RAWSYS_H_
#define _RAWSYS_H_

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#include "../../sys/sys/lwp.h"
#include "../../sys/sys/syscall.h"

#define RAWSYS_ERESTART 91
#define RAWSYS_GETCONTEXT 0
#define RAWSYS_SETCONTEXT 1

/* A call's two results, and its error number (0 if it succeeded). */
struct rawsys {
        long    rv;
        long    rv2;
        int     error;
};

/*
 * System call n. %rdx goes in as a3 and the kernel hands it back as the
 * second result unless the call sets one (fork's child flag).
 */
static inline struct rawsys
rawsys6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
        struct rawsys r;

        for (;;) {
                register long r10 __asm__("r10") = a4;
                register long r8 __asm__("r8") = a5;
                register long r9 __asm__("r9") = a6;
                long rax = n, rdi = a1, rsi = a2, rdx = a3;
                unsigned char cf;

                __asm__ __volatile__("syscall\n\tsetc %2"
                    : "+a"(rax), "+d"(rdx), "=q"(cf), "+D"(rdi), "+S"(rsi),
                      "+r"(r10), "+r"(r8), "+r"(r9)
                    :
                    : "rcx", "r11", "memory", "cc");
                if (cf && rax == RAWSYS_ERESTART)
                        continue;
                r.rv = cf ? -1 : rax;
                r.rv2 = rdx;
                r.error = cf ? (int)rax : 0;
                return r;
        }
}

#define RAWSYS0(n)              rawsys6((n), 0, 0, 0, 0, 0, 0)
#define RAWSYS1(n, a)           rawsys6((n), (long)(a), 0, 0, 0, 0, 0)
#define RAWSYS2(n, a, b)        rawsys6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define RAWSYS3(n, a, b, c)     rawsys6((n), (long)(a), (long)(b), \
                                    (long)(c), 0, 0, 0)

/* The process: exit, exec, and the two forks; pids or -errors. */

static inline void
rawsys_exit(int status)
{
        for (;;)
                (void)RAWSYS1(SYS_exit, status);
}

static inline int
rawsys_execve(const char *path, char *const argv[], char *const envp[])
{
        return RAWSYS3(SYS_execve, path, argv, envp).error;
}

static inline pid_t
rawsys_fork_with(long n)
{
        struct rawsys r = RAWSYS0(n);

        if (r.error != 0)
                return -r.error;
        return r.rv2 != 0 ? 0 : (pid_t)r.rv;
}

static inline pid_t
fork1(void)
{
        return rawsys_fork_with(SYS_fork1);
}

static inline pid_t
forkall(void)
{
        return rawsys_fork_with(SYS_forkall);
}

/* The break: set it to addr; 0 or an error number. */
static inline int
rawsys_brk(void *addr)
{
        return RAWSYS1(SYS_brk, addr).error;
}

static inline void *
rawsys_mmap(void *addr, size_t len, int prot, int flags)
{
        struct rawsys r = rawsys6(SYS_mmap, (long)addr, (long)len, prot,
            flags, -1, 0);

        return r.error != 0 ? MAP_FAILED : (void *)r.rv;
}

static inline int
rawsys_munmap(void *addr, size_t len)
{
        return RAWSYS2(SYS_munmap, addr, len).error;
}

/* The LWP private pointer, the %fs base. */
static inline int
rawsys_lwp_private(void *p)
{
        return RAWSYS1(SYS_lwpprivate, p).error;
}

static inline int
rawsys_sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
        return RAWSYS3(SYS_sigprocmask, how, set, oset).error;
}

/*
 * The System V signal(): the handler is reset when the signal comes.
 * The library's sigaction registers its handler trampoline first.
 */
static inline void (*rawsys_signal(int sig, void (*fn)(int)))(int)
{
        struct sigaction sa;
        struct rawsys r;

        if (sigaction(sig, NULL, &sa) != 0)
                return SIG_ERR;
        r = RAWSYS2(SYS_signal, sig, fn);
        return r.error != 0 ? SIG_ERR : (void (*)(int))r.rv;
}

/*
 * sigwait for the signals in set, for at most ts (seconds and
 * nanoseconds; NULL: no limit): the signal, or -EAGAIN when the time
 * ran out, or another -error.
 */
static inline int
rawsys_sigtimedwait(const sigset_t *set, siginfo_t *info, const int64_t *ts)
{
        struct rawsys r = RAWSYS3(SYS_sigwait, set, info, ts);

        return r.error != 0 ? -r.error : (int)r.rv;
}

/* Contexts: rawsys_getcontext returns 0 at first and again on resume. */
int rawsys_getcontext(ucontext_t *);

static inline int
rawsys_setcontext(const ucontext_t *uc)
{
        return RAWSYS2(SYS_context, RAWSYS_SETCONTEXT, uc).error;
}

/*
 * The kernel saves the registers as at the call; the context is made to
 * resume as a return from rawsys_getcontext, with its caller's %rip and
 * %rsp and 0 in %rax, so that it stays good after this frame is gone.
 * 192, 216, and 104 are the offsets of gregs[REG_RIP], [REG_RSP], and
 * [REG_RAX], checked below.
 */
__asm__(
    "        .text\n"
    "        .globl  rawsys_getcontext\n"
    "        .type   rawsys_getcontext, @function\n"
    "rawsys_getcontext:\n"
    "        pushq   %rdi\n"
    "        movq    %rdi, %rsi\n"
    "        xorl    %edi, %edi\n"
    "        movl    $100, %eax\n"
    "        syscall\n"
    "        popq    %rdi\n"
    "        jc      1f\n"
    "        movq    (%rsp), %rax\n"
    "        movq    %rax, 192(%rdi)\n"
    "        leaq    8(%rsp), %rax\n"
    "        movq    %rax, 216(%rdi)\n"
    "        movq    $0, 104(%rdi)\n"
    "        xorl    %eax, %eax\n"
    "1:\n"
    "        ret\n"
    "        .size   rawsys_getcontext, . - rawsys_getcontext\n");

typedef char rawsys_rip_offset[offsetof(ucontext_t,
    uc_mcontext.gregs[REG_RIP]) == 192 ? 1 : -1];
typedef char rawsys_rsp_offset[offsetof(ucontext_t,
    uc_mcontext.gregs[REG_RSP]) == 216 ? 1 : -1];
typedef char rawsys_rax_offset[offsetof(ucontext_t,
    uc_mcontext.gregs[REG_RAX]) == 104 ? 1 : -1];

/* LWPs, with UnixWare's names: these return 0 or an error number. */

static inline void
_lwp_exit(void)
{
        for (;;)
                (void)RAWSYS0(SYS_lwpexit);
}

static inline void
rawsys_lwp_return(void)
{
        _lwp_exit();
}

/*
 * A context for a new LWP: start at fn(arg) on the stack [stack, stack
 * + size), with the psABI's entry alignment and a return address that
 * ends the LWP, the caller's mask and floating-point state, and no %fs
 * base (so no C library).
 */
static inline int
_lwp_makecontext(ucontext_t *uc, void (*fn)(void *), void *arg, void *stack,
    size_t size)
{
        uint64_t sp = ((uint64_t)(uintptr_t)stack + size) & ~15UL;
        int error;

        if ((error = rawsys_getcontext(uc)) != 0)
                return error;
        sp -= 8;
        *(uint64_t *)(uintptr_t)sp = (uint64_t)(uintptr_t)rawsys_lwp_return;
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)fn;
        uc->uc_mcontext.gregs[REG_RDI] = (greg_t)(uintptr_t)arg;
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)sp;
        uc->uc_mcontext.gregs[REG_FSBASE] = 0;
        uc->uc_flags = UC_ALL;
        return 0;
}

static inline int
_lwp_create(ucontext_t *uc, unsigned long flags, lwpid_t *idp)
{
        struct rawsys r = RAWSYS2(SYS_lwpcreate, uc, flags);

        if (r.error == 0 && idp != NULL)
                *idp = (lwpid_t)r.rv;
        return r.error;
}

static inline int
_lwp_wait(lwpid_t id, lwpid_t *departed)
{
        return RAWSYS2(SYS_lwpwait, id, departed).error;
}

static inline lwpid_t
_lwp_self(void)
{
        return (lwpid_t)RAWSYS0(SYS_lwpself).rv;
}

static inline int
_lwp_kill(lwpid_t id, int sig)
{
        return RAWSYS2(SYS_lwpkill, id, sig).error;
}

static inline int
_lwp_suspend(lwpid_t id)
{
        return RAWSYS1(SYS_lwpsuspend, id).error;
}

static inline int
_lwp_continue(lwpid_t id)
{
        return RAWSYS1(SYS_lwpcontinue, id).error;
}

static inline int
_lwp_sema_init(_lwp_sema_t *sp, int count)
{
        memset(sp, 0, sizeof(*sp));
        sp->sema_count = count;
        return 0;
}

static inline int
_lwp_sema_wait(_lwp_sema_t *sp)
{
        return RAWSYS1(SYS_lwp_sema_wait, sp).error;
}

static inline int
_lwp_sema_post(_lwp_sema_t *sp)
{
        return RAWSYS1(SYS_lwp_sema_post, sp).error;
}

static inline int
_lwp_sema_trywait(_lwp_sema_t *sp)
{
        return RAWSYS1(SYS_lwp_sema_trywait, sp).error;
}

/* Sleep while *addr == expected, for at most the relative time ts. */
static inline int
_lwp_addr_wait(volatile int32_t *addr, int32_t expected, const int64_t *ts)
{
        return RAWSYS3(SYS_lwp_addr_wait, addr, expected, ts).error;
}

/* Wake up to n waiters on addr (all if n <= 0); *woken says how many. */
static inline int
_lwp_addr_wake(volatile int32_t *addr, int n, int *woken)
{
        struct rawsys r = RAWSYS2(SYS_lwp_addr_wake, addr, n);

        if (r.error == 0 && woken != NULL)
                *woken = (int)r.rv;
        return r.error;
}

#endif /* _RAWSYS_H_ */
