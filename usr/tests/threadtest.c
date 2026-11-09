/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 */

/*
 * POSIX threads over LWPs, built against mlibc: creation, join and
 * return values, detached threads, mutexes under contention, condition
 * variables (with timeouts), semaphores, read-write locks, barriers,
 * once, thread-specific data and its destructors, per-thread errno,
 * signals to one thread, cancellation, sleeping, and a process that
 * ends while its threads are blocked.
 *
 * Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("threadtest: %d: %s\n", __LINE__, #expr); \
                        failures++;                                     \
                }                                                       \
        } while (0)

#define NTHREADS        8
#define ROUNDS          20000

static long
now_ms(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* An absolute CLOCK_REALTIME time ms from now. */
static struct timespec
deadline(long ms)
{
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
        }
        return ts;
}

/* --- Create and join, with return values and identities. --- */

static pthread_t seen_self[NTHREADS];

static void *
returner(void *arg)
{
        int slot = (int)(intptr_t)arg;

        seen_self[slot] = pthread_self();
        errno = 1000 + slot;            /* each thread has its own */
        return (void *)(intptr_t)(slot * 10 + errno - 1000);
}

static void
test_create_join(void)
{
        pthread_t t[NTHREADS];
        void *ret;
        int ok = 1;

        errno = 0;
        for (int i = 0; i < NTHREADS; i++)
                CHECK(pthread_create(&t[i], NULL, returner,
                    (void *)(intptr_t)i) == 0);
        for (int i = 0; i < NTHREADS; i++) {
                CHECK(pthread_join(t[i], &ret) == 0);
                if ((intptr_t)ret != i * 11)
                        ok = 0;
                if (!pthread_equal(seen_self[i], t[i]))
                        ok = 0;
        }
        CHECK(ok);
        CHECK(errno == 0);
        CHECK(!pthread_equal(pthread_self(), t[0]));
}

/* --- Detached threads. --- */

static volatile int detached_ran;

static void *
detached(void *arg)
{
        (void)arg;
        __atomic_fetch_add(&detached_ran, 1, __ATOMIC_RELAXED);
        return NULL;
}

static void
test_detached(void)
{
        pthread_attr_t attr;
        pthread_t t;
        long t0 = now_ms();

        CHECK(pthread_attr_init(&attr) == 0);
        CHECK(pthread_attr_setdetachstate(&attr,
            PTHREAD_CREATE_DETACHED) == 0);
        for (int i = 0; i < 4; i++)
                CHECK(pthread_create(&t, &attr, detached, NULL) == 0);
        pthread_attr_destroy(&attr);
        while (__atomic_load_n(&detached_ran, __ATOMIC_RELAXED) < 4 &&
            now_ms() - t0 < 5000)
                usleep(1000);
        CHECK(detached_ran == 4);
}

/* --- Mutexes under contention. --- */

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long shared_count;

static void *
mutex_worker(void *arg)
{
        (void)arg;
        for (int i = 0; i < ROUNDS; i++) {
                pthread_mutex_lock(&mtx);
                shared_count = shared_count + 1;
                pthread_mutex_unlock(&mtx);
        }
        return NULL;
}

static void *
timedlock_worker(void *arg)
{
        struct timespec ts = deadline(50);

        (void)arg;
        return (void *)(intptr_t)pthread_mutex_timedlock(&mtx, &ts);
}

static void
test_mutex(void)
{
        pthread_t t[NTHREADS], tl;
        pthread_mutexattr_t ma;
        pthread_mutex_t rec;
        void *ret;

        for (int i = 0; i < NTHREADS; i++)
                CHECK(pthread_create(&t[i], NULL, mutex_worker, NULL) == 0);
        for (int i = 0; i < NTHREADS; i++)
                CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(shared_count == (long)NTHREADS * ROUNDS);

        CHECK(pthread_mutex_lock(&mtx) == 0);
        CHECK(pthread_mutex_trylock(&mtx) == EBUSY);
        CHECK(pthread_create(&tl, NULL, timedlock_worker, NULL) == 0);
        CHECK(pthread_join(tl, &ret) == 0 && (intptr_t)ret == ETIMEDOUT);
        CHECK(pthread_mutex_unlock(&mtx) == 0);

        pthread_mutexattr_init(&ma);
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
        CHECK(pthread_mutex_init(&rec, &ma) == 0);
        CHECK(pthread_mutex_lock(&rec) == 0 && pthread_mutex_lock(&rec) == 0);
        CHECK(pthread_mutex_unlock(&rec) == 0 &&
            pthread_mutex_unlock(&rec) == 0);
        pthread_mutex_destroy(&rec);
        pthread_mutexattr_destroy(&ma);
}

/* --- Condition variables: a bounded queue, and timeouts. --- */

#define QSIZE   4
#define ITEMS   2000

static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
static int queue[QSIZE], qhead, qcount;

static void *
producer(void *arg)
{
        (void)arg;
        for (int i = 1; i <= ITEMS; i++) {
                pthread_mutex_lock(&qm);
                while (qcount == QSIZE)
                        pthread_cond_wait(&not_full, &qm);
                queue[(qhead + qcount++) % QSIZE] = i;
                pthread_cond_signal(&not_empty);
                pthread_mutex_unlock(&qm);
        }
        return NULL;
}

static void
test_cond(void)
{
        pthread_t t[2];
        long sum = 0, t0;
        struct timespec ts;
        int r;

        for (int i = 0; i < 2; i++)
                CHECK(pthread_create(&t[i], NULL, producer, NULL) == 0);
        for (int n = 0; n < 2 * ITEMS; n++) {
                pthread_mutex_lock(&qm);
                while (qcount == 0)
                        pthread_cond_wait(&not_empty, &qm);
                sum += queue[qhead];
                qhead = (qhead + 1) % QSIZE;
                qcount--;
                pthread_cond_broadcast(&not_full);
                pthread_mutex_unlock(&qm);
        }
        for (int i = 0; i < 2; i++)
                CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(sum == 2L * ITEMS * (ITEMS + 1) / 2);

        t0 = now_ms();
        ts = deadline(100);
        pthread_mutex_lock(&qm);
        r = pthread_cond_timedwait(&not_empty, &qm, &ts);
        pthread_mutex_unlock(&qm);
        CHECK(r == ETIMEDOUT);
        CHECK(now_ms() - t0 >= 90 && now_ms() - t0 < 2000);
}

/* --- Semaphores. --- */

static sem_t sem;
static volatile int sem_done;

static void *
sem_poster(void *arg)
{
        (void)arg;
        for (int i = 0; i < 1000; i++)
                sem_post(&sem);
        return NULL;
}

static void *
sem_taker(void *arg)
{
        (void)arg;
        for (int i = 0; i < 1000; i++)
                sem_wait(&sem);
        __atomic_fetch_add(&sem_done, 1, __ATOMIC_RELAXED);
        return NULL;
}

static void
test_sem(void)
{
        pthread_t t[4];
        struct timespec ts;
        int v = -1;

        CHECK(sem_init(&sem, 0, 0) == 0);
        CHECK(pthread_create(&t[0], NULL, sem_taker, NULL) == 0);
        CHECK(pthread_create(&t[1], NULL, sem_taker, NULL) == 0);
        CHECK(pthread_create(&t[2], NULL, sem_poster, NULL) == 0);
        CHECK(pthread_create(&t[3], NULL, sem_poster, NULL) == 0);
        for (int i = 0; i < 4; i++)
                CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(sem_done == 2);
        CHECK(sem_getvalue(&sem, &v) == 0 && v == 0);
        CHECK(sem_trywait(&sem) == -1 && errno == EAGAIN);
        ts = deadline(50);
        CHECK(sem_timedwait(&sem, &ts) == -1 && errno == ETIMEDOUT);
        sem_destroy(&sem);
}

/* --- Read-write locks and barriers. --- */

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static long rw_value;
static volatile int rw_bad;
static pthread_barrier_t bar;
static volatile int bar_serial;

static void *
rw_worker(void *arg)
{
        int writer = (int)(intptr_t)arg & 1;

        for (int i = 0; i < 2000; i++) {
                if (writer) {
                        pthread_rwlock_wrlock(&rw);
                        rw_value++;
                        rw_value++;
                        pthread_rwlock_unlock(&rw);
                } else {
                        pthread_rwlock_rdlock(&rw);
                        if (rw_value % 2 != 0)
                                rw_bad = 1;
                        pthread_rwlock_unlock(&rw);
                }
        }
        if (pthread_barrier_wait(&bar) == PTHREAD_BARRIER_SERIAL_THREAD)
                __atomic_fetch_add(&bar_serial, 1, __ATOMIC_RELAXED);
        return NULL;
}

static void
test_rwlock_barrier(void)
{
        pthread_t t[6];

        CHECK(pthread_barrier_init(&bar, NULL, 6) == 0);
        for (int i = 0; i < 6; i++)
                CHECK(pthread_create(&t[i], NULL, rw_worker,
                    (void *)(intptr_t)i) == 0);
        for (int i = 0; i < 6; i++)
                CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(!rw_bad && rw_value == 3 * 2000 * 2);
        CHECK(bar_serial == 1);
        pthread_barrier_destroy(&bar);
}

/* --- Once, and thread-specific data with destructors. --- */

static pthread_once_t once = PTHREAD_ONCE_INIT;
static volatile int once_count;
static pthread_key_t key;
static volatile int dtor_count;

static void
once_fn(void)
{
        once_count++;
}

static void
key_dtor(void *v)
{
        if (v != NULL)
                __atomic_fetch_add(&dtor_count, 1, __ATOMIC_RELAXED);
}

static void *
tsd_worker(void *arg)
{
        int ok;

        pthread_once(&once, once_fn);
        ok = pthread_getspecific(key) == NULL;
        pthread_setspecific(key, arg);
        ok = ok && pthread_getspecific(key) == arg;
        return (void *)(intptr_t)ok;
}

static void
test_once_tsd(void)
{
        pthread_t t[4];
        void *ret;
        int ok = 1;

        CHECK(pthread_key_create(&key, key_dtor) == 0);
        CHECK(pthread_setspecific(key, (void *)1) == 0);
        for (int i = 0; i < 4; i++)
                CHECK(pthread_create(&t[i], NULL, tsd_worker,
                    (void *)(intptr_t)(i + 2)) == 0);
        for (int i = 0; i < 4; i++) {
                CHECK(pthread_join(t[i], &ret) == 0);
                if (ret == NULL)
                        ok = 0;
        }
        CHECK(ok);
        CHECK(once_count == 1);
        CHECK(dtor_count == 4);
        CHECK(pthread_getspecific(key) == (void *)1);
}

/* --- A signal to one thread, cancellation, and sleeping. --- */

static volatile pthread_t usr1_thread;
static volatile int usr1_count;
static volatile int sig_stop;

static void
on_usr1(int sig)
{
        (void)sig;
        usr1_thread = pthread_self();
        usr1_count++;
}

static void *
sig_target(void *arg)
{
        (void)arg;
        while (!sig_stop)
                usleep(1000);
        return NULL;
}

static void *
cancel_target(void *arg)
{
        (void)arg;
        for (;;) {
                pthread_testcancel();
                usleep(1000);
        }
        return NULL;
}

static void
test_signal_cancel_sleep(void)
{
        struct sigaction sa;
        struct timespec req = { 0, 60000000 }, rem;
        pthread_t t;
        void *ret = NULL;
        long t0;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
        sig_stop = 0;
        CHECK(pthread_create(&t, NULL, sig_target, NULL) == 0);
        CHECK(pthread_kill(t, SIGUSR1) == 0);
        t0 = now_ms();
        while (usr1_count == 0 && now_ms() - t0 < 5000)
                usleep(1000);
        CHECK(usr1_count == 1 && pthread_equal(usr1_thread, t));
        sig_stop = 1;
        CHECK(pthread_join(t, NULL) == 0);

        CHECK(pthread_create(&t, NULL, cancel_target, NULL) == 0);
        CHECK(pthread_cancel(t) == 0);
        CHECK(pthread_join(t, &ret) == 0 && ret == PTHREAD_CANCELED);

        t0 = now_ms();
        CHECK(nanosleep(&req, &rem) == 0);
        CHECK(now_ms() - t0 >= 50 && now_ms() - t0 < 2000);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGUSR1, &sa, NULL);
}

/* --- A process ends while its threads wait. --- */

static void *
blocker(void *arg)
{
        (void)arg;
        pthread_mutex_lock(&mtx);       /* held by the main thread */
        return NULL;
}

static void
test_exit_with_threads(void)
{
        int status = -1;
        pid_t pid = fork();

        if (pid == 0) {
                pthread_t t[3];

                pthread_mutex_lock(&mtx);
                for (int i = 0; i < 3; i++)
                        pthread_create(&t[i], NULL, blocker, NULL);
                usleep(20000);
                exit(3);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3);
}

int
main(void)
{
        test_create_join();
        test_detached();
        test_mutex();
        test_cond();
        test_sem();
        test_rwlock_barrier();
        test_once_tsd();
        test_signal_cancel_sleep();
        test_exit_with_threads();
        printf("threadtest: %d failures\n", failures);
        return failures;
}
