/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-20
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rawsys.h"

static int failures;
static volatile int value;
static volatile sig_atomic_t notified;
static const char *root;
static __thread int private_value = 7;
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int libc_errors;
static int counter;
static pid_t creator;
static int *creator_errno;
extern char end[];

#define CHECK(e) do { \
        if (!(e)) { \
                printf("sproctest: %d: %s\n", __LINE__, #e); \
                failures++; \
        } \
} while (0)

struct request {
        int mode;
        int fd;
        pid_t parent;
        unsigned int mask;
        size_t len;
        char *program;
        char *directory;
};

static void
raw_exit(int code)
{
        (void)rawsys6(SYS_exit, code, 0, 0, 0, 0, 0);
        for (;;)
                ;
}

static void
raw_child(void *opaque, size_t len)
{
        struct request *q = opaque;
        struct rawsys r;
        int bad = 0;

        r = rawsys6(SYS_prctl, PR_GETSHMASK, 0, 0, 0, 0, 0);
        bad += r.error != 0 || (unsigned int)r.rv != q->mask;
        r = rawsys6(SYS_getpid, 0, 0, 0, 0, 0, 0);
        bad += r.rv == q->parent || r.rv2 != q->parent;
        bad += q->len != (size_t)-1 && q->len != len;
        value = 42;
        if (q->mode == 1) {
                struct rlimit lim = { 131072, RLIM_INFINITY };

                bad += rawsys6(SYS_close, q->fd, 0, 0, 0, 0, 0).error != 0;
                bad += rawsys6(SYS_umask, 027, 0, 0, 0, 0, 0).error != 0;
                bad += rawsys6(SYS_chdir, (long)q->directory, 0,
                    0, 0, 0, 0).error != 0;
                bad += rawsys6(SYS_setrlimit, RLIMIT_FSIZE, (long)&lim,
                    0, 0, 0, 0).error != 0;
                bad += rawsys6(SYS_setegid, 1234, 0, 0, 0, 0, 0).error != 0;
                bad += rawsys6(SYS_seteuid, 1234, 0, 0, 0, 0, 0).error != 0;
        } else if (q->mode == 2) {
                r = rawsys6(SYS_prctl, PR_ISBLOCKED, q->parent, 0, 0, 0, 0);
                bad += r.error != 0 || r.rv != 1;
                bad += rawsys6(SYS_procblk, 1, q->parent,
                    0, 0, 0, 0).error != 0;
        } else if (q->mode == 3) {
                pid_t self = (pid_t)r.rv;

                value = 1;
                bad += rawsys6(SYS_procblk, 2, self, -1, 0, 0, 0).error != 0;
                value = 2;
        } else if (q->mode == 4) {
                struct request grand = *q;
                pid_t self = (pid_t)r.rv;

                grand.mode = 0;
                grand.parent = self;
                grand.mask = PR_SPROC | PR_NOLIBC;
                value = 5;
                r = rawsys6(SYS_sproc, (long)raw_child, PR_SALL | PR_NOLIBC,
                    (long)&grand, 0, 0, 0);
                if (r.error != 0) {
                        bad++;
                } else {
                        r = rawsys6(SYS_wait, 0, 0, 0, 0, 0, 0);
                        bad += r.error != 0 || r.rv2 != 37 * 256;
                }
                bad += value != 5;
                bad += rawsys6(SYS_close, q->fd, 0, 0, 0, 0, 0).error != 0;
        } else if (q->mode == 5) {
                char *av[] = { q->program, NULL };
                char *ev[] = { NULL };

                bad += rawsys6(SYS_prctl, PR_UNBLKONEXEC, q->parent,
                    0, 0, 0, 0).error != 0;
                if (bad == 0)
                        (void)rawsys6(SYS_execve, (long)q->program,
                            (long)av, (long)ev, 0, 0, 0);
                raw_exit(99);
        } else if (q->mode == 6) {
                value = 1;
                for (;;)
                        (void)rawsys6(SYS_yield, 0, 0, 0, 0, 0, 0);
        } else if (q->mode == 7) {
                char *heap = (char *)(((uintptr_t)end + 4095) & ~(uintptr_t)4095);

                r = rawsys6(SYS_brk, (long)(heap + 8192), 0, 0, 0, 0, 0);
                bad += r.error != 0;
                if (r.error == 0)
                        heap[4096] = 0x5a;
        }
        raw_exit(bad == 0 ? 37 : 99);
}

static struct rawsys
spawn(struct request *q, unsigned int mask, void *top, size_t len, int supplied)
{
        q->parent = getpid();
        q->mask = (mask & (PR_SALL | PR_BLOCK | PR_NOLIBC)) | PR_SPROC;
        q->len = supplied ? len : (size_t)-1;
        return rawsys6(supplied ? SYS_sprocsp : SYS_sproc,
            (long)raw_child, mask, (long)q, (long)top, (long)len, 0);
}

static void
reap(struct rawsys r, int code, int signal)
{
        int status;

        CHECK(r.error == 0 && r.rv > 0 && r.rv2 == 0);
        if (r.error != 0)
                return;
        CHECK(waitpid((pid_t)r.rv, &status, 0) == r.rv);
        if (signal != 0)
                CHECK(WIFSIGNALED(status) && WTERMSIG(status) == signal);
        else
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == code);
}

static void
notice(int signal)
{
        notified = signal;
}

struct fp_request {
        uint16_t control;
        uint32_t mxcsr;
};

static void
fp_child(void *opaque, size_t len)
{
        unsigned char state[512] __attribute__((aligned(16)));
        struct fp_request *q = opaque;
        uint16_t control;
        uint32_t mxcsr;
        int bad = 0;

        __asm__ __volatile__("fxsave %0" : "=m"(state));
        (void)len;
        control = state[0] | (uint16_t)state[1] << 8;
        mxcsr = state[24] | (uint32_t)state[25] << 8 |
            (uint32_t)state[26] << 16 | (uint32_t)state[27] << 24;
        bad += control != q->control || mxcsr != q->mxcsr;
        bad += state[4] != 0;    /* No occupied x87 registers. */
        for (int i = 160; i < 416; i++)
                bad += state[i] != 0;
        raw_exit(bad != 0 ? 99 : 37);
}

static void
test_fp(void)
{
        unsigned char saved[512] __attribute__((aligned(16)));
        struct fp_request q;
        struct rawsys r;

        __asm__ __volatile__("fxsave %0" : "=m"(saved));
        q.control = (saved[0] | (uint16_t)saved[1] << 8) ^ 0x400;
        q.mxcsr = (saved[24] | (uint32_t)saved[25] << 8 |
            (uint32_t)saved[26] << 16 | (uint32_t)saved[27] << 24) ^ 0x2000;
        __asm__ __volatile__("fldcw %0; ldmxcsr %1; fld1; pcmpeqd %%xmm0, %%xmm0"
            : : "m"(q.control), "m"(q.mxcsr) : "st", "xmm0");
        r = rawsys6(SYS_sproc, (long)fp_child, PR_SADDR | PR_NOLIBC,
            (long)&q, 0, 0, 0);
        __asm__ __volatile__("fxrstor %0" : : "m"(saved) : "memory");
        reap(r, 37, 0);
}

static void
test_creation(void)
{
        struct request q = { 0 };
        struct rawsys r;
        void *stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);

        CHECK(stack != MAP_FAILED);
        value = 0;
        reap(spawn(&q, PR_SADDR | PR_NOLIBC | PR_EVENT | 0x1000,
            NULL, 0, 0), 37, 0);
        CHECK(value == 42);
        CHECK(rawsys6(SYS_prctl, PR_GETNSHARE, 0, 0, 0, 0, 0).rv == 1);
        r = rawsys6(SYS_sprocsp, (long)raw_child, PR_NOLIBC,
            (long)&q, 0, 8191, 0);
        CHECK(r.error == EINVAL);
        if (stack != MAP_FAILED) {
                reap(spawn(&q, PR_SADDR | PR_NOLIBC,
                    (char *)stack + 65536, 0, 1), 37, 0);
                *(volatile char *)stack = 7;
                CHECK(*(volatile char *)stack == 7);
                CHECK(munmap(stack, 65536) == 0);
        }
        r = rawsys6(SYS_sproc, -1, PR_SADDR | PR_NOLIBC, 0, 0, 0, 0);
        reap(r, 0, SIGSEGV);
        r = rawsys6(SYS_sprocsp, (long)raw_child, PR_SADDR | PR_NOLIBC,
            (long)&q, 1, 0, 0);
        reap(r, 0, SIGSEGV);
}

static void
test_resources(void)
{
        struct request q = { 0 };
        struct rlimit old, now;
        char directory[256], expected[256];
        mode_t oldmask = umask(022);
        int fd = open("/p/bin/hello", O_RDONLY);

        CHECK(fd >= 0);
        CHECK(getrlimit(RLIMIT_FSIZE, &old) == 0);
        snprintf(expected, sizeof(expected), "%s/dir", root);
        q.fd = fd;
        q.mode = 1;
        q.directory = expected;
        reap(spawn(&q, PR_SALL | PR_NOLIBC, NULL, 0, 0), 37, 0);
        CHECK(geteuid() == 1234 && getegid() == 1234);
        CHECK(seteuid(0) == 0);
        CHECK(setegid(0) == 0);
        CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        CHECK(umask(oldmask) == 027);
        CHECK(getrlimit(RLIMIT_FSIZE, &now) == 0 && now.rlim_cur == 131072);
        CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
        CHECK(getcwd(directory, sizeof(directory)) != NULL);
        CHECK(strcmp(directory, expected) == 0);
        CHECK(chdir(root) == 0);

        fd = open("/p/bin/hello", O_RDONLY);
        q.fd = fd;
        q.mode = 4;
        value = 0;
        reap(spawn(&q, PR_NOLIBC, NULL, 0, 0), 37, 0);
        CHECK(value == 0);
        CHECK(fcntl(fd, F_GETFD) >= 0);
        CHECK(close(fd) == 0);
}

static void
libc_child(void *arg)
{
        int bad = 0;
        char *memory;

        bad += arg != (void *)&counter;
        bad += private_value != 7 || &errno == creator_errno;
        bad += getpid() == creator || getppid() != creator;
        private_value = 99;
        errno = 0;
        bad += open("/missing-sproc-file", O_RDONLY) != -1 || errno != ENOENT;
        memory = malloc(4096);
        if (memory != NULL) {
                memset(memory, 0x5a, 4096);
                bad += memory[4095] != 0x5a;
                free(memory);
        } else {
                bad++;
        }
        for (int i = 0; i < 100; i++) {
                if (pthread_mutex_lock(&counter_lock) != 0) {
                        bad++;
                        break;
                }
                counter++;
                (void)rawsys6(SYS_yield, 0, 0, 0, 0, 0, 0);
                bad += pthread_mutex_unlock(&counter_lock) != 0;
        }
        if (bad != 0)
                libc_errors = 1;
}

static void
libc_stack_child(void *arg, size_t len)
{
        if (len != 65536)
                libc_errors = 1;
        libc_child(arg);
}

static void *
plain_thread(void *arg)
{
        return arg;
}

static void
libc_waiter(void *arg)
{
        *(volatile int *)arg = 1;
        for (;;)
                pause();
}

static void
test_libc(void)
{
        enum { MEMBERS = 4 };
        pid_t pids[MEMBERS], pid;
        pthread_t thread;
        int status;
        void *stack;

        creator = getpid();
        creator_errno = &errno;
        private_value = 11;
        libc_errors = 0;
        counter = 0;
        for (int i = 0; i < MEMBERS; i++) {
                pids[i] = sproc(libc_child, PR_SADDR, (void *)&counter);
                CHECK(pids[i] > 0);
        }
        for (int i = 0; i < MEMBERS; i++)
                if (pids[i] > 0)
                        CHECK(waitpid(pids[i], &status, 0) == pids[i] &&
                            WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(!libc_errors && counter == MEMBERS * 100 && private_value == 11);
        CHECK(pthread_create(&thread, NULL, plain_thread, NULL) == EPERM);
        stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(stack != MAP_FAILED);
        if (stack != MAP_FAILED) {
                pid = sprocsp(libc_stack_child, PR_SADDR, (void *)&counter,
                    (char *)stack + 65536, 65536);
                CHECK(pid > 0);
                if (pid > 0)
                        CHECK(waitpid(pid, &status, 0) == pid &&
                            WIFEXITED(status) && WEXITSTATUS(status) == 0);
                CHECK(munmap(stack, 65536) == 0);
                CHECK(!libc_errors && counter == (MEMBERS + 1) * 100);
        }
        errno = E2BIG;
        pid = sproc(libc_child, 0, (void *)&counter);
        CHECK(pid > 0);
        if (pid > 0)
                CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0);
        CHECK(counter == (MEMBERS + 1) * 100 && private_value == 11);
        CHECK(errno == E2BIG);
        value = 0;
        pid = sproc(libc_waiter, PR_SADDR, (void *)&value);
        CHECK(pid > 0);
        if (pid > 0) {
                for (int i = 0; value != 1 && i < 100000; i++)
                        (void)rawsys6(SYS_yield, 0, 0, 0, 0, 0, 0);
                CHECK(value == 1);
                CHECK(kill(pid, SIGKILL) == 0);
                CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
                    WTERMSIG(status) == SIGKILL);
        }
        CHECK(prctl(PR_GETNSHARE) == 1);
        CHECK((prctl(PR_GETSHMASK, 0) & PR_SALL) == PR_SALL);
        CHECK(prctl(PR_INIT_THREADS) == -1 && errno == EINVAL);
        CHECK(unblockproc(getpid()) == 0);
        CHECK(blockproc(getpid()) == 0);
}

struct survivor {
        pid_t parent;
        int fd;
        volatile int marker;
};

static void
survivor_child(void *opaque, size_t len)
{
        struct survivor *q = opaque;
        unsigned char result = 0;

        (void)len;
        for (int i = 0; i < 100000; i++) {
                struct rawsys r = rawsys6(SYS_getpid, 0, 0, 0, 0, 0, 0);

                if (r.rv2 != q->parent) {
                        result = q->marker == 0x5137 ? 0x5a : 0;
                        break;
                }
                (void)rawsys6(SYS_yield, 0, 0, 0, 0, 0, 0);
        }
        (void)rawsys6(SYS_write, q->fd, (long)&result, 1, 0, 0, 0);
        raw_exit(0);
}

static void
test_creator_exit(void)
{
        int fds[2], status;
        pid_t pid;
        unsigned char result = 0;

        CHECK(pipe(fds) == 0);
        pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
                struct survivor q = { getpid(), fds[1], 0x5137 };
                struct rawsys child = rawsys6(SYS_sproc, (long)survivor_child,
                    PR_SADDR | PR_NOLIBC, (long)&q, 0, 0, 0);

                raw_exit(child.error == 0 ? 13 : 99);
        }
        CHECK(close(fds[1]) == 0);
        if (pid > 0) {
                CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 13);
                CHECK(read(fds[0], &result, 1) == 1 && result == 0x5a);
                CHECK(read(fds[0], &result, 1) == 0);
        }
        CHECK(close(fds[0]) == 0);
}

static void
test_control(void)
{
        struct request q = { 0 };
        struct rawsys r;
        struct sigaction sa = { 0 };
        pid_t pid;
        int status;

        q.mode = 2;
        value = 0;
        r = spawn(&q, PR_SADDR | PR_BLOCK | PR_NOLIBC, NULL, 0, 0);
        CHECK(value == 42);
        reap(r, 37, 0);
        CHECK(rawsys6(SYS_procblk, 6, getpid(), 0, 0, 0, 0).error == EINVAL);
        CHECK(rawsys6(SYS_procblk, 2, getpid(), PR_MAXBLOCKCNT + 1,
            0, 0, 0).error == EINVAL);
        CHECK(rawsys6(SYS_procblk, 1, 0, 0, 0, 0, 0).error == ESRCH);
        q.mode = 3;
        value = 0;
        r = spawn(&q, PR_SADDR | PR_NOLIBC, NULL, 0, 0);
        CHECK(r.error == 0);
        if (r.error == 0) {
                int blocked = 0;

                for (int i = 0; i < 100000; i++) {
                        struct rawsys check = rawsys6(SYS_prctl,
                            PR_ISBLOCKED, r.rv, 0, 0, 0, 0);

                        if (check.rv == 1) {
                                blocked = 1;
                                break;
                        }
                        (void)rawsys6(SYS_yield, 0, 0, 0, 0, 0, 0);
                }
                CHECK(blocked && value == 1);
                CHECK(rawsys6(SYS_procblk, 1, r.rv, 0, 0, 0, 0).error == 0);
                reap(r, 37, 0);
                CHECK(value == 2);
        }
        pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
                int bad = 0;
                pthread_t thread;

                bad += rawsys6(SYS_prctl, PR_GETNSHARE, 0, 0, 0, 0, 0).rv != 0;
                bad += pthread_create(&thread, NULL, plain_thread, NULL) != 0;
                if (bad == 0)
                        bad += pthread_join(thread, NULL) != 0;
                bad += rawsys6(SYS_prctl, PR_INIT_THREADS, 0, 0, 0, 0, 0).error != 0;
                bad += rawsys6(SYS_sproc, (long)raw_child,
                    PR_NOLIBC, 0, 0, 0, 0).error != EPERM;
                bad += rawsys6(SYS_procblk, 0, getpid(), 0, 0, 0, 0).error != EPERM;
                _exit(bad != 0);
        }
        if (pid > 0)
                CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0);
        CHECK(rawsys6(SYS_prctl, PR_INIT_THREADS, 0, 0, 0, 0, 0).error == EINVAL);
        sa.sa_handler = notice;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
        CHECK(rawsys6(SYS_prctl, PR_SETEXITSIG, SIGUSR1, 0, 0, 0, 0).error == 0);
        notified = 0;
        q.mode = 0;
        reap(spawn(&q, PR_SADDR | PR_NOLIBC, NULL, 0, 0), 37, 0);
        CHECK(notified == SIGUSR1);
        CHECK(rawsys6(SYS_prctl, PR_SETEXITSIG, 0, 0, 0, 0, 0).error == 0);
        CHECK(prctl(PR_SETABORTSIG, SIGUSR1) == 0);
        notified = 0;
        reap(spawn(&q, PR_SADDR | PR_NOLIBC, NULL, 0, 0), 37, 0);
        CHECK(notified == 0);
        q.mode = 6;
        value = 0;
        r = spawn(&q, PR_SADDR | PR_NOLIBC, NULL, 0, 0);
        if (r.error == 0) {
                for (int i = 0; value != 1 && i < 100000; i++)
                        (void)rawsys6(SYS_yield, 0, 0, 0, 0, 0, 0);
                CHECK(value == 1);
                CHECK(kill((pid_t)r.rv, SIGKILL) == 0);
        }
        reap(r, 0, SIGKILL);
        CHECK(notified == SIGUSR1);
        CHECK(prctl(PR_SETABORTSIG, 0) == 0);
        CHECK(setblockproccntall(getpid(), 2) == 0);
        CHECK(blockprocall(getpid()) == 0);
        CHECK(blockprocall(getpid()) == 0);
        CHECK(unblockprocall(getpid()) == 0);
        CHECK(blockprocall(getpid()) == 0);
        pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
                pid_t parent = getppid();
                int bad = setuid(1234) != 0;

                bad += blockproc(parent) != -1 || errno != EPERM;
                _exit(bad != 0);
        }
        if (pid > 0)
                CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0);
        q.mode = 5;
        q.program = "/p/bin/hello";
        q.fd = open(q.program, O_RDONLY | O_CLOEXEC);
        CHECK(q.fd >= 0);
        reap(spawn(&q, PR_SADDR | PR_SFDS | PR_BLOCK | PR_NOLIBC,
            NULL, 0, 0), 0, 0);
        CHECK(fcntl(q.fd, F_GETFD) >= 0);
        CHECK(close(q.fd) == 0);
}

int
main(int argc, char **argv)
{
        root = argc > 1 ? argv[1] : "/p";
        puts("sproctest: creation and stacks");
        test_creation();
        puts("sproctest: floating-point controls and initial registers");
        test_fp();
        puts("sproctest: resource sharing");
        test_resources();
        {
                struct request q = { 0 };
                char *heap = (char *)(((uintptr_t)end + 4095) & ~(uintptr_t)4095);

                q.mode = 7;
                reap(spawn(&q, PR_SADDR | PR_NOLIBC, NULL, 0, 0), 37, 0);
                CHECK(rawsys_brk(heap + 12288) == 0);
                CHECK(heap[4096] == 0x5a);
                CHECK(rawsys_brk(heap) == 0);
        }
        puts("sproctest: blocking, signals and exec");
        test_control();
        puts("sproctest: libc, TLS and concurrent locks");
        test_libc();
        puts("sproctest: creator exit and original stack lifetime");
        test_creator_exit();
        printf("sproctest: %d failures\n", failures);
        return failures != 0;
}
