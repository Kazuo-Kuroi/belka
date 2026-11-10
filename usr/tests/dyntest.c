/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-10
 * SPDX-License-Identifier: MIT
 */

/*
 * A dynamically linked program: started by the program interpreter
 * (mlibc's ld.so) with mlibc's libc.so, both linked by the illumos
 * link-editor. Checks that the interpreter ran, that calls and data
 * reach the shared C library (stdio, malloc, errno, environ), that
 * thread-local storage works in the program and in the library across
 * threads, that dlopen and dlsym find symbols, that fork and exit
 * handlers work, and that the program's own constructors ran.
 *
 * Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("dyntest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

static int constructed;

__attribute__((constructor)) static void
construct(void)
{
        constructed = 1;
}

/* The interpreter is loaded apart from the program, and says where. */
static void
test_interp(void)
{
        CHECK(constructed);
        CHECK(getauxval(AT_BASE) != 0);
        CHECK(getauxval(AT_ENTRY) != 0);
        CHECK(getauxval(AT_PHDR) != 0);
}

static void
test_libc(void)
{
        char buf[64];
        char *p;

        snprintf(buf, sizeof(buf), "%s-%d-%x", "dyn", 42, 0xbe1);
        CHECK(strcmp(buf, "dyn-42-be1") == 0);
        p = malloc(100000);
        CHECK(p != NULL);
        if (p != NULL) {
                memset(p, 0xa5, 100000);
                CHECK((unsigned char)p[99999] == 0xa5);
                free(p);
        }
        CHECK(strtol("-1234", NULL, 10) == -1234);
        errno = 0;
        CHECK(open("/p/no-such-file", O_RDONLY) == -1);
        CHECK(errno == ENOENT);
        CHECK(setenv("DYNTEST", "yes", 1) == 0);
        CHECK(getenv("DYNTEST") != NULL &&
            strcmp(getenv("DYNTEST"), "yes") == 0);
}

static __thread int tls_counter = 5;

static void *
tls_thread(void *arg)
{
        int n = (int)(intptr_t)arg;

        CHECK(tls_counter == 5);
        tls_counter = n;
        errno = n;
        sched_yield();
        CHECK(tls_counter == n);
        CHECK(errno == n);
        return (void *)(intptr_t)(tls_counter * 2);
}

/* The program's and libc's (errno) thread-local storage, per thread. */
static void
test_tls(void)
{
        pthread_t t[4];
        void *ret;

        tls_counter = 1;
        for (int i = 0; i < 4; i++)
                CHECK(pthread_create(&t[i], NULL, tls_thread,
                    (void *)(intptr_t)(100 + i)) == 0);
        for (int i = 0; i < 4; i++) {
                CHECK(pthread_join(t[i], &ret) == 0);
                CHECK((intptr_t)ret == (100 + i) * 2);
        }
        CHECK(tls_counter == 1);
}

static void
test_dl(void)
{
        void *h = dlopen("libc.so", RTLD_NOW);
        size_t (*len)(const char *);

        CHECK(h != NULL);
        if (h == NULL)
                return;
        len = (size_t (*)(const char *))dlsym(h, "strlen");
        CHECK(len != NULL);
        if (len != NULL)
                CHECK(len("shared") == 6);
        CHECK(dlsym(h, "no_such_symbol_here") == NULL);
}

static void
test_fork(void)
{
        int status = 0;
        pid_t pid = fork();

        if (pid == 0) {
                printf("dyntest: child\n");
                _exit(tls_counter + 6);
        }
        CHECK(pid > 0);
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
}

static void
at_exit(void)
{
        printf("dyntest: exit handler\n");
}

int
main(int argc, char **argv)
{
        CHECK(argc >= 1 && argv[0] != NULL);
        test_interp();
        test_libc();
        test_tls();
        test_dl();
        test_fork();
        CHECK(atexit(at_exit) == 0);
        printf("dyntest: %d failures\n", failures);
        return failures;
}
