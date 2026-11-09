/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 */

/*
 * The C library test: a program built against mlibc, checking that
 * the library's view of the system matches the kernel's: start-up and
 * the environment, the heap, stdio on files and directories, errno,
 * processes and wait statuses, signals and handlers, time, and the
 * thread pointer.
 *
 * usage: libctest dir, where dir holds bin/ and is writable. Exits with
 * the number of failed checks.
 */

#define _DEFAULT_SOURCE         /* POSIX, XSI, and the BSD and SVID extras */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
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
        printf("libctest: %d failures\n", failures);
        return failures;
}
