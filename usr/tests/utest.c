/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The user-mode test suite: system calls, fork and copy on write,
 * exec, wait, faults, files, memory mapping, the floating-point state,
 * and the LWP private pointer, from the point of view of a program.
 *
 * usage: utest dir, where dir holds bin/ with the other test programs
 * and is writable. Exits with the number of failed checks.
 *
 * Built against mlibc; the kernel's own interfaces (the raw result of a
 * call, the break, the old wait, the private pointer) through rawsys.h.
 */

#define _GNU_SOURCE             /* REG_* for rawsys.h, MAP_ANON */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rawsys.h"

static int failures;
static const char *dir;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("utest: %d: %s\n", __LINE__, #expr);     \
                        failures++;                                     \
                }                                                       \
        } while (0)

static char *
path(const char *rel)
{
        static char buf[4][256];
        static int next;
        char *p = buf[next++ % 4];

        snprintf(p, 256, "%s%s", dir, rel);
        return p;
}

/* Run fn in a child; return its wait status. */
static int
in_child(void (*fn)(void))
{
        int status = -1;
        pid_t pid = fork();

        if (pid == 0) {
                fn();
                _exit(0);
        }
        if (pid < 0 || waitpid(pid, &status, 0) != pid)
                return -1;
        return status;
}

static void
test_basics(void)
{
        struct utsname u;
        struct rawsys r;

        CHECK(getpid() > 0);
        CHECK(getppid() == 0);          /* started by the kernel */
        CHECK(write(1, "utest: start\n", 13) == 13);
        CHECK(uname(&u) == 0 && strcmp(u.sysname, "Belka") == 0 &&
            strcmp(u.machine, "amd64") == 0);
        CHECK(time(NULL) > 1700000000L);

        /*
         * Errors come back with the carry flag set and errno in %rax. An
         * unassigned number also raises SIGSYS, ignored here.
         */
        CHECK(signal(SIGSYS, SIG_IGN) != SIG_ERR);
        r = RAWSYS1(250, 0);
        CHECK(r.error == EINVAL);
        CHECK(signal(SIGSYS, SIG_DFL) == SIG_IGN);
        r = RAWSYS1(SYS_close, 99);
        CHECK(r.error == EBADF);
        r = RAWSYS0(SYS_getpid);
        CHECK(r.error == 0 && r.rv == getpid());

        /* Bad pointers are EFAULT, user or kernel. */
        CHECK(write(1, (void *)16, 4) == -1 && errno == EFAULT);
        CHECK(stat(path("/bin/utest"),
            (struct stat *)0xffff800000001000UL) == -1 && errno == EFAULT);
        CHECK(open((const char *)0x7ffffffff000UL, O_RDONLY, 0) == -1 &&
            errno == EFAULT);
        CHECK(open(path("/no/such/file"), O_RDONLY, 0) == -1 &&
            errno == ENOENT);
}

static char bss_block[100000];
static int global = 1;

/* The break, moved with the call itself (mlibc's heap is mapped). */
extern char end[];
static char *curbrk;

static int
brk_to(void *addr)
{
        int error = rawsys_brk(addr);

        if (error != 0) {
                errno = error;
                return -1;
        }
        curbrk = addr;
        return 0;
}

static void *
sbrk_by(long incr)
{
        char *old;

        if (curbrk == NULL)
                curbrk = end;
        old = curbrk;
        if (incr != 0 && brk_to(old + incr) < 0)
                return (void *)-1;
        return old;
}

static void
test_memory(void)
{
        char *base = sbrk_by(0), *p;
        unsigned char *m;
        int ok = 1;

        /* bss is zero and writable. */
        for (size_t i = 0; i < sizeof(bss_block); i++)
                ok &= bss_block[i] == 0;
        CHECK(ok);
        memset(bss_block, 0x5a, sizeof(bss_block));

        /* brk grows and shrinks the heap. */
        CHECK(sbrk_by(1 << 20) == base);
        p = base;
        for (int i = 0; i < (1 << 20); i += 4096)
                p[i] = (char)(i >> 12);
        ok = 1;
        for (int i = 0; i < (1 << 20); i += 4096)
                ok &= p[i] == (char)(i >> 12);
        CHECK(ok);
        CHECK(brk_to(base + 4096) == 0);
        CHECK(brk_to(base - 4096 * 1024) == -1 && errno == ENOMEM);
        CHECK(sbrk_by(8192) == base + 4096);

        /* Anonymous mmap, munmap, and mapping over a fixed address. */
        m = mmap(NULL, 256 * 1024, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(m != MAP_FAILED);
        if (m != MAP_FAILED) {
                for (int i = 0; i < 256 * 1024; i++)
                        m[i] = (unsigned char)i;
                ok = 1;
                for (int i = 0; i < 256 * 1024; i++)
                        ok &= m[i] == (unsigned char)i;
                CHECK(ok);
                /* A hole in the middle, then a fresh page in it. */
                CHECK(munmap(m + 65536, 4096) == 0);
                CHECK(mmap(m + 65536, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == m + 65536);
                CHECK(m[65536] == 0 && m[65535] == (unsigned char)65535 &&
                    m[65536 + 4096] == (unsigned char)(65536 + 4096));
                CHECK(munmap(m, 256 * 1024) == 0);
        }
        CHECK(mmap(NULL, 4096, PROT_READ, MAP_SHARED | MAP_PRIVATE | MAP_ANON,
            -1, 0) == MAP_FAILED && errno == EINVAL);
}

static int child_flag;

static void
cow_child(void)
{
        int local = 10;

        global = 2;
        child_flag = 99;
        local++;
        memset(bss_block, 0x11, 4096);
        _exit(global == 2 && local == 11 && bss_block[0] == 0x11 ? 42 : 1);
}

static void
test_fork(void)
{
        int status, sum = 0, n = 0;
        pid_t pid;

        status = in_child(cow_child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
        /* The child's writes were its own. */
        CHECK(global == 1 && child_flag == 0 && bss_block[0] == 0x5a);

        /*
         * The parent writes after fork; the child, which waits for a flag
         * file, must still see the old values. This needs the parent's
         * own mappings write-protected and flushed at fork.
         */
        {
                char flag[96];
                struct stat st;
                int fd;

                snprintf(flag, sizeof(flag), "%s/cow%d", dir, getpid());
                global = 1;
                bss_block[100] = 0x5a;
                if ((pid = fork()) == 0) {
                        while (stat(flag, &st) != 0)
                                continue;
                        _exit(global == 1 && bss_block[100] == 0x5a ? 0 : 1);
                }
                global = 3;
                bss_block[100] = 0x33;
                fd = open(flag, O_WRONLY | O_CREAT, 0644);
                close(fd);
                CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0);
                CHECK(unlink(flag) == 0);
                global = 1;
                bss_block[100] = 0x5a;
        }

        /* Many children, reaped in any order. */
        for (int i = 1; i <= 16; i++) {
                if ((pid = fork()) == 0)
                        _exit(i);
                CHECK(pid > 0);
        }
        while ((pid = waitpid(-1, &status, 0)) > 0) {
                sum += WEXITSTATUS(status);
                n++;
        }
        CHECK(n == 16 && sum == 16 * 17 / 2);
        CHECK(pid == -1 && errno == ECHILD);

        /*
         * WNOHANG on a child still running: it waits for a file that is
         * made only after the check.
         */
        {
                char flag[96];
                struct stat st;
                int fd;

                snprintf(flag, sizeof(flag), "%s/flag%d", dir, getpid());
                if ((pid = fork()) == 0) {
                        while (stat(flag, &st) != 0)
                                continue;
                        _exit(5);
                }
                CHECK(waitpid(pid, &status, WNOHANG) == 0);
                fd = open(flag, O_WRONLY | O_CREAT, 0644);
                CHECK(fd >= 0);
                close(fd);
                CHECK(waitpid(pid, &status, 0) == pid &&
                    WEXITSTATUS(status) == 5);
                CHECK(unlink(flag) == 0);
        }

        /* The old wait() returns the status in the second register. */
        if ((pid = fork()) == 0)
                _exit(9);
        {
                struct rawsys r = RAWSYS0(SYS_wait);

                CHECK(r.rv == pid && (r.rv2 >> 8) == 9);
        }
}

/*
 * Parent and child copy the same pages on write at the same time, many
 * times over: whichever copies second must find its own page intact.
 */
#define RACE_PAGES      256
static char race_buf[RACE_PAGES * 4096];

static void
test_cow_race(void)
{
        int ok = 1;

        for (int round = 0; round < 8; round++) {
                pid_t pid;
                int status;

                for (int i = 0; i < RACE_PAGES; i++)
                        race_buf[i * 4096] = (char)round;
                if ((pid = fork()) == 0) {
                        int good = 1;

                        for (int i = 0; i < RACE_PAGES; i++)
                                race_buf[i * 4096 + 1] = 'c';
                        for (int i = 0; i < RACE_PAGES; i++)
                                good &= race_buf[i * 4096] == (char)round &&
                                    race_buf[i * 4096 + 1] == 'c';
                        _exit(good ? 0 : 1);
                }
                for (int i = 0; i < RACE_PAGES; i++)
                        race_buf[i * 4096 + 1] = 'p';
                for (int i = 0; i < RACE_PAGES; i++)
                        ok &= race_buf[i * 4096] == (char)round &&
                            race_buf[i * 4096 + 1] == 'p';
                ok &= waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0;
        }
        CHECK(ok);
}

static void
exec_child(void)
{
        char *argv[] = { "argtest", "one", "two words", NULL };
        char *envp[] = { "FOO=bar", "X=1", NULL };

        (void)execve(path("/bin/argtest"), argv, envp);
        _exit(3);
}

static void
exec_missing(void)
{
        char *argv[] = { "nothing", NULL };

        if (execve(path("/bin/nothing"), argv, NULL) == -1 &&
            errno == ENOENT)
                _exit(4);
        _exit(5);
}

static void
test_exec(void)
{
        int status = in_child(exec_child);

        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
        status = in_child(exec_missing);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 4);
}

static void
null_write(void)
{
        *(volatile int *)0 = 1;
}

/*
 * Written at run time, so that it stays in .data: a compiler may put an
 * array it sees never written in .rodata, which shares the text segment.
 */
static void
exec_data(void)
{
        static unsigned char code[1];

        *(volatile unsigned char *)code = 0xc3;         /* ret */
        ((void (*)(void))(void *)code)();
}

static volatile int zero, one = 1;

/* Both operands volatile: the compiler must emit a real division. */
static void
divide(void)
{
        printf("%d\n", one / zero);
}

static void
bad_opcode(void)
{
        __asm__ __volatile__("ud2");
}

static void
readonly_write(void)
{
        int *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);

        *p = 1;
        if (mprotect(p, 4096, PROT_READ) != 0)
                _exit(2);
        if (*p != 1)
                _exit(3);
        *p = 2;
}

static void
text_write(void)
{
        *(volatile unsigned char *)(void *)test_basics = 0;
}

static void
recurse(int depth, int limit)
{
        volatile char frame[4096];

        frame[0] = (char)depth;
        /* Its address escapes, so the whole frame is kept. */
        __asm__ __volatile__("" : : "r"(frame) : "memory");
        if (depth < limit)
                recurse(depth + 1, limit);
        frame[1] = frame[0];
}

static void
deep_stack(void)
{
        recurse(0, 1500);               /* about 6 MB */
}

static void
too_deep(void)
{
        recurse(0, 4000);               /* past the 8 MB stack */
}

static void
test_faults(void)
{
        int s;

        s = in_child(null_write);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGSEGV);
        s = in_child(exec_data);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGSEGV);
        s = in_child(divide);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGFPE);
        s = in_child(bad_opcode);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGILL);
        s = in_child(readonly_write);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGSEGV);
        s = in_child(text_write);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGSEGV);
        s = in_child(deep_stack);
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0);
        s = in_child(too_deep);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGSEGV);
}

static unsigned char filebuf[20000];
static unsigned char readbuf[20000];

static int
dir_has(const char *d, const char *name)
{
        DIR *dp = opendir(d);
        struct dirent *de;
        int found = 0;

        if (dp == NULL)
                return 0;
        while ((de = readdir(dp)) != NULL) {
                if (strcmp(de->d_name, name) == 0)
                        found = 1;
        }
        closedir(dp);
        return found;
}

static void
test_files(void)
{
        char tmp[64], f1[96], f2[96], link[96], target[96];
        struct stat st;
        int fd, fd2, ok;

        snprintf(tmp, sizeof(tmp), "%s/u%d", dir, getpid());
        snprintf(f1, sizeof(f1), "%s/data", tmp);
        snprintf(f2, sizeof(f2), "%s/moved", tmp);
        snprintf(link, sizeof(link), "%s/link", tmp);
        CHECK(mkdir(tmp, 0755) == 0);
        CHECK(mkdir(tmp, 0755) == -1 && errno == EEXIST);

        for (size_t i = 0; i < sizeof(filebuf); i++)
                filebuf[i] = (unsigned char)(i * 13 + 7);
        fd = open(f1, O_RDWR | O_CREAT | O_EXCL, 0644);
        CHECK(fd >= 0);
        CHECK(write(fd, filebuf, sizeof(filebuf)) == sizeof(filebuf));
        CHECK(lseek(fd, 0, SEEK_CUR) == sizeof(filebuf));
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        CHECK(read(fd, readbuf, sizeof(readbuf)) == sizeof(readbuf));
        CHECK(memcmp(filebuf, readbuf, sizeof(filebuf)) == 0);
        CHECK(read(fd, readbuf, 10) == 0);
        CHECK(lseek(fd, -100, SEEK_END) == sizeof(filebuf) - 100);
        CHECK(fstat(fd, &st) == 0 && st.st_size == sizeof(filebuf) &&
            (st.st_mode & S_IFMT) == S_IFREG && st.st_nlink == 1);
        CHECK(pwrite(fd, "XYZ", 3, 5000) == 3);
        CHECK(pread(fd, readbuf, 5, 4999) == 5 && readbuf[1] == 'X' &&
            readbuf[3] == 'Z');
        CHECK(ftruncate(fd, 5000) == 0);
        CHECK(fstat(fd, &st) == 0 && st.st_size == 5000);

        /* dup and fcntl share the offset; close-on-exec is per slot. */
        fd2 = dup(fd);
        CHECK(fd2 > fd);
        CHECK(lseek(fd, 100, SEEK_SET) == 100 && lseek(fd2, 0, SEEK_CUR) == 100);
        CHECK(fcntl(fd, F_GETFD, 0) == 0);
        CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
            fcntl(fd, F_GETFD, 0) == FD_CLOEXEC && fcntl(fd2, F_GETFD, 0) == 0);
        CHECK((fcntl(fd, F_GETFL, 0) & O_ACCMODE) == O_RDWR);
        CHECK(fcntl(fd, F_DUPFD, 20) == 20 && close(20) == 0);
        CHECK(close(fd2) == 0 && close(fd2) == -1 && errno == EBADF);
        CHECK(close(fd) == 0);

        /* Append, and a file shared with a child through fork. */
        fd = open(f1, O_WRONLY | O_APPEND, 0);
        CHECK(fd >= 0);
        if (fork() == 0) {
                _exit(write(fd, "child", 5) == 5 ? 0 : 1);
        }
        {
                int s;

                CHECK(waitpid(-1, &s, 0) > 0 && WEXITSTATUS(s) == 0);
        }
        CHECK(write(fd, "parent", 6) == 6);
        CHECK(close(fd) == 0);
        fd = open(f1, O_RDONLY, 0);
        CHECK(pread(fd, readbuf, 11, 5000) == 11 &&
            memcmp(readbuf, "childparent", 11) == 0);

        /* A private file mapping: the file's bytes, copied on write. */
        {
                unsigned char *m = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE,
                    fd, 0);

                CHECK(m != MAP_FAILED);
                if (m != MAP_FAILED) {
                        CHECK(memcmp(m, filebuf, 4096) == 0 && m[5000] == 'c');
                        /* Past the end of the file in the last page: 0. */
                        CHECK(m[5011] == 0 && m[8191] == 0);
                        CHECK(mprotect(m, 8192, PROT_READ | PROT_WRITE) == 0);
                        m[0] = (unsigned char)~filebuf[0];
                        CHECK(pread(fd, readbuf, 1, 0) == 1 &&
                            readbuf[0] == filebuf[0]);
                        /* A mapped file cannot be cut short under it. */
                        {
                                int wfd = open(f1, O_WRONLY, 0);

                                CHECK(ftruncate(wfd, 10) == -1 &&
                                    errno == ETXTBSY);
                                close(wfd);
                        }
                        CHECK(munmap(m, 8192) == 0);
                }
                CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, 0) == MAP_FAILED);
        }
        CHECK(close(fd) == 0);

        /* Names. */
        CHECK(rename(f1, f2) == 0);
        CHECK(stat(f1, &st) == -1 && errno == ENOENT);
        CHECK(stat(f2, &st) == 0 && st.st_size == 5011);
        CHECK(symlink("moved", link) == 0);
        {
                ssize_t n = readlink(link, target, sizeof(target));

                CHECK(n == 5 && memcmp(target, "moved", 5) == 0);
        }
        CHECK(dir_has(tmp, "moved") && dir_has(tmp, "link") &&
            !dir_has(tmp, "data"));
        CHECK(dir_has(path("/bin"), "utest"));

        /* Relative names start at the current directory. */
        CHECK(chdir(tmp) == 0);
        fd = open("link", O_RDONLY, 0);
        CHECK(fd >= 0 && read(fd, readbuf, 4) == 4 &&
            memcmp(readbuf, filebuf, 4) == 0);
        close(fd);
        CHECK(chdir("/") == 0);
        fd = open(tmp, O_RDONLY, 0);
        CHECK(fd >= 0 && fchdir(fd) == 0);
        close(fd);
        CHECK(stat("moved", &st) == 0);
        CHECK(chdir("/") == 0);

        CHECK(unlink(link) == 0 && unlink(f2) == 0);
        CHECK(rmdir(tmp) == 0);
        ok = stat(tmp, &st) == -1 && errno == ENOENT;
        CHECK(ok);
}

/* Floating point: each process keeps its own registers across switches. */
static double
fp_work(double seed, long n)
{
        double x = seed, sum = 0;

        for (long i = 0; i < n; i++) {
                x = x * 1.0000001 + 0.5;
                if (x > 1e6)
                        x /= 3.0;
                sum += x;
        }
        return sum;
}

static void
test_fpu(void)
{
        double want[4], got;
        pid_t pids[4];
        int ok = 1;

        for (int i = 0; i < 4; i++)
                want[i] = fp_work(i + 1.5, 3000000);
        for (int i = 0; i < 4; i++) {
                if ((pids[i] = fork()) == 0)
                        _exit(fp_work(i + 1.5, 3000000) == want[i] ? 0 : 1);
        }
        got = fp_work(0.25, 3000000);
        for (int i = 0; i < 4; i++) {
                int s;

                ok &= waitpid(pids[i], &s, 0) == pids[i] && WIFEXITED(s) &&
                    WEXITSTATUS(s) == 0;
        }
        CHECK(ok);
        CHECK(got == fp_work(0.25, 3000000));
}

/*
 * The LWP private pointer is the %fs base, inherited by fork. mlibc's
 * thread block is the %fs base too, so the test runs in a child, which
 * makes no library call until it has put its own back.
 */
static void *tls_block[4];

static void *
fs0(void)
{
        void *v;

        __asm__ __volatile__("movq %%fs:0, %0" : "=r"(v));
        return v;
}

static void
tls_child(void)
{
        void *mine = fs0();     /* the thread block points at itself */
        int set, seen, inherited;
        pid_t pid;

        failures = 0;
        tls_block[0] = tls_block;
        set = rawsys_lwp_private(tls_block) == 0;
        seen = fs0() == (void *)tls_block;
        if ((pid = fork1()) == 0)
                rawsys_exit(fs0() == (void *)tls_block ? 0 : 1);
        (void)rawsys_lwp_private(mine);
        CHECK(set && seen);
        CHECK(pid > 0 && waitpid(pid, &inherited, 0) == pid &&
            WIFEXITED(inherited) && WEXITSTATUS(inherited) == 0);
        fflush(stdout);
        _exit(failures);
}

static void
test_tls(void)
{
        int s = in_child(tls_child);

        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0);
}

int
main(int argc, char **argv)
{
        if (argc != 2) {
                printf("usage: utest dir\n");
                return 100;
        }
        dir = argv[1];
        setvbuf(stdout, NULL, _IONBF, 0);
        test_basics();
        test_memory();
        test_fork();
        test_cow_race();
        test_exec();
        test_faults();
        test_files();
        test_fpu();
        test_tls();
        printf("utest: pid %d: %d failures\n", getpid(), failures);
        return failures;
}
