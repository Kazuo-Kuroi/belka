/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Signals from the point of view of a program: handlers and siginfo,
 * masks and pending signals, the UnixWare delivery order, SA_RESETHAND,
 * SA_NODEFER, SA_RESTART and ERESTART, faults handled and repaired with
 * every register back, alarm and pause, sigsuspend, stop and continue
 * with wait, SIGCHLD,
 * alternate stacks, contexts, the floating-point state across a
 * handler, process groups, and the old signal interfaces.
 *
 * Exits with the number of failed checks.
 *
 * Built against mlibc; the System V signal(), contexts, and the raw
 * result of a call through rawsys.h.
 */

#define _GNU_SOURCE             /* REG_* for rawsys.h, MAP_ANON */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "rawsys.h"

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("sigtest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

static volatile int hits, last_sig, order[8], norder;
static volatile int info_code, info_pid, info_signo;
static volatile sig_atomic_t info_value;
static volatile unsigned long info_addr;
static volatile int masked_inside;

static int
catch(int sig, void (*fn)(int, siginfo_t *, void *), int flags)
{
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = fn;
        sa.sa_flags = flags | SA_SIGINFO;
        return sigaction(sig, &sa, NULL);
}

static int
set_default(int sig)
{
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        return sigaction(sig, &sa, NULL);
}

static void
on_sig(int sig, siginfo_t *si, void *uc)
{
        sigset_t cur;

        hits++;
        last_sig = sig;
        if (norder < 8)
                order[norder++] = sig;
        if (si != NULL) {
                info_signo = si->si_signo;
                info_code = si->si_code;
                info_pid = si->si_pid;
                info_addr = (unsigned long)si->si_addr;
                if (si->si_code == SI_QUEUE)
                        info_value = si->si_value.sival_int;
        }
        sigprocmask(SIG_BLOCK, NULL, &cur);
        masked_inside = sigismember(&cur, sig);
        (void)uc;
}

static void
reset_counts(void)
{
        hits = 0;
        norder = 0;
        last_sig = 0;
}

/* A handler runs before kill(2) of oneself returns, with siginfo. */
static void
test_basic(void)
{
        reset_counts();
        CHECK(catch(SIGUSR1, on_sig, 0) == 0);
        CHECK(kill(getpid(), SIGUSR1) == 0);
        CHECK(hits == 1 && last_sig == SIGUSR1);
        CHECK(info_signo == SIGUSR1 && info_code == SI_USER &&
            info_pid == getpid());
        CHECK(masked_inside == 1);              /* held while it runs */
        CHECK(catch(SIGUSR1, on_sig, SA_NODEFER) == 0);
        CHECK(kill(getpid(), SIGUSR1) == 0 && masked_inside == 0);
        CHECK(kill(getpid(), 0) == 0);
        CHECK(kill(30001, SIGUSR1) == -1 && errno == ESRCH);
        CHECK(kill(getpid(), 99) == -1 && errno == EINVAL);
        CHECK(catch(SIGKILL, on_sig, 0) == -1 && errno == EINVAL);
        CHECK(catch(SIGSTOP, on_sig, 0) == -1 && errno == EINVAL);
}

/* Held signals wait; released, the highest-numbered comes first. */
static void
test_mask_order(void)
{
        sigset_t set, pend, old;

        reset_counts();
        CHECK(catch(SIGUSR1, on_sig, 0) == 0);
        CHECK(catch(SIGUSR2, on_sig, 0) == 0);
        CHECK(catch(SIGHUP, on_sig, 0) == 0);
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        sigaddset(&set, SIGUSR2);
        sigaddset(&set, SIGHUP);
        CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0);
        CHECK(kill(getpid(), SIGHUP) == 0);
        CHECK(kill(getpid(), SIGUSR1) == 0);
        CHECK(kill(getpid(), SIGUSR2) == 0);
        CHECK(kill(getpid(), SIGUSR1) == 0);    /* not queued twice */
        CHECK(hits == 0);
        CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1) &&
            sigismember(&pend, SIGUSR2) && sigismember(&pend, SIGHUP));
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
        /* A system call to collect any left for the next return. */
        (void)getpid();
        (void)getpid();
        CHECK(hits == 3);
        CHECK(norder == 3 && order[0] == SIGUSR2 && order[1] == SIGUSR1 &&
            order[2] == SIGHUP);
        CHECK(sigpending(&pend) == 0 && !sigismember(&pend, SIGUSR1));
}

/*
 * SA_RESETHAND, and System V's signal(), reset the handler on delivery
 * (mlibc's signal() is POSIX's, which need not).
 */
static void
plain_handler(int sig)
{
        hits++;
        last_sig = sig;
}

static void
test_resethand(void)
{
        struct sigaction oa;

        reset_counts();
        CHECK(catch(SIGUSR1, on_sig, SA_RESETHAND) == 0);
        CHECK(kill(getpid(), SIGUSR1) == 0 && hits == 1);
        CHECK(sigaction(SIGUSR1, NULL, &oa) == 0 && oa.sa_handler == SIG_DFL);
        CHECK(rawsys_signal(SIGUSR2, plain_handler) != SIG_ERR);
        CHECK(kill(getpid(), SIGUSR2) == 0 && hits == 2);
        CHECK(rawsys_signal(SIGUSR2, SIG_IGN) == SIG_DFL);
        CHECK(kill(getpid(), SIGUSR2) == 0 && hits == 2);       /* ignored */
        CHECK(rawsys_signal(SIGUSR2, SIG_DFL) == SIG_IGN);
}

/*
 * A fault is a signal with the address; the handler maps the missing
 * page and returns, and the faulting instruction then succeeds.
 */
static void
on_segv(int sig, siginfo_t *si, void *uc)
{
        void *page = (void *)((unsigned long)si->si_addr & ~4095UL);

        hits++;
        last_sig = sig;
        info_code = si->si_code;
        info_addr = (unsigned long)si->si_addr;
        (void)uc;
        if (mmap(page, 4096, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != page)
                _exit(99);
}

static void
test_fault(void)
{
        volatile int *p = (volatile int *)0x50001234UL;

        reset_counts();
        CHECK(catch(SIGSEGV, on_segv, 0) == 0);
        *p = 7;
        CHECK(hits == 1 && last_sig == SIGSEGV && *p == 7);
        CHECK(info_code == SEGV_MAPERR && info_addr == 0x50001234UL);
        CHECK(munmap((void *)0x50001000UL, 4096) == 0);
        CHECK(set_default(SIGSEGV) == 0);
}

static void
on_ill(int sig, siginfo_t *si, void *ctx)
{
        ucontext_t *uc = ctx;

        (void)si;
        hits++;
        last_sig = sig;
        uc->uc_mcontext.gregs[REG_RIP] += 2;    /* past the ud2 */
}

/*
 * A handler's return gives the interrupted code every register back,
 * %rcx and %r11 too, though the return is a system call whose sysretq
 * would leave them holding %rip and %rflags.
 */
static void
test_fault_regs(void)
{
        uint64_t rcx, r11;

        reset_counts();
        CHECK(catch(SIGILL, on_ill, 0) == 0);
        __asm__ __volatile__(
            "movabsq $0x0123456789abcdef, %%rcx\n\t"
            "movabsq $0xfedcba9876543210, %%r11\n\t"
            "ud2\n\t"
            "movq %%rcx, %0\n\t"
            "movq %%r11, %1"
            : "=&r"(rcx), "=&r"(r11) : : "rcx", "r11", "memory");
        CHECK(hits == 1 && last_sig == SIGILL);
        CHECK(rcx == 0x0123456789abcdefULL);
        CHECK(r11 == 0xfedcba9876543210ULL);
        CHECK(set_default(SIGILL) == 0);
}

/* alarm and pause: pause returns EINTR once the handler has run. */
static void
test_alarm(void)
{
        time_t t0 = time(NULL);

        reset_counts();
        CHECK(catch(SIGALRM, on_sig, 0) == 0);
        CHECK(alarm(1) == 0);
        CHECK(pause() == -1 && errno == EINTR);
        CHECK(hits == 1 && last_sig == SIGALRM);
        CHECK(time(NULL) - t0 <= 3);
        CHECK(alarm(10) == 0);
        CHECK(alarm(0) >= 9);                   /* what was left */
}

/* sigsuspend takes a signal held otherwise, and the mask comes back. */
static void
test_sigsuspend(void)
{
        sigset_t set, empty, cur;
        pid_t pid;
        int status;

        reset_counts();
        CHECK(catch(SIGUSR1, on_sig, 0) == 0);
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
        if ((pid = fork()) == 0) {
                kill(getppid(), SIGUSR1);
                _exit(0);
        }
        sigemptyset(&empty);
        CHECK(sigsuspend(&empty) == -1 && errno == EINTR);
        CHECK(hits == 1 && last_sig == SIGUSR1);
        CHECK(sigprocmask(SIG_BLOCK, NULL, &cur) == 0 &&
            sigismember(&cur, SIGUSR1));
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(sigprocmask(SIG_UNBLOCK, &set, NULL) == 0);
}

/* Payloads reach both handlers and synchronous waits. */
static void
test_sigqueue(void)
{
        const struct timespec now = { 0, 0 }, invalid = { 0, 1000000000 };
        const struct timespec timeout = { 2, 0 };
        union sigval value = { .sival_int = -23 };
        sigset_t set, old, empty;
        siginfo_t info;
        int signo = 0, status;
        pid_t self = getpid(), child;

        reset_counts();
        CHECK(catch(SIGUSR1, on_sig, 0) == 0);
        CHECK(sigqueue(self, 0, value) == 0 && hits == 0);
        CHECK(sigqueue(0, SIGUSR1, value) == -1 && errno == ESRCH);
        CHECK(sigqueue(self, -1, value) == -1 && errno == EINVAL);
        CHECK(sigqueue(self, SIGUSR1, value) == 0);
        CHECK(hits == 1 && info_code == SI_QUEUE && info_pid == self &&
            info_value == -23);

        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &set, &old) != 0) {
                CHECK(0);
                return;
        }
        CHECK(sigtimedwait(&set, NULL, &invalid) == -1 && errno == EINVAL);
        CHECK(sigtimedwait(&set, NULL, &now) == -1 && errno == EAGAIN);
        value.sival_ptr = &signo;
        CHECK(sigqueue(self, SIGUSR1, value) == 0);
        CHECK(sigqueue(self, SIGUSR1, (union sigval){ .sival_int = 7 }) == 0);
        CHECK(sigtimedwait(&set, &info, &now) == SIGUSR1 &&
            info.si_signo == SIGUSR1 && info.si_code == SI_QUEUE &&
            info.si_pid == self && info.si_uid == getuid() &&
            info.si_value.sival_ptr == &signo);
        CHECK(sigtimedwait(&set, NULL, &now) == -1 && errno == EAGAIN);

        sigemptyset(&empty);
        errno = EDOM;
        CHECK(sigwait(&empty, &signo) == EINVAL && errno == EDOM);
        if (kill(self, SIGUSR1) == 0)
                CHECK(sigwait(&set, &signo) == 0 && signo == SIGUSR1);
        else
                CHECK(0);
        if (kill(self, SIGUSR1) == 0)
                CHECK(sigwaitinfo(&set, &info) == SIGUSR1 &&
                    info.si_code == SI_USER);
        else
                CHECK(0);

        child = fork();
        if (child == 0) {
                value.sival_int = 42;
                _exit(sigqueue(self, SIGUSR1, value) == 0 ? 0 : 1);
        }
        CHECK(child > 0);
        if (child > 0) {
                CHECK(sigtimedwait(&set, &info, &timeout) == SIGUSR1 &&
                    info.si_code == SI_QUEUE && info.si_pid == child &&
                    info.si_value.sival_int == 42);
                CHECK(waitpid(child, &status, 0) == child &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
}

/* Default actions on a child: terminate, and ignore. */
static void
test_defaults(void)
{
        pid_t pid;
        int status;

        if ((pid = fork()) == 0) {
                for (;;)
                        pause();
        }
        CHECK(kill(pid, SIGTERM) == 0);
        CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGTERM);

        if ((pid = fork()) == 0) {
                signal(SIGTERM, SIG_IGN);
                kill(getpid(), SIGTERM);
                kill(getpid(), SIGWINCH);       /* ignored by default */
                _exit(5);
        }
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 5);

        /* An unassigned system call raises SIGSYS. */
        if ((pid = fork()) == 0) {
                (void)RAWSYS1(250, 0);
                _exit(0);
        }
        CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGSYS);
}

static volatile pid_t victim;

static void
kill_victim(int sig)
{
        (void)sig;
        hits++;
        kill(victim, SIGKILL);
}

/*
 * A wait a signal interrupts: EINTR without SA_RESTART; with it, the
 * library sees ERESTART and waits on.
 */
static void
test_restart(void)
{
        struct sigaction sa;
        pid_t pid;
        int status;

        reset_counts();
        if ((victim = pid = fork()) == 0) {
                for (;;)
                        pause();
        }
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = plain_handler;
        CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
        alarm(1);
        CHECK(waitpid(pid, &status, 0) == -1 && errno == EINTR && hits == 1);

        sa.sa_handler = kill_victim;
        sa.sa_flags = SA_RESTART;
        CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
        alarm(1);
        {
                pid_t r = waitpid(pid, &status, 0);

                if (!(r == pid && WIFSIGNALED(status) &&
                    WTERMSIG(status) == SIGKILL && hits == 2))
                        printf("sigtest: restart: r %d errno %d status %x "
                            "hits %d\n", r, errno, status, hits);
                CHECK(r == pid && WIFSIGNALED(status) &&
                    WTERMSIG(status) == SIGKILL && hits == 2);
        }
}

/* Stop, continue, and kill a child, watching each through wait. */
static volatile int chld_code;

static void
on_chld(int sig, siginfo_t *si, void *uc)
{
        (void)sig;
        (void)uc;
        chld_code = si->si_code;
}

static void
test_stop(void)
{
        pid_t pid;
        int status;

        CHECK(catch(SIGCHLD, on_chld, 0) == 0);
        if ((pid = fork()) == 0) {
                for (;;)
                        pause();
        }
        CHECK(kill(pid, SIGSTOP) == 0);
        CHECK(waitpid(pid, &status, WSTOPPED) == pid && WIFSTOPPED(status) &&
            WSTOPSIG(status) == SIGSTOP);
        CHECK(chld_code == CLD_STOPPED);
        CHECK(waitpid(pid, &status, WSTOPPED | WNOHANG) == 0);
        CHECK(kill(pid, SIGCONT) == 0);
        CHECK(waitpid(pid, &status, WCONTINUED) == pid &&
            WIFCONTINUED(status));
        /* A stopped child still dies of SIGKILL. */
        CHECK(kill(pid, SIGTSTP) == 0);
        CHECK(waitpid(pid, &status, WSTOPPED) == pid && WIFSTOPPED(status) &&
            WSTOPSIG(status) == SIGTSTP);
        CHECK(kill(pid, SIGKILL) == 0);
        CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGKILL);
        CHECK(chld_code == CLD_KILLED);
        CHECK(set_default(SIGCHLD) == 0);

        /* With SIGCHLD ignored, children leave no zombies. */
        CHECK(signal(SIGCHLD, SIG_IGN) != SIG_ERR);
        if ((pid = fork()) == 0)
                _exit(1);
        CHECK(waitpid(pid, &status, 0) == -1 && errno == ECHILD);
        CHECK(signal(SIGCHLD, SIG_DFL) == SIG_IGN);
}

/* A handler on the alternate stack. */
static char altstack[SIGSTKSZ];
static volatile unsigned long handler_sp;
static volatile int handler_onstack;

static void
on_alt(int sig, siginfo_t *si, void *ucv)
{
        stack_t cur;
        int local;

        (void)sig;
        (void)si;
        (void)ucv;
        handler_sp = (unsigned long)&local;
        handler_onstack = sigaltstack(NULL, &cur) == 0 &&
            (cur.ss_flags & SS_ONSTACK) != 0;
}

static void
test_altstack(void)
{
        stack_t ss, old;

        ss.ss_sp = altstack;
        ss.ss_size = sizeof(altstack);
        ss.ss_flags = 0;
        CHECK(sigaltstack(&ss, &old) == 0 && (old.ss_flags & SS_DISABLE));
        CHECK(catch(SIGUSR1, on_alt, SA_ONSTACK) == 0);
        CHECK(kill(getpid(), SIGUSR1) == 0);
        CHECK(handler_sp > (unsigned long)altstack &&
            handler_sp < (unsigned long)altstack + sizeof(altstack));
        CHECK(handler_onstack);
        ss.ss_size = 100;
        CHECK(sigaltstack(&ss, NULL) == -1 && errno == ENOMEM);
        ss.ss_flags = SS_DISABLE;
        CHECK(sigaltstack(&ss, NULL) == 0);
}

/* getcontext and setcontext, and a context the kernel refuses. */
static void
test_context(void)
{
        static ucontext_t uc, bad;
        static volatile int passes;

        passes = 0;
        CHECK(rawsys_getcontext(&uc) == 0);
        if (++passes < 3)
                rawsys_setcontext(&uc);
        CHECK(passes == 3);
        CHECK(rawsys_getcontext(&bad) == 0);
        if (passes == 3) {
                passes++;
                bad.uc_mcontext.gregs[REG_RIP] = (greg_t)0xffff800000000000UL;
                CHECK(rawsys_setcontext(&bad) == EINVAL);
                bad.uc_mcontext.gregs[REG_RIP] = 0;
                bad.uc_mcontext.gregs[REG_FSBASE] =
                    (greg_t)0x8000000000000000UL;
                CHECK(rawsys_setcontext(&bad) == EINVAL);
        }
}

/* The interrupted code's vector registers survive a handler. */
static void
clobber_fp(int sig)
{
        volatile double x = 3.0;

        (void)sig;
        __asm__ __volatile__("pcmpeqd %%xmm7, %%xmm7\n\t"
            "xorpd %%xmm6, %%xmm6" : : : "xmm6", "xmm7");
        x = x * x;
        hits++;
}

/*
 * Many times over, since the state a handler's return restores can be
 * lost only if the LWP is switched out at the wrong moment.
 */
static void
test_fpu(void)
{
        uint64_t before[2] = { 0x0123456789abcdefULL, 0xfedcba9876543210ULL };
        uint64_t after[2];
        int lost = 0;

        reset_counts();
        for (int i = 0; i < 500; i++) {
                /* signal() resets the handler at each delivery. */
                if (rawsys_signal(SIGUSR2, clobber_fp) == SIG_ERR)
                        lost++;
                after[0] = after[1] = 0;
                __asm__ __volatile__(
                    "movdqu %1, %%xmm7\n\t"
                    "movq $37, %%rax\n\t"               /* SYS_kill */
                    "movq %2, %%rdi\n\t"
                    "movq $17, %%rsi\n\t"               /* SIGUSR2 */
                    "syscall\n\t"
                    "movdqu %%xmm7, %0"
                    : "=m"(after) : "m"(before), "r"((long)getpid())
                    : "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10",
                      "r11", "memory", "xmm7", "cc");
                if (after[0] != before[0] || after[1] != before[1])
                        lost++;
        }
        CHECK(hits == 500);
        CHECK(lost == 0);
}

/* Process groups: a child in a group of its own, killed as a group. */
static void
test_pgrp(void)
{
        pid_t pid;
        int status;

        CHECK(getpgrp() == getpid());           /* the kernel's session */
        CHECK(setsid() == -1 && errno == EPERM);
        if ((pid = fork()) == 0) {
                for (;;)
                        pause();
        }
        CHECK(setpgid(pid, pid) == 0);
        CHECK(kill(-pid, SIGTERM) == 0);
        CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGTERM);
        CHECK(kill(-pid, SIGTERM) == -1 && errno == ESRCH);
}

/*
 * Ping-pong: parent and child send SIGUSR1 to each other, each waiting
 * in sigsuspend with it otherwise held. Every round races the sender
 * against the receiver falling asleep; a lost wakeup hangs both.
 */
#define PINGPONG        500

static volatile int pongs;

static void
on_ping(int sig)
{
        (void)sig;
        pongs++;
}

static void
test_pingpong(void)
{
        sigset_t set, empty;
        pid_t pid, parent = getpid();
        struct sigaction sa;
        int status;

        pongs = 0;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_ping;
        CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        sigemptyset(&empty);
        CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
        if ((pid = fork()) == 0) {
                for (int i = 0; i < PINGPONG; i++) {
                        while (pongs <= i)
                                sigsuspend(&empty);
                        kill(parent, SIGUSR1);
                }
                _exit(pongs == PINGPONG ? 0 : 1);
        }
        for (int i = 0; i < PINGPONG; i++) {
                kill(pid, SIGUSR1);
                while (pongs <= i)
                        sigsuspend(&empty);
        }
        CHECK(pongs == PINGPONG);
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0);
        CHECK(sigprocmask(SIG_UNBLOCK, &set, NULL) == 0);
        CHECK(set_default(SIGUSR1) == 0);
}

int
main(int argc, char **argv)
{
        setvbuf(stdout, NULL, _IONBF, 0);
        /* "sigtest pause": wait for a signal from the kernel. */
        if (argc == 2 && strcmp(argv[1], "pause") == 0) {
                for (;;)
                        pause();
        }
        test_basic();
        test_mask_order();
        test_resethand();
        test_fault();
        test_fault_regs();
        test_alarm();
        test_sigsuspend();
        test_sigqueue();
        test_defaults();
        test_restart();
        test_stop();
        test_altstack();
        test_context();
        test_fpu();
        test_pgrp();
        test_pingpong();
        printf("sigtest: pid %d: %d failures\n", getpid(), failures);
        return failures;
}
