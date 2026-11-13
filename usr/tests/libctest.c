/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 */

/*
 * The C library test: a program built against mlibc, checking that
 * the library's view of the system matches the kernel's: start-up and
 * the environment, the heap, stdio on files and directories, errno,
 * processes and wait statuses, signals and handlers, time, the thread
 * pointer, the clocks and processor time, pipes and named FIFOs, poll
 * and select, ppoll and pselect, resource limits and getrusage,
 * vectored I/O, the *at calls, and getcwd().
 *
 * usage: libctest dir, where dir holds bin/ and is writable. Exits with
 * the number of failed checks.
 */

#define _DEFAULT_SOURCE         /* POSIX, XSI, and the BSD and SVID extras */
#define _POSIX_C_SOURCE 202405L /* mlibc wants it named: renameat, ppoll */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static const char *dir;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("libctest: %d: %s\n", __LINE__, #expr);  \
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

static int
cmp_int(const void *a, const void *b)
{
        int x = *(const int *)a, y = *(const int *)b;

        return x < y ? -1 : x > y;
}

/* Formatting, conversions, and the string functions. */
static void
test_strings(void)
{
        char buf[128];
        int v[] = { 5, -3, 9, 0, 2 };
        char *end;

        snprintf(buf, sizeof(buf), "%d %5s|%-4x|%lld %.3f %c", -42, "ab",
            0xbe, (long long)1 << 40, 2.5, 'z');
        CHECK(strcmp(buf, "-42    ab|be  |1099511627776 2.500 z") == 0);
        CHECK(strtol("  -0x1f", &end, 0) == -31 && *end == '\0');
        CHECK(strtoul("777", NULL, 8) == 511);
        CHECK(strtod("1.5e3", NULL) == 1500.0);
        CHECK(atoi("123abc") == 123);
        qsort(v, 5, sizeof(v[0]), cmp_int);
        CHECK(v[0] == -3 && v[1] == 0 && v[2] == 2 && v[3] == 5 && v[4] == 9);
        CHECK(strstr("belka kernel", "kern") != NULL);
        CHECK(strncmp("abcdef", "abcxyz", 3) == 0);
        CHECK(sscanf("12 ab 3.5", "%d %2s %lf", &v[0], buf, &(double){0}) == 3);
        CHECK(v[0] == 12 && strcmp(buf, "ab") == 0);
}

/* malloc and friends, small and large, and mmap underneath them. */
static void
test_heap(void)
{
        char *small[64];
        char *big, *p;
        int ok = 1;

        for (int i = 0; i < 64; i++) {
                small[i] = malloc((size_t)i * 13 + 1);
                if (small[i] == NULL)
                        ok = 0;
                else
                        memset(small[i], i, (size_t)i * 13 + 1);
        }
        for (int i = 0; i < 64; i++) {
                if (small[i] != NULL && small[i][i * 13] != (char)i)
                        ok = 0;
                free(small[i]);
        }
        CHECK(ok);
        big = malloc(4 << 20);
        CHECK(big != NULL);
        if (big != NULL) {
                big[0] = 1;
                big[(4 << 20) - 1] = 2;
                p = realloc(big, 8 << 20);
                CHECK(p != NULL && p[0] == 1 && p[(4 << 20) - 1] == 2);
                free(p != NULL ? p : big);
        }
        p = calloc(1000, 4);
        CHECK(p != NULL);
        if (p != NULL) {
                ok = 1;
                for (int i = 0; i < 4000; i++)
                        if (p[i] != 0)
                                ok = 0;
                CHECK(ok);
                free(p);
        }
        p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
            -1, 0);
        CHECK(p != MAP_FAILED);
        if (p != MAP_FAILED) {
                p[8191] = 7;
                CHECK(munmap(p, 8192) == 0);
        }
}

/* The environment and the arguments the kernel passed. */
static void
test_env(int argc, char **argv)
{
        CHECK(argc == 2);
        CHECK(strcmp(argv[0], "libctest") == 0);
        CHECK(getenv("BELKA") != NULL && strcmp(getenv("BELKA"), "1") == 0);
        CHECK(getenv("NOT_SET") == NULL);
        CHECK(setenv("LIBCTEST", "yes", 1) == 0);
        CHECK(strcmp(getenv("LIBCTEST"), "yes") == 0);
        CHECK(unsetenv("LIBCTEST") == 0 && getenv("LIBCTEST") == NULL);
}

/* stdio on a real file, stat, errno, and directory reading. */
static void
test_files(void)
{
        char line[64];
        struct stat st;
        FILE *f;
        DIR *d;
        struct dirent *de;
        int seen_self = 0, seen_hello = 0, n = 0, fd;

        f = fopen(path("/libctest.tmp"), "w");
        CHECK(f != NULL);
        if (f != NULL) {
                for (int i = 0; i < 1000; i++)
                        fprintf(f, "line %d\n", i);
                CHECK(fclose(f) == 0);
        }
        CHECK(stat(path("/libctest.tmp"), &st) == 0);
        CHECK(S_ISREG(st.st_mode) && st.st_size == 8890);
        f = fopen(path("/libctest.tmp"), "r");
        CHECK(f != NULL);
        if (f != NULL) {
                while (fgets(line, sizeof(line), f) != NULL)
                        n++;
                CHECK(n == 1000 && strcmp(line, "line 999\n") == 0);
                CHECK(fseek(f, 5, SEEK_SET) == 0 && fgetc(f) == '0');
                CHECK(ftell(f) == 6);
                CHECK(fstat(fileno(f), &st) == 0 && st.st_size == 8890);
                fclose(f);
        }
        f = fopen(path("/libctest.tmp"), "a");
        CHECK(f != NULL);
        if (f != NULL) {
                fputs("tail\n", f);
                fclose(f);
        }
        CHECK(stat(path("/libctest.tmp"), &st) == 0 && st.st_size == 8895);
        CHECK(rename(path("/libctest.tmp"), path("/libctest.mv")) == 0);
        CHECK(unlink(path("/libctest.mv")) == 0);
        CHECK(unlink(path("/libctest.mv")) == -1 && errno == ENOENT);
        CHECK(fopen(path("/no/such/file"), "r") == NULL && errno == ENOENT);
        CHECK(strerror(ENOENT) != NULL && strlen(strerror(ENOENT)) > 0);

        /* open() flags the library carries out itself. */
        fd = open(path("/bin"), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
        if (fd >= 0)
                close(fd);
        CHECK(open(path("/bin/hello"), O_RDONLY | O_DIRECTORY) == -1 &&
            errno == ENOTDIR);
        CHECK(mkdir(path("/libctest.d"), 0755) == 0);
        CHECK(mkdir(path("/libctest.d"), 0755) == -1 && errno == EEXIST);
        CHECK(rmdir(path("/libctest.d")) == 0);

        d = opendir(path("/bin"));
        CHECK(d != NULL);
        if (d != NULL) {
                n = 0;
                while ((de = readdir(d)) != NULL) {
                        n++;
                        if (strcmp(de->d_name, "libctest") == 0)
                                seen_self = 1;
                        if (strcmp(de->d_name, "hello") == 0)
                                seen_hello = 1;
                }
                CHECK(closedir(d) == 0);
                CHECK(seen_self && seen_hello && n >= 4);
        }
        d = opendir(path("/many"));
        CHECK(d != NULL);
        if (d != NULL) {
                n = 0;
                while ((de = readdir(d)) != NULL)
                        if (strncmp(de->d_name, "f", 1) == 0)
                                n++;
                closedir(d);
                CHECK(n == 200);
        }
}

/* fork, exit statuses, exec, and abort. */
static void
test_procs(void)
{
        int status = -1;
        pid_t pid, self = getpid();

        CHECK(self > 1 && getppid() >= 0);
        pid = fork();
        if (pid == 0) {
                if (getpid() == self || getppid() != self)
                        _exit(99);
                exit(7);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);

        pid = fork();
        if (pid == 0) {
                abort();
                _exit(98);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

        pid = fork();
        if (pid == 0) {
                char *argv[] = { "hello", NULL };
                char *envp[] = { NULL };

                execve(path("/bin/hello"), argv, envp);
                _exit(97);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}

static volatile sig_atomic_t got_sig, got_code;
static jmp_buf jb;
static volatile uintptr_t bad_addr = 16;       /* unmapped; hidden from the compiler */

static void
on_usr1(int sig)
{
        got_sig = sig;
}

static void
on_info(int sig, siginfo_t *si, void *uc)
{
        (void)uc;
        got_sig = sig;
        got_code = si->si_code;
}

static void
on_segv(int sig)
{
        (void)sig;
        longjmp(jb, 1);
}

/* Handlers, masks, and resuming after a handler returns. */
static void
test_signals(void)
{
        struct sigaction sa;
        sigset_t set, old, pend;

        CHECK(signal(SIGUSR1, on_usr1) != SIG_ERR);
        got_sig = 0;
        CHECK(raise(SIGUSR1) == 0);
        CHECK(got_sig == SIGUSR1);

        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_info;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);
        got_sig = 0;
        got_code = 1;
        CHECK(kill(getpid(), SIGUSR2) == 0);
        CHECK(got_sig == SIGUSR2 && got_code == SI_USER);

        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0);
        got_sig = 0;
        raise(SIGUSR1);
        CHECK(got_sig == 0);
        CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1) == 1);
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
        CHECK(got_sig == SIGUSR1);

        signal(SIGSEGV, on_segv);
        if (setjmp(jb) == 0) {
                *(volatile int *)bad_addr = 1;
                CHECK(!"no fault");
        }
        signal(SIGSEGV, SIG_DFL);
}

/* The clocks, uname, and errno, which lives at the thread pointer. */
static void
test_misc(void)
{
        struct timespec ts;
        struct timeval tv;
        struct utsname u;
        time_t t = time(NULL);

        CHECK(t > 1700000000);
        CHECK(clock_gettime(CLOCK_REALTIME, &ts) == 0);
        CHECK(ts.tv_sec >= t && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);
        CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
        CHECK(gettimeofday(&tv, NULL) == 0 && tv.tv_sec >= t);
        CHECK(uname(&u) == 0 && strcmp(u.sysname, "Belka") == 0);
        errno = 0;
        CHECK(close(-1) == -1 && errno == EBADF);
        CHECK(&errno != NULL);
}

static long
mono_ms(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long
cpu_ms(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Spin until this process has used ms more of processor time. */
static void
burn_cpu(long ms)
{
        long t0 = mono_ms(), c0 = cpu_ms();

        do {
                for (volatile int i = 0; i < 100000; i++)
                        continue;
        } while (cpu_ms() - c0 < ms && mono_ms() - t0 < 20000);
}

/* The clocks, sleeping, processor time, and sched_yield(). */
static void
test_clocks(void)
{
        struct timespec res, a, b, req = { 0, 30000000 };
        struct timeval tv;
        struct tms t0, t1;
        clock_t e0, e1, c0;
        int ok = 1, status = -1;
        pid_t pid;
        long t;

        /* 1 ns from an invariant TSC, else a clock tick. */
        CHECK(clock_getres(CLOCK_MONOTONIC, &res) == 0);
        CHECK(res.tv_sec == 0 &&
            (res.tv_nsec == 1 || res.tv_nsec == 10000000));
        /* At 1 ns, reads a few microseconds apart differ. */
        if (res.tv_nsec == 1) {
                int distinct = 0;

                clock_gettime(CLOCK_MONOTONIC, &a);
                for (int i = 0; i < 1000; i++) {
                        clock_gettime(CLOCK_MONOTONIC, &b);
                        if (b.tv_nsec != a.tv_nsec || b.tv_sec != a.tv_sec)
                                distinct++;
                        a = b;
                }
                CHECK(distinct > 100);
        }
        CHECK(clock_getres(CLOCK_PROCESS_CPUTIME_ID, &res) == 0 &&
            res.tv_sec == 0 && res.tv_nsec == 10000000);
        CHECK(clock_gettime(99, &a) == -1 && errno == EINVAL);

        /* Monotonic: never earlier than the read before. */
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 10000; i++) {
                clock_gettime(CLOCK_MONOTONIC, &b);
                if (b.tv_sec < a.tv_sec ||
                    (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec))
                        ok = 0;
                a = b;
        }
        CHECK(ok);
        CHECK(clock_gettime(CLOCK_BOOTTIME, &b) == 0 && b.tv_sec >= a.tv_sec);

        /* The time of day agrees with gettimeofday(). */
        CHECK(clock_gettime(CLOCK_REALTIME, &a) == 0 &&
            gettimeofday(&tv, NULL) == 0);
        CHECK(tv.tv_sec - a.tv_sec <= 1 && tv.tv_sec >= a.tv_sec);

        t = mono_ms();
        CHECK(nanosleep(&req, NULL) == 0);
        t = mono_ms() - t;
        CHECK(t >= 30 && t < 2000);

        /* Processor time: this process's, an LWP's, and a child's. */
        e0 = times(&t0);
        c0 = clock();
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
        burn_cpu(100);
        e1 = times(&t1);
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
        CHECK(e0 != (clock_t)-1 && e1 >= e0);
        CHECK(t1.tms_utime - t0.tms_utime >= 8);
        CHECK(clock() - c0 >= 80000);
        CHECK(b.tv_sec * 1000 + b.tv_nsec / 1000000 -
            (a.tv_sec * 1000 + a.tv_nsec / 1000000) >= 80);
        CHECK(sysconf(_SC_CLK_TCK) == 100);

        pid = fork();
        if (pid == 0) {
                burn_cpu(100);
                _exit(0);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        times(&t1);
        CHECK(t1.tms_cutime - t0.tms_cutime >= 8);

        CHECK(sched_yield() == 0);
}

static volatile int got_sigpipe;

static void
on_sigpipe(int sig)
{
        (void)sig;
        got_sigpipe++;
}

/* Read exactly n bytes, or fewer at end of file. */
static ssize_t
read_full(int fd, void *buf, size_t n)
{
        size_t done = 0;

        while (done < n) {
                ssize_t r = read(fd, (char *)buf + done, n - done);

                if (r <= 0)
                        return r < 0 ? r : (ssize_t)done;
                done += (size_t)r;
        }
        return (ssize_t)done;
}

/* Pipes: data, end of file, EPIPE, non-blocking ends, and atomic writes. */
static void
test_pipes(void)
{
        static char big[100000], rec[PIPE_BUF];
        char buf[64];
        struct stat st;
        struct iovec iov[3];
        int fds[2], status = -1, ok, flags;
        ssize_t n, total;
        pid_t pid, kids[2];

        CHECK(pipe(fds) == 0);
        CHECK(write(fds[1], "hello", 5) == 5);
        CHECK(read(fds[0], buf, sizeof(buf)) == 5 &&
            memcmp(buf, "hello", 5) == 0);
        CHECK(fstat(fds[0], &st) == 0 && S_ISFIFO(st.st_mode));
        CHECK(lseek(fds[0], 0, SEEK_SET) == -1 && errno == ESPIPE);
        CHECK(pread(fds[0], buf, 1, 0) == -1 && errno == ESPIPE);
        CHECK(write(fds[0], "x", 1) == -1 && errno == EBADF);

        /* Vectored I/O through it. */
        iov[0].iov_base = "ab";
        iov[0].iov_len = 2;
        iov[1].iov_base = "cde";
        iov[1].iov_len = 3;
        iov[2].iov_base = "f";
        iov[2].iov_len = 1;
        CHECK(writev(fds[1], iov, 3) == 6);
        memset(buf, 0, sizeof(buf));
        iov[0].iov_base = buf;
        iov[0].iov_len = 4;
        iov[1].iov_base = buf + 10;
        iov[1].iov_len = 10;
        CHECK(readv(fds[0], iov, 2) == 6);
        CHECK(memcmp(buf, "abcd", 4) == 0 && memcmp(buf + 10, "ef", 2) == 0);
        CHECK(fsync(fds[0]) == -1 && errno == EINVAL);

        CHECK(close(fds[1]) == 0);
        CHECK(read(fds[0], buf, sizeof(buf)) == 0);
        close(fds[0]);

        /* No reader: SIGPIPE, then EPIPE. */
        CHECK(signal(SIGPIPE, on_sigpipe) != SIG_ERR);
        CHECK(pipe(fds) == 0);
        close(fds[0]);
        got_sigpipe = 0;
        CHECK(write(fds[1], "x", 1) == -1 && errno == EPIPE);
        CHECK(got_sigpipe == 1);
        close(fds[1]);
        signal(SIGPIPE, SIG_DFL);

        /* Non-blocking: EAGAIN on an empty pipe and on a full one. */
        CHECK(pipe(fds) == 0);
        flags = fcntl(fds[0], F_GETFL);
        CHECK(fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) == 0);
        flags = fcntl(fds[1], F_GETFL);
        CHECK(fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
        CHECK(read(fds[0], buf, 1) == -1 && errno == EAGAIN);
        memset(rec, 'r', sizeof(rec));
        total = 0;
        while ((n = write(fds[1], rec, sizeof(rec))) > 0)
                total += n;
        CHECK(n == -1 && errno == EAGAIN);
        CHECK(total >= PIPE_BUF && total % PIPE_BUF == 0);
        close(fds[0]);
        close(fds[1]);

        /* More than a pipe holds, from a child, read back in order. */
        for (size_t i = 0; i < sizeof(big); i++)
                big[i] = (char)(i * 13 + 7);
        CHECK(pipe(fds) == 0);
        pid = fork();
        if (pid == 0) {
                close(fds[0]);
                _exit(write(fds[1], big, sizeof(big)) == (ssize_t)sizeof(big) ?
                    0 : 1);
        }
        close(fds[1]);
        ok = 1;
        total = 0;
        for (;;) {
                n = read(fds[0], rec, sizeof(rec));
                if (n <= 0)
                        break;
                if (total + n > (ssize_t)sizeof(big) ||
                    memcmp(rec, big + total, (size_t)n) != 0)
                        ok = 0;
                total += n;
        }
        CHECK(ok && n == 0 && total == (ssize_t)sizeof(big));
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(fds[0]);

        /* Two writers of PIPE_BUF records: no record is interleaved. */
        CHECK(pipe(fds) == 0);
        for (int k = 0; k < 2; k++) {
                kids[k] = fork();
                if (kids[k] == 0) {
                        close(fds[0]);
                        memset(rec, 'A' + k, sizeof(rec));
                        for (int i = 0; i < 100; i++) {
                                if (write(fds[1], rec, sizeof(rec)) !=
                                    (ssize_t)sizeof(rec))
                                        _exit(1);
                        }
                        _exit(0);
                }
        }
        close(fds[1]);
        ok = 1;
        total = 0;
        while ((n = read_full(fds[0], rec, sizeof(rec))) == (ssize_t)sizeof(rec)) {
                for (size_t i = 1; i < sizeof(rec); i++) {
                        if (rec[i] != rec[0])
                                ok = 0;
                }
                total++;
        }
        CHECK(ok && n == 0 && total == 200);
        for (int k = 0; k < 2; k++)
                CHECK(kids[k] > 0 && waitpid(kids[k], &status, 0) == kids[k] &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(fds[0]);
}

/*
 * A named FIFO at name: making it, its attributes, the non-blocking
 * opens, the rendezvous of a blocking reader and writer either way
 * round, end of file, and its data going once both ends are closed.
 */
static void
test_fifo_at(const char *name)
{
        struct stat st, fst;
        char buf[8];
        int rfd, wfd, fd, status;
        pid_t kid;

        CHECK(mkfifo(name, 0640) == 0);
        CHECK(mkfifo(name, 0640) == -1 && errno == EEXIST);
        CHECK(stat(name, &st) == 0 && S_ISFIFO(st.st_mode) &&
            (st.st_mode & 07777) == 0640 && st.st_size == 0);

        /* No reader: a non-blocking writer fails; a reader goes ahead. */
        CHECK(open(name, O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO);
        rfd = open(name, O_RDONLY | O_NONBLOCK);
        CHECK(rfd >= 0);
        CHECK(read(rfd, buf, sizeof(buf)) == 0);        /* no writer: EOF */
        wfd = open(name, O_WRONLY | O_NONBLOCK);
        CHECK(wfd >= 0);
        CHECK(fstat(wfd, &fst) == 0 && S_ISFIFO(fst.st_mode) &&
            fst.st_ino == st.st_ino && fst.st_dev == st.st_dev);
        CHECK(lseek(wfd, 0, SEEK_SET) == -1 && errno == ESPIPE);
        CHECK(write(wfd, "hello", 5) == 5);
        CHECK(read(rfd, buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0);
        CHECK(read(rfd, buf, sizeof(buf)) == -1 && errno == EAGAIN);
        close(wfd);
        CHECK(read(rfd, buf, sizeof(buf)) == 0);
        close(rfd);

        /* A blocking writer waits for the reader. */
        kid = fork();
        if (kid == 0) {
                fd = open(name, O_WRONLY);
                _exit(fd >= 0 && write(fd, "ping", 4) == 4 ? 0 : 1);
        }
        usleep(50000);
        rfd = open(name, O_RDONLY);
        CHECK(rfd >= 0);
        CHECK(read_full(rfd, buf, 4) == 4 && memcmp(buf, "ping", 4) == 0);
        CHECK(read(rfd, buf, sizeof(buf)) == 0);
        close(rfd);
        CHECK(kid > 0 && waitpid(kid, &status, 0) == kid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);

        /* A blocking reader waits for the writer. */
        kid = fork();
        if (kid == 0) {
                fd = open(name, O_RDONLY);
                _exit(fd >= 0 && read_full(fd, buf, 4) == 4 &&
                    memcmp(buf, "pong", 4) == 0 ? 0 : 1);
        }
        usleep(50000);
        wfd = open(name, O_WRONLY);
        CHECK(wfd >= 0);
        CHECK(write(wfd, "pong", 4) == 4);
        close(wfd);
        CHECK(kid > 0 && waitpid(kid, &status, 0) == kid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);

        /* Opened for both, it never waits; closing both ends drops data. */
        fd = open(name, O_RDWR);
        CHECK(fd >= 0);
        CHECK(write(fd, "x", 1) == 1);
        close(fd);
        fd = open(name, O_RDWR | O_NONBLOCK);
        CHECK(fd >= 0);
        CHECK(read(fd, buf, sizeof(buf)) == -1 && errno == EAGAIN);
        close(fd);

        CHECK(unlink(name) == 0);
        CHECK(stat(name, &st) == -1 && errno == ENOENT);
}

static long
ms_since(const struct timespec *t0)
{
        struct timespec t;

        clock_gettime(CLOCK_MONOTONIC, &t);
        return (t.tv_sec - t0->tv_sec) * 1000 +
            (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/*
 * poll() and select(): the events of pipe ends and files, hang-up and
 * error, bad descriptors, the timeout, a wakeup by another process, and
 * a signal ending the wait.
 */
static void
test_poll(void)
{
        struct pollfd pfd[3];
        struct sigaction sa, old;
        struct timespec t0;
        struct timeval tv;
        fd_set rset, wset;
        int fds[2], fd, status;
        char c;
        pid_t kid;

        CHECK(pipe(fds) == 0);
        pfd[0] = (struct pollfd){ fds[0], POLLIN, 0 };
        pfd[1] = (struct pollfd){ fds[1], POLLOUT | POLLIN, 0 };
        pfd[2] = (struct pollfd){ -1, POLLIN, 0 };     /* ignored */
        CHECK(poll(pfd, 3, 0) == 1 && pfd[0].revents == 0 &&
            pfd[1].revents == POLLOUT && pfd[2].revents == 0);
        CHECK(write(fds[1], "x", 1) == 1);
        CHECK(poll(pfd, 1, 0) == 1 && pfd[0].revents == POLLIN);

        /* The timeout, with nothing to read. */
        CHECK(read(fds[0], &c, 1) == 1);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(poll(pfd, 1, 100) == 0 && pfd[0].revents == 0);
        CHECK(ms_since(&t0) >= 100);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(poll(NULL, 0, 30) == 0 && ms_since(&t0) >= 30);

        /* select() on the same pipe. */
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_SET(fds[0], &rset);
        FD_SET(fds[1], &wset);
        tv = (struct timeval){ 0, 0 };
        CHECK(select(fds[1] + 1, &rset, &wset, NULL, &tv) == 1 &&
            !FD_ISSET(fds[0], &rset) && FD_ISSET(fds[1], &wset));

        /* Another process writes while we wait without a timeout. */
        kid = fork();
        if (kid == 0) {
                usleep(50000);
                _exit(write(fds[1], "y", 1) == 1 ? 0 : 1);
        }
        CHECK(poll(pfd, 1, -1) == 1 && pfd[0].revents == POLLIN);
        CHECK(kid > 0 && waitpid(kid, &status, 0) == kid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
        FD_ZERO(&rset);
        FD_SET(fds[0], &rset);
        CHECK(select(fds[0] + 1, &rset, NULL, NULL, NULL) == 1 &&
            FD_ISSET(fds[0], &rset));
        CHECK(read(fds[0], &c, 1) == 1 && c == 'y');

        /* A signal ends the wait with EINTR, SA_RESTART or not. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sa.sa_flags = SA_RESTART;
        CHECK(sigaction(SIGUSR1, &sa, &old) == 0);
        got_sig = 0;
        kid = fork();
        if (kid == 0) {
                usleep(50000);
                kill(getppid(), SIGUSR1);
                _exit(0);
        }
        CHECK(poll(pfd, 1, 5000) == -1 && errno == EINTR &&
            got_sig == SIGUSR1);
        CHECK(kid > 0 && waitpid(kid, &status, 0) == kid);
        CHECK(sigaction(SIGUSR1, &old, NULL) == 0);

        /* No writer left: hang-up; no reader left: an error. */
        close(fds[1]);
        CHECK(poll(pfd, 1, 0) == 1 && pfd[0].revents == POLLHUP);
        CHECK(pipe(fds) == 0);
        close(fds[0]);
        pfd[0] = (struct pollfd){ fds[1], POLLOUT, 0 };
        CHECK(poll(pfd, 1, 0) == 1 && (pfd[0].revents & POLLERR) != 0);
        close(fds[1]);

        /* A closed descriptor, and a regular file (always ready). */
        pfd[0] = (struct pollfd){ fds[1], POLLIN, 0 };
        CHECK(poll(pfd, 1, 0) == 1 && pfd[0].revents == POLLNVAL);
        fd = open(path("/libctest.poll"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0);
        pfd[0] = (struct pollfd){ fd, POLLIN | POLLOUT, 0 };
        CHECK(poll(pfd, 1, -1) == 1 &&
            pfd[0].revents == (POLLIN | POLLOUT));
        close(fd);
        CHECK(unlink(path("/libctest.poll")) == 0);
}

/* Whether sig is in the calling thread's mask. */
static int
masked(int sig)
{
        sigset_t cur;

        return sigprocmask(SIG_BLOCK, NULL, &cur) == 0 &&
            sigismember(&cur, sig) == 1;
}

/*
 * ppoll() and pselect(): a timespec timeout, and a signal mask held
 * for the wait alone. A SIGUSR1 held and already pending ends a wait
 * whose mask lets it in, and is held again once the handler is done; a
 * wait that ends with an event puts the old mask back too.
 */
static void
test_ppoll(void)
{
        struct timespec t0, ts;
        struct pollfd pfd;
        struct sigaction sa, old;
        sigset_t usr1, usr2, none, pend;
        fd_set rset;
        int fds[2];
        char c;

        CHECK(pipe(fds) == 0);
        pfd = (struct pollfd){ fds[0], POLLIN, 0 };
        sigemptyset(&none);
        sigemptyset(&usr1);
        sigaddset(&usr1, SIGUSR1);
        sigemptyset(&usr2);
        sigaddset(&usr2, SIGUSR2);
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        CHECK(sigaction(SIGUSR1, &sa, &old) == 0);
        CHECK(sigprocmask(SIG_BLOCK, &usr1, NULL) == 0);

        /* The timeout, with no mask. */
        ts = (struct timespec){ 0, 50000000 };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(ppoll(&pfd, 1, &ts, NULL) == 0 && ms_since(&t0) >= 50);
        ts = (struct timespec){ 0, 1000000000 };
        CHECK(ppoll(&pfd, 1, &ts, NULL) == -1 && errno == EINVAL);

        /* The pending signal gets in only through the wait's mask. */
        got_sig = 0;
        CHECK(kill(getpid(), SIGUSR1) == 0 && got_sig == 0);
        ts = (struct timespec){ 0, 0 };
        CHECK(ppoll(&pfd, 1, &ts, &usr2) == 0 && got_sig == 0);
        CHECK(masked(SIGUSR1) && !masked(SIGUSR2));
        ts = (struct timespec){ 5, 0 };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(ppoll(&pfd, 1, &ts, &none) == -1 && errno == EINTR &&
            got_sig == SIGUSR1 && ms_since(&t0) < 5000);
        CHECK(masked(SIGUSR1));
        CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1) == 0);

        /* An event ends the wait, and the old mask comes back. */
        CHECK(write(fds[1], "z", 1) == 1);
        CHECK(ppoll(&pfd, 1, NULL, &usr2) == 1 && pfd.revents == POLLIN);
        CHECK(masked(SIGUSR1) && !masked(SIGUSR2));

        /* pselect(): the same with descriptor sets. */
        FD_ZERO(&rset);
        FD_SET(fds[0], &rset);
        CHECK(pselect(fds[0] + 1, &rset, NULL, NULL, NULL, &usr2) == 1 &&
            FD_ISSET(fds[0], &rset));
        CHECK(read(fds[0], &c, 1) == 1 && c == 'z');
        got_sig = 0;
        CHECK(kill(getpid(), SIGUSR1) == 0 && got_sig == 0);
        FD_ZERO(&rset);
        FD_SET(fds[0], &rset);
        ts = (struct timespec){ 5, 0 };
        CHECK(pselect(fds[0] + 1, &rset, NULL, NULL, &ts, &none) == -1 &&
            errno == EINTR && got_sig == SIGUSR1);
        CHECK(masked(SIGUSR1) && !masked(SIGUSR2));
        ts = (struct timespec){ 0, 30000000 };
        FD_ZERO(&rset);
        FD_SET(fds[0], &rset);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(pselect(fds[0] + 1, &rset, NULL, NULL, &ts, NULL) == 0 &&
            ms_since(&t0) >= 30);

        CHECK(sigprocmask(SIG_UNBLOCK, &usr1, NULL) == 0);
        CHECK(sigaction(SIGUSR1, &old, NULL) == 0);
        close(fds[0]);
        close(fds[1]);
}

/* Run fn in a child; its wait status. */
static int
in_child(int (*fn)(void))
{
        int status = -1;
        pid_t kid = fork();

        if (kid == 0)
                _exit(fn());
        if (kid < 0)
                return -1;
        while (waitpid(kid, &status, 0) != kid)
                if (errno != EINTR)
                        return -1;
        return status;
}

static int
child_nofile(void)
{
        struct rlimit rl = { 8, 8 };
        int fd, last = -1;

        if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
                return 1;
        while ((fd = dup(0)) >= 0)
                last = fd;
        if (errno != EMFILE || last != 7)
                return 2;
        if (dup2(0, 8) != -1 || errno != EBADF)
                return 3;
        if (fcntl(0, F_DUPFD, 8) != -1 || errno != EINVAL)
                return 4;
        return 0;
}

static int
child_fsize(void)
{
        struct rlimit rl = { 10, RLIM_INFINITY };
        int fd = open(path("/libctest.fsize"), O_RDWR | O_CREAT | O_TRUNC,
            0644);

        if (fd < 0 || signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
            setrlimit(RLIMIT_FSIZE, &rl) != 0)
                return 1;
        if (write(fd, "0123456789abcdefghij", 20) != 10)
                return 2;
        if (write(fd, "x", 1) != -1 || errno != EFBIG)
                return 3;
        if (ftruncate(fd, 100) != -1 || errno != EFBIG)
                return 4;
        if (ftruncate(fd, 5) != 0)
                return 5;
        signal(SIGXFSZ, SIG_DFL);
        (void)pwrite(fd, "y", 1, 10);   /* SIGXFSZ ends the process */
        return 6;
}

static int
child_vmem(void)
{
        struct rlimit rl = { 64UL << 20, RLIM_INFINITY };
        void *p;

        if (setrlimit(RLIMIT_AS, &rl) != 0)
                return 1;
        if (mmap(NULL, 256UL << 20, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
            errno != ENOMEM)
                return 2;
        p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return p == MAP_FAILED ? 3 : 0;
}

static volatile sig_atomic_t got_xcpu;

static void
on_xcpu(int sig)
{
        (void)sig;
        got_xcpu = 1;
}

/* The CPU-limit child waited too long: say how long, by both clocks. */
static int
cpu_gave_up(int code, const struct timespec *t0)
{
        struct tms tms;

        times(&tms);
        printf("libctest: cpu limit child: gave up (%d) after %ld ms, "
            "%ld ticks of processor time\n", code, ms_since(t0),
            (long)tms.tms_utime);
        fflush(stdout);         /* the child leaves with _exit */
        return code;
}

/* Spin past one second of processor time (SIGXCPU) and two (SIGKILL). */
static int
child_cpu(void)
{
        struct rlimit rl = { 1, 2 };
        struct timespec t0;

        if (signal(SIGXCPU, on_xcpu) == SIG_ERR ||
            setrlimit(RLIMIT_CPU, &rl) != 0)
                return 1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!got_xcpu)
                if (ms_since(&t0) > 60000)
                        return cpu_gave_up(2, &t0);
        for (;;)
                if (ms_since(&t0) > 120000)
                        return cpu_gave_up(3, &t0);
}

/* getrlimit and setrlimit, and each limit enforced. */
static void
test_rlimits(void)
{
        struct rlimit rl;
        int status;

        CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 1024 &&
            rl.rlim_max == 1024);
        CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 &&
            rl.rlim_cur == 8UL << 20 && rl.rlim_max == RLIM_INFINITY);
        CHECK(getrlimit(RLIMIT_CPU, &rl) == 0 &&
            rl.rlim_cur == RLIM_INFINITY);
        CHECK(getrlimit(99, &rl) == -1 && errno == EINVAL);
        rl = (struct rlimit){ 2, 1 };
        CHECK(setrlimit(RLIMIT_CORE, &rl) == -1 && errno == EINVAL);

        status = in_child(child_nofile);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        status = in_child(child_fsize);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ);
        CHECK(unlink(path("/libctest.fsize")) == 0);
        status = in_child(child_vmem);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        status = in_child(child_cpu);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
                printf("libctest: cpu limit child: status %#x\n", status);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

        /* getrusage: the CPU child's two seconds, and our own time. */
        {
                struct rusage ru;
                struct tms tms;
                long ticks;

                CHECK(getrusage(RUSAGE_CHILDREN, &ru) == 0 &&
                    ru.ru_utime.tv_sec >= 2 && ru.ru_utime.tv_usec >= 0 &&
                    ru.ru_utime.tv_usec < 1000000);
                times(&tms);
                CHECK(getrusage(RUSAGE_SELF, &ru) == 0);
                ticks = ru.ru_utime.tv_sec * 100 + ru.ru_utime.tv_usec / 10000;
                CHECK(ticks >= (long)tms.tms_utime);
                CHECK(getrusage(5, &ru) == -1 && errno == EINVAL);
        }
}

/* Named FIFOs on ext2 (dir) and on tmpfs (the root). */
static void
test_fifos(void)
{
        test_fifo_at(path("/libctest.fifo"));
        test_fifo_at("/libctest.fifo");
}

/* fsync(), and the modes and owners of files. */
static void
test_file_ops(void)
{
        struct stat st;
        int fd;

        fd = open(path("/libctest.mode"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0);
        if (fd < 0)
                return;
        CHECK(write(fd, "data", 4) == 4);
        CHECK(fsync(fd) == 0);
        CHECK(fchmod(fd, 0600) == 0);
        CHECK(fstat(fd, &st) == 0 && (st.st_mode & 07777) == 0600);
        CHECK(chmod(path("/libctest.mode"), 0640) == 0);
        CHECK(stat(path("/libctest.mode"), &st) == 0 &&
            (st.st_mode & 07777) == 0640);

        /* Owners: -1 keeps an id; ids past 16 bits; lchown on the link. */
        CHECK(chown(path("/libctest.mode"), 100, 200) == 0);
        CHECK(stat(path("/libctest.mode"), &st) == 0 && st.st_uid == 100 &&
            st.st_gid == 200);
        CHECK(chown(path("/libctest.mode"), (uid_t)-1, 70000) == 0);
        CHECK(stat(path("/libctest.mode"), &st) == 0 && st.st_uid == 100 &&
            st.st_gid == 70000);
        CHECK(fchown(fd, 70001, (gid_t)-1) == 0);
        CHECK(fstat(fd, &st) == 0 && st.st_uid == 70001 &&
            st.st_gid == 70000);
        CHECK(symlink("libctest.mode", path("/libctest.link")) == 0);
        CHECK(lchown(path("/libctest.link"), 5, 6) == 0);
        CHECK(lstat(path("/libctest.link"), &st) == 0 && st.st_uid == 5 &&
            st.st_gid == 6);
        CHECK(stat(path("/libctest.link"), &st) == 0 && st.st_uid == 70001);
        CHECK(chown(path("/libctest.link"), 0, 0) == 0);
        CHECK(stat(path("/libctest.mode"), &st) == 0 && st.st_uid == 0 &&
            st.st_gid == 0);
        CHECK(lstat(path("/libctest.link"), &st) == 0 && st.st_uid == 5);
        CHECK(unlink(path("/libctest.link")) == 0);
        CHECK(chown(path("/libctest.none"), 0, 0) == -1 && errno == ENOENT);
        close(fd);
        CHECK(unlink(path("/libctest.mode")) == 0);
}

/*
 * The *at calls, relative to a directory descriptor on ext2 (dir) whose
 * directory is renamed while open, with the current directory on tmpfs.
 */
static void
test_at_calls(void)
{
        struct stat st, st2;
        char buf[16];
        ino_t ino;
        int dfd, fd;

        CHECK(chdir("/") == 0);
        CHECK(mkdir(path("/libctest.at"), 0755) == 0);
        dfd = open(path("/libctest.at"), O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0);
        if (dfd < 0)
                return;
        /* The descriptor keeps naming the directory after a rename. */
        CHECK(rename(path("/libctest.at"), path("/libctest.at2")) == 0);

        fd = openat(dfd, "f", O_RDWR | O_CREAT | O_EXCL, 0644);
        CHECK(fd >= 0);
        CHECK(write(fd, "abc", 3) == 3);
        close(fd);
        CHECK(openat(dfd, "f", O_RDWR | O_CREAT | O_EXCL, 0644) == -1 &&
            errno == EEXIST);
        CHECK(stat(path("/libctest.at2/f"), &st) == 0 && st.st_size == 3);
        CHECK(fstatat(dfd, "f", &st2, 0) == 0 && st2.st_ino == st.st_ino &&
            S_ISREG(st2.st_mode));
        ino = st.st_ino;
        CHECK(faccessat(dfd, "f", R_OK | W_OK, 0) == 0);
        CHECK(faccessat(dfd, "f", F_OK, AT_EACCESS) == 0);
        CHECK(faccessat(dfd, "none", F_OK, 0) == -1 && errno == ENOENT);
        CHECK(fchmodat(dfd, "f", 0600, 0) == 0);
        CHECK(fstatat(dfd, "f", &st, 0) == 0 && (st.st_mode & 07777) == 0600);
        CHECK(fchownat(dfd, "f", 7, 8, 0) == 0);
        CHECK(fstatat(dfd, "f", &st, 0) == 0 && st.st_uid == 7 &&
            st.st_gid == 8);

        CHECK(mkdirat(dfd, "d", 0755) == 0);
        CHECK(fstatat(dfd, "d", &st, 0) == 0 && S_ISDIR(st.st_mode));
        CHECK(mkfifoat(dfd, "p", 0644) == 0);
        CHECK(fstatat(dfd, "p", &st, 0) == 0 && S_ISFIFO(st.st_mode));

        /* Links: the symbolic one is followed only when asked. */
        CHECK(symlinkat("f", dfd, "l") == 0);
        CHECK(readlinkat(dfd, "l", buf, sizeof(buf)) == 1 && buf[0] == 'f');
        CHECK(fstatat(dfd, "l", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(st.st_mode));
        CHECK(fstatat(dfd, "l", &st, 0) == 0 && st.st_ino == ino);
        CHECK(fchownat(dfd, "l", 1, 2, AT_SYMLINK_NOFOLLOW) == 0);
        CHECK(fstatat(dfd, "l", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            st.st_uid == 1);
        CHECK(fstatat(dfd, "f", &st, 0) == 0 && st.st_uid == 7);
        CHECK(fchmodat(dfd, "l", 0644, AT_SYMLINK_NOFOLLOW) == -1 &&
            errno == EOPNOTSUPP);
        CHECK(linkat(dfd, "f", dfd, "d/f2", 0) == 0);
        CHECK(fstatat(dfd, "f", &st, 0) == 0 && st.st_nlink == 2);
        CHECK(linkat(dfd, "l", dfd, "l2", 0) == 0);
        CHECK(fstatat(dfd, "l2", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(st.st_mode));
        CHECK(linkat(dfd, "l", dfd, "f3", AT_SYMLINK_FOLLOW) == 0);
        CHECK(fstatat(dfd, "f3", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            st.st_ino == ino);

        /* renameat, between the descriptor and the current directory. */
        CHECK(renameat(dfd, "d/f2", AT_FDCWD, path("/libctest.at2/g")) == 0);
        CHECK(fstatat(dfd, "g", &st, 0) == 0 && st.st_ino == ino);
        CHECK(renameat(dfd, "g", AT_FDCWD, "libctest.g") == -1 &&
            errno == EXDEV);

        /* An absolute path ignores the descriptor; errors. */
        fd = openat(-1, "/", O_RDONLY);
        CHECK(fd >= 0);
        if (fd >= 0) {
                /* tmpfs too: the current directory is the root. */
                CHECK(mkdirat(fd, "libctest.atd", 0755) == 0);
                CHECK(stat("libctest.atd", &st) == 0 && S_ISDIR(st.st_mode));
                CHECK(unlinkat(AT_FDCWD, "libctest.atd", AT_REMOVEDIR) == 0);
                close(fd);
        }
        fd = openat(dfd, "f", O_RDONLY);
        CHECK(fd >= 0);
        CHECK(openat(fd, "x", O_RDONLY) == -1 && errno == ENOTDIR);
        CHECK(fstatat(fd, "x", &st, 0) == -1 && errno == ENOTDIR);
        close(fd);
        CHECK(openat(999, "f", O_RDONLY) == -1 && errno == EBADF);
        CHECK(mkdirat(999, "x", 0755) == -1 && errno == EBADF);
        CHECK(unlinkat(dfd, "f", AT_REMOVEDIR) == -1 && errno == ENOTDIR);
        CHECK(unlinkat(dfd, "f", 0x40) == -1 && errno == EINVAL);
        CHECK(fstatat(dfd, "f", &st, 0x40) == -1 && errno == EINVAL);
        CHECK(linkat(dfd, "f", dfd, "f4", AT_SYMLINK_NOFOLLOW) == -1 &&
            errno == EINVAL);

        CHECK(unlinkat(dfd, "f", 0) == 0);
        CHECK(unlinkat(dfd, "f3", 0) == 0);
        CHECK(unlinkat(dfd, "g", 0) == 0);
        CHECK(unlinkat(dfd, "l", 0) == 0);
        CHECK(unlinkat(dfd, "l2", 0) == 0);
        CHECK(unlinkat(dfd, "p", 0) == 0);
        CHECK(unlinkat(dfd, "d", AT_REMOVEDIR) == 0);
        CHECK(fstatat(dfd, "d", &st, 0) == -1 && errno == ENOENT);
        close(dfd);
        CHECK(rmdir(path("/libctest.at2")) == 0);
}

/* getcwd() at the root, in a mounted file system, and below it. */
static void
test_cwd(void)
{
        char buf[256], want[256], small[4];

        CHECK(chdir("/") == 0);
        CHECK(getcwd(buf, sizeof(buf)) != NULL && strcmp(buf, "/") == 0);
        CHECK(chdir(dir) == 0);
        CHECK(getcwd(buf, sizeof(buf)) != NULL && strcmp(buf, dir) == 0);
        CHECK(mkdir("libctest.cwd", 0755) == 0);
        CHECK(mkdir("libctest.cwd/sub", 0755) == 0);
        CHECK(chdir("libctest.cwd/sub") == 0);
        snprintf(want, sizeof(want), "%s/libctest.cwd/sub", dir);
        CHECK(getcwd(buf, sizeof(buf)) != NULL && strcmp(buf, want) == 0);
        CHECK(getcwd(small, sizeof(small)) == NULL && errno == ERANGE);
        CHECK(chdir("/") == 0);
        CHECK(rmdir(path("/libctest.cwd/sub")) == 0);
        CHECK(rmdir(path("/libctest.cwd")) == 0);
}

int
main(int argc, char **argv)
{
        dir = argc > 1 ? argv[1] : "";
        test_env(argc, argv);
        test_strings();
        test_heap();
        test_files();
        test_procs();
        test_signals();
        test_misc();
        test_clocks();
        test_pipes();
        test_fifos();
        test_poll();
        test_ppoll();
        test_rlimits();
        test_file_ops();
        test_at_calls();
        test_cwd();
        printf("libctest: %d failures\n", failures);
        return failures;
}
