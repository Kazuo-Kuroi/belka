/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-10
 * SPDX-License-Identifier: MIT
 */

/*
 * A dynamically linked program: started by the program interpreter
 * (mlibc's ld.so.1) with mlibc's libc.so.1, both linked by the illumos
 * link-editor, and with libdt.so.1, found through the run path
 * $ORIGIN/../lib. Checks that the interpreter ran, that calls and data
 * reach the shared libraries (stdio, malloc, errno, environ, and
 * libdt's functions, data, and constructor), that thread-local storage
 * works in the program and in both libraries across threads, that
 * dlopen and dlsym find symbols, that fork and exit handlers work, and
 * that the program's own constructors ran.
 *
 * Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

/* libdt.so.1 (usr/tests/libdt.c). */
extern int dt_counter;
extern __thread int dt_tls;
int dt_constructed(void);
int dt_add(int, int);
int dt_get_counter(void);
int dt_get_tls(void);
void dt_set_tls(int);
int dt_format(char *, size_t, int);

static int failures;

#ifdef BELKA_TEXTREL_TEST
long dt_textrel_signed(void);
uint32_t dt_textrel_unsigned(void);
void dt_textrel_write(const char *);
static const char textrel_readonly[] = "read-only";
#endif

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
        /* The process self-test runs it as /p/bin/dyntest or dyntest-pie. */
        CHECK(getauxval(AT_EXECFN) != 0 &&
            strncmp((const char *)getauxval(AT_EXECFN), "/p/bin/dyntest",
            14) == 0);
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
        CHECK(dt_tls == 3 && dt_get_tls() == 3);
        dt_set_tls(n + 1);
        CHECK(errno == n);
        CHECK(dt_tls == n + 1);
        return (void *)(intptr_t)(tls_counter * 2);
}

/* The program's, libc's (errno), and libdt's thread-local storage. */
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

/* A library of the program's own, found through $ORIGIN. */
static void
test_libdt(void)
{
        char buf[32];
        void *h;
        int (*add)(int, int);

        CHECK(dt_constructed());
        CHECK(dt_add(40, 2) == 42);
        CHECK(dt_format(buf, sizeof(buf), 9) == 7 &&
            strcmp(buf, "libdt 9") == 0);
        /* The program and the library see one dt_counter. */
        CHECK(dt_counter == 7);
        dt_counter = 11;
        CHECK(dt_get_counter() == 11);
        h = dlopen("libdt.so.1", RTLD_NOW);
        CHECK(h != NULL);
        if (h == NULL)
                return;
        add = (int (*)(int, int))dlsym(h, "dt_add");
        CHECK(add != NULL && add(1, 2) == 3);
}

static void
test_dl(void)
{
        void *h = dlopen("libc.so.1", RTLD_NOW);
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

#ifdef BELKA_TEXTREL_TEST
/* Both narrow relocation widths and the restoration of read-only mappings. */
static void
test_text_relocations(void)
{
        pid_t child, waited;
        int status;

        CHECK(dt_textrel_signed() == -1);
        CHECK(dt_textrel_unsigned() == UINT32_MAX);
        child = fork();
        CHECK(child >= 0);
        if (child == 0) {
                dt_textrel_write(textrel_readonly);
                _exit(99);
        }
        if (child > 0) {
                do {
                        waited = waitpid(child, &status, 0);
                } while (waited == -1 && errno == EINTR);
                CHECK(waited == child);
                if (waited == child)
                        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
        }
}
#endif

int
main(int argc, char **argv)
{
        CHECK(argc >= 1 && argv[0] != NULL);
        test_interp();
        test_libc();
        test_tls();
        test_libdt();
        test_dl();
#ifdef BELKA_TEXTREL_TEST
        test_text_relocations();
#endif
        test_fork();
        CHECK(atexit(at_exit) == 0);
        printf("dyntest: %d failures\n", failures);
        return failures;
}
