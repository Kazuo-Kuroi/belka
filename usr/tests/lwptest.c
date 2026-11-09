/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * LWPs from the point of view of a program: create, exit, and wait;
 * shared memory and atomics across LWPs; LWP semaphores; address waits
 * and wakes; suspend and continue; signals to one LWP and to the
 * process; sigwait; exit, exec, and faults that end every LWP; fork1
 * against forkall; concurrent faults and mappings; job control of a
 * multi-LWP process; and each LWP's own floating-point state and
 * private pointer.
 *
 * Exits with the number of failed checks.
 */

#include "lib/libt.h"

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("lwptest: %d: %s\n", __LINE__, #expr);   \
                        failures++;                                     \
                }                                                       \
        } while (0)

#define STACK_SIZE      (64 * 1024)
#define NLWP            8

static void *
new_stack(void)
{
        void *p = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);

        return p == MAP_FAILED ? NULL : p;
}

/* Start fn(arg) in a new LWP; 0 or an error number. */
static int
spawn_lwp(void (*fn)(void *), void *arg, unsigned long flags, lwpid_t *idp)
{
        ucontext_t uc;
        void *stack = new_stack();
        int error;

        if (stack == NULL)
                return ENOMEM;
        if ((error = _lwp_makecontext(&uc, fn, arg, stack, STACK_SIZE)) != 0)
                return error;
        return _lwp_create(&uc, flags, idp);
}

static void
spin_ticks(long n)
{
        for (volatile long i = 0; i < n; i++)
                continue;
}

/* --- Create, count together, and wait. --- */

static volatile uint64_t counter;
static volatile lwpid_t seen_ids[NLWP];

static void
adder(void *arg)
{
        int slot = (int)(intptr_t)arg;

        seen_ids[slot] = _lwp_self();
        for (int i = 0; i < 100000; i++)
                __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED);
}

static void
test_create_wait(void)
{
        lwpid_t ids[NLWP], gone;
        int ok = 1;

        counter = 0;
        for (int i = 0; i < NLWP; i++)
                CHECK(spawn_lwp(adder, (void *)(intptr_t)i, 0, &ids[i]) == 0);
        for (int i = 0; i < NLWP; i++) {
                ok &= _lwp_wait(ids[i], &gone) == 0 && gone == ids[i];
        }
        CHECK(ok);
        CHECK(counter == NLWP * 100000);
        ok = 1;
        for (int i = 0; i < NLWP; i++) {
                ok &= seen_ids[i] == ids[i] && ids[i] != _lwp_self();
                for (int j = 0; j < i; j++)
                        ok &= ids[i] != ids[j];
        }
        CHECK(ok);

        /* Any LWP; then none left to wait for. */
        CHECK(spawn_lwp(adder, (void *)0, 0, &ids[0]) == 0);
        CHECK(_lwp_wait(0, &gone) == 0 && gone == ids[0]);
        CHECK(_lwp_wait(0, &gone) == ESRCH);
        CHECK(_lwp_wait(_lwp_self(), &gone) == EDEADLK);
        CHECK(_lwp_wait(12345, &gone) == ESRCH);

        /* A detached LWP is never waited for. */
        CHECK(spawn_lwp(adder, (void *)0, LWP_DETACHED, &ids[0]) == 0);
        CHECK(_lwp_wait(ids[0], &gone) == ESRCH);
}

/* --- Semaphores: a producer and a consumer. --- */

#define ITEMS           2000

static _lwp_sema_t sem_full, sem_empty;
static volatile int ring[8], ring_in, consumed_sum;

static void
producer(void *arg)
{
        (void)arg;
        for (int i = 1; i <= ITEMS; i++) {
                _lwp_sema_wait(&sem_empty);
                ring[ring_in++ % 8] = i;
                _lwp_sema_post(&sem_full);
        }
}

static void
test_sema(void)
{
        lwpid_t id, gone;
        int out = 0, sum = 0;
        _lwp_sema_t z;

        _lwp_sema_init(&z, 0);
        CHECK(_lwp_sema_trywait(&z) == EBUSY);
        CHECK(_lwp_sema_post(&z) == 0 && _lwp_sema_trywait(&z) == 0);
        CHECK(_lwp_sema_wait((_lwp_sema_t *)8) == EFAULT);

        _lwp_sema_init(&sem_full, 0);
        _lwp_sema_init(&sem_empty, 8);
        ring_in = 0;
        CHECK(spawn_lwp(producer, NULL, 0, &id) == 0);
        for (int i = 0; i < ITEMS; i++) {
                _lwp_sema_wait(&sem_full);
                sum += ring[out++ % 8];
                _lwp_sema_post(&sem_empty);
        }
        consumed_sum = sum;
        CHECK(sum == ITEMS * (ITEMS + 1) / 2);
        CHECK(_lwp_wait(id, &gone) == 0);
}

/* --- Address waits: a lock built on them, wake counts, timeouts. --- */

static volatile int32_t lock_word;     /* 0 free, 1 held, 2 held with waiters */
static volatile uint64_t locked_count;

static void
word_lock(void)
{
        int32_t c = 0;

        if (__atomic_compare_exchange_n(&lock_word, &c, 1, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
        if (c != 2)
                c = __atomic_exchange_n(&lock_word, 2, __ATOMIC_ACQUIRE);
        while (c != 0) {
                (void)_lwp_addr_wait(&lock_word, 2, NULL);
                c = __atomic_exchange_n(&lock_word, 2, __ATOMIC_ACQUIRE);
        }
}

static void
word_unlock(void)
{
        if (__atomic_exchange_n(&lock_word, 0, __ATOMIC_RELEASE) == 2)
                (void)_lwp_addr_wake(&lock_word, 1, NULL);
}

static void
locker(void *arg)
{
        (void)arg;
        for (int i = 0; i < 20000; i++) {
                word_lock();
                locked_count = locked_count + 1;        /* not atomic */
                word_unlock();
        }
}

static volatile int32_t gate;
static volatile int gate_passed, gate_result[3];

static void
gate_waiter(void *arg)
{
        int slot = (int)(intptr_t)arg;

        gate_result[slot] = _lwp_addr_wait(&gate, 0, NULL);
        __atomic_fetch_add(&gate_passed, 1, __ATOMIC_RELAXED);
}

static volatile int32_t intr_word;
static volatile int intr_got, intr_handled;

static void
on_usr1(int sig)
{
        (void)sig;
        intr_handled++;
}

static void
intr_waiter(void *arg)
{
        int64_t ts[2] = { 10, 0 };

        (void)arg;
        while (!intr_got) {
                if (_lwp_addr_wait(&intr_word, 0, ts) == EINTR)
                        intr_got = 1;
        }
}

static void
test_addr(void)
{
        int64_t zero[2] = { 0, 0 }, short_ts[2] = { 0, 30000000 };
        int64_t bad_ts[2] = { 0, 1000000000 };
        volatile int32_t w = 5;
        lwpid_t ids[NLWP], gone;
        struct sigaction sa;
        int woken = -1, total = 0;
        long t0;

        CHECK(_lwp_addr_wait(&w, 4, NULL) == EAGAIN);
        CHECK(_lwp_addr_wait(&w, 5, zero) == ETIMEDOUT);
        CHECK(_lwp_addr_wait(&w, 5, bad_ts) == EINVAL);
        CHECK(_lwp_addr_wait((volatile int32_t *)((char *)&w + 1), 5,
            NULL) == EINVAL);
        CHECK(_lwp_addr_wait((volatile int32_t *)8, 0, NULL) == EFAULT);
        t0 = time();
        CHECK(_lwp_addr_wait(&w, 5, short_ts) == ETIMEDOUT);
        CHECK(time() - t0 <= 1);
        CHECK(_lwp_addr_wake(&w, 0, &woken) == 0 && woken == 0);

        /* Mutual exclusion through a lock that sleeps when contended. */
        lock_word = 0;
        locked_count = 0;
        for (int i = 0; i < 4; i++)
                CHECK(spawn_lwp(locker, NULL, 0, &ids[i]) == 0);
        locker(NULL);
        for (int i = 0; i < 4; i++)
                CHECK(_lwp_wait(ids[i], &gone) == 0);
        CHECK(locked_count == 5 * 20000);
        CHECK(lock_word == 0);

        /* One wake per waiter: poll with n = 1 until three were woken. */
        gate = 0;
        gate_passed = 0;
        for (int i = 0; i < 3; i++)
                CHECK(spawn_lwp(gate_waiter, (void *)(intptr_t)i, 0,
                    &ids[i]) == 0);
        for (long tries = 0; total < 3 && tries < 2000000; tries++) {
                if (_lwp_addr_wake(&gate, 1, &woken) == 0) {
                        CHECK(woken == 0 || woken == 1);
                        total += woken;
                }
        }
        CHECK(total == 3);
        for (int i = 0; i < 3; i++)
                CHECK(_lwp_wait(ids[i], &gone) == 0);
        CHECK(gate_passed == 3);
        CHECK(gate_result[0] == 0 && gate_result[1] == 0 &&
            gate_result[2] == 0);

        /* A handler interrupts the wait. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
        intr_got = 0;
        CHECK(spawn_lwp(intr_waiter, NULL, 0, &ids[0]) == 0);
        /* Until it is in the wait when a signal comes; give it time under load. */
        t0 = time();
        while (!intr_got && time() - t0 < 5) {
                _lwp_kill(ids[0], SIGUSR1);
                spin_ticks(200000);
        }
        CHECK(intr_got && intr_handled > 0);
        intr_got = 1;
        CHECK(_lwp_wait(ids[0], &gone) == 0);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGUSR1, &sa, NULL);
}

/* --- Suspend and continue. --- */

static volatile uint64_t spins;
static volatile int stop_spinning;

static void
spinner(void *arg)
{
        (void)arg;
        while (!stop_spinning)
                __atomic_fetch_add(&spins, 1, __ATOMIC_RELAXED);
}

static void
test_suspend(void)
{
        lwpid_t id, gone;
        uint64_t a, b;

        spins = 0;
        stop_spinning = 0;
        CHECK(spawn_lwp(spinner, NULL, LWP_SUSPENDED, &id) == 0);
        spin_ticks(5000000);
        CHECK(spins == 0);                      /* created suspended */
        CHECK(_lwp_continue(id) == 0);
        while (spins == 0)
                continue;
        CHECK(_lwp_suspend(id) == 0);
        a = spins;
        spin_ticks(5000000);
        b = spins;
        CHECK(a == b);                          /* not running */
        CHECK(_lwp_continue(id) == 0);
        while (spins == b)
                continue;
        stop_spinning = 1;
        CHECK(_lwp_wait(id, &gone) == 0);
        CHECK(_lwp_suspend(9999) == ESRCH);
}

/* --- Signals: to one LWP, and to the process. --- */

static volatile lwpid_t handled_in;

static void
note_lwp(int sig)
{
        (void)sig;
        handled_in = _lwp_self();
}

static void
blocker(void *arg)
{
        sigset_t set;

        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        sigprocmask(SIG_UNBLOCK, &set, NULL);   /* the one that takes it */
        while (!stop_spinning)
                spin_ticks(1000);
        (void)arg;
}

static void
test_signals(void)
{
        struct sigaction sa;
        sigset_t set;
        lwpid_t id, gone;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = note_lwp;
        CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
        CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);

        /* _lwp_kill reaches the LWP named. */
        stop_spinning = 0;
        handled_in = 0;
        CHECK(spawn_lwp(spinner, NULL, 0, &id) == 0);
        CHECK(_lwp_kill(id, SIGUSR1) == 0);
        while (handled_in == 0)
                continue;
        CHECK(handled_in == id);
        stop_spinning = 1;
        CHECK(_lwp_wait(id, &gone) == 0);
        CHECK(_lwp_kill(id, SIGUSR1) == ESRCH);

        /* A process signal goes to the LWP that does not hold it. */
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
        stop_spinning = 0;
        handled_in = 0;
        CHECK(spawn_lwp(blocker, NULL, 0, &id) == 0);
        spin_ticks(2000000);
        CHECK(kill(getpid(), SIGUSR2) == 0);
        while (handled_in == 0)
                continue;
        CHECK(handled_in == id);
        stop_spinning = 1;
        CHECK(_lwp_wait(id, &gone) == 0);
        CHECK(sigprocmask(SIG_UNBLOCK, &set, NULL) == 0);
}

/* --- sigwait in one LWP. --- */

static volatile int waited_sig;

static void
waiter(void *arg)
{
        sigset_t set;

        (void)arg;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        waited_sig = sigtimedwait(&set, NULL, NULL);
}

static void
test_sigwait(void)
{
        sigset_t set;
        lwpid_t id, gone;
        int64_t ts[2] = { 0, 200000000 };
        long t0;

        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
        waited_sig = 0;
        CHECK(spawn_lwp(waiter, NULL, 0, &id) == 0);
        spin_ticks(5000000);
        CHECK(kill(getpid(), SIGUSR1) == 0);
        CHECK(_lwp_wait(id, &gone) == 0);
        CHECK(waited_sig == SIGUSR1);
        t0 = time();
        CHECK(sigtimedwait(&set, NULL, ts) == -1 && errno == EAGAIN);
        CHECK(time() - t0 <= 2);
        ts[1] = 0;
        CHECK(sigtimedwait(&set, NULL, ts) == -1 && errno == EAGAIN);
        CHECK(kill(getpid(), SIGUSR1) == 0);
        CHECK(sigtimedwait(&set, NULL, ts) == SIGUSR1);
        CHECK(sigprocmask(SIG_UNBLOCK, &set, NULL) == 0);
}

/* --- Whole-process endings, in children. --- */

static void
forever(void *arg)
{
        (void)arg;
        for (;;)
                spin_ticks(1000);
}

static void
null_deref(void *arg)
{
        (void)arg;
        spin_ticks(1000000);
        *(volatile int *)0 = 1;
}

static void
exit_four(void *arg)
{
        (void)arg;
        spin_ticks(1000000);
        _exit(4);
}

static void
exec_argtest(void *arg)
{
        char *argv[] = { "argtest", "one", "two words", NULL };
        char *envp[] = { "FOO=bar", "X=1", NULL };

        execve((const char *)arg, argv, envp);
        _exit(55);
}

static void
lwp_exit_later(void *arg)
{
        (void)arg;
        spin_ticks(3000000);
        _lwp_exit();
}

static int
child_status(void (*setup)(const char *), const char *arg)
{
        int status = -1;
        pid_t pid = fork1();

        if (pid == 0) {
                setup(arg);
                _exit(99);
        }
        waitpid(pid, &status, 0);
        return status;
}

static void
case_main_exits(const char *arg)
{
        (void)arg;
        for (int i = 0; i < 4; i++)
                spawn_lwp(forever, NULL, 0, NULL);
        spin_ticks(2000000);
        _exit(3);
}

static void
case_lwp_exits(const char *arg)
{
        (void)arg;
        spawn_lwp(forever, NULL, 0, NULL);
        spawn_lwp(exit_four, NULL, 0, NULL);
        for (;;)
                spin_ticks(1000);
}

static void
case_fault(const char *arg)
{
        (void)arg;
        spawn_lwp(forever, NULL, 0, NULL);
        spawn_lwp(null_deref, NULL, 0, NULL);
        for (;;)
                spin_ticks(1000);
}

static void
case_last_lwp(const char *arg)
{
        (void)arg;
        spawn_lwp(lwp_exit_later, NULL, 0, NULL);
        _lwp_exit();
}

static void
case_exec(const char *arg)
{
        for (int i = 0; i < 3; i++)
                spawn_lwp(forever, NULL, 0, NULL);
        spawn_lwp(exec_argtest, (void *)arg, 0, NULL);
        for (;;)
                spin_ticks(1000);
}

static void
test_endings(const char *argtest)
{
        int s;

        s = child_status(case_main_exits, NULL);
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 3);
        s = child_status(case_lwp_exits, NULL);
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 4);
        s = child_status(case_fault, NULL);
        CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGSEGV);
        s = child_status(case_last_lwp, NULL);
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0);
        s = child_status(case_exec, argtest);
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 7);
}

/* --- fork1 copies the caller only; forkall copies every LWP. --- */

static void
test_forks(void)
{
        lwpid_t a, b, gone;
        pid_t pid;
        int s;

        stop_spinning = 0;
        CHECK(spawn_lwp(spinner, NULL, 0, &a) == 0);
        CHECK(spawn_lwp(spinner, NULL, 0, &b) == 0);
        if ((pid = fork1()) == 0)
                _exit(_lwp_kill(a, 0) == ESRCH && _lwp_kill(b, 0) == ESRCH &&
                    _lwp_wait(0, &gone) == ESRCH ? 0 : 1);
        CHECK(waitpid(pid, &s, 0) == pid && WIFEXITED(s) &&
            WEXITSTATUS(s) == 0);
        if ((pid = forkall()) == 0) {
                uint64_t before = spins;
                long until = time() + 10;

                /*
                 * The copies run on in the child, under the same ids;
                 * on one processor, perhaps only once this LWP's time
                 * slice is over.
                 */
                while (spins == before && time() < until)
                        continue;
                _exit(_lwp_kill(a, 0) == 0 && _lwp_kill(b, 0) == 0 &&
                    spins != before ? 0 : 1);
        }
        CHECK(waitpid(pid, &s, 0) == pid && WIFEXITED(s) &&
            WEXITSTATUS(s) == 0);
        stop_spinning = 1;
        CHECK(_lwp_wait(a, &gone) == 0 && _lwp_wait(b, &gone) == 0);
}

/* --- Concurrent faults and mappings. --- */

#define TOUCH_PAGES     256

static char *volatile touch_area;
static volatile int touch_bad;

static void
toucher(void *arg)
{
        int me = (int)(intptr_t)arg;

        for (int i = 0; i < TOUCH_PAGES; i++) {
                volatile char *p = touch_area + (size_t)i * 4096 + me;

                *p = (char)me;
                if (*p != (char)me)
                        touch_bad = 1;
        }
        for (int round = 0; round < 50; round++) {
                char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);

                if (m == MAP_FAILED) {
                        touch_bad = 1;
                        break;
                }
                m[0] = m[8191] = (char)me;
                spin_ticks(1000);
                if (m[0] != (char)me || m[8191] != (char)me)
                        touch_bad = 1;
                munmap(m, 8192);
        }
}

static void
test_concurrent_vm(void)
{
        lwpid_t ids[NLWP], gone;
        int ok = 1;

        touch_area = mmap(NULL, TOUCH_PAGES * 4096, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(touch_area != MAP_FAILED);
        touch_bad = 0;
        for (int i = 0; i < NLWP; i++)
                CHECK(spawn_lwp(toucher, (void *)(intptr_t)(i + 1), 0,
                    &ids[i]) == 0);
        for (int i = 0; i < NLWP; i++)
                ok &= _lwp_wait(ids[i], &gone) == 0;
        CHECK(ok && !touch_bad);
        for (int i = 0; i < TOUCH_PAGES; i++) {
                for (int j = 1; j <= NLWP; j++)
                        ok &= touch_area[(size_t)i * 4096 + j] == (char)j;
        }
        CHECK(ok);
        CHECK(munmap(touch_area, TOUCH_PAGES * 4096) == 0);
}

/* --- Job control of a process with several LWPs. --- */

static void
test_jobcontrol(void)
{
        volatile uint64_t *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANON, -1, 0);
        uint64_t a, b;
        pid_t pid;
        int s;

        CHECK(shared != MAP_FAILED);
        shared[0] = 0;
        if ((pid = fork1()) == 0) {
                for (int i = 0; i < 3; i++)
                        spawn_lwp(spinner, NULL, 0, NULL);
                for (;;)
                        __atomic_fetch_add(&shared[0], 1, __ATOMIC_RELAXED);
        }
        while (shared[0] == 0)
                continue;
        CHECK(kill(pid, SIGSTOP) == 0);
        CHECK(waitpid(pid, &s, WSTOPPED) == pid && WIFSTOPPED(s));
        /* Every LWP was told at once; give the last of them a moment. */
        spin_ticks(2000000);
        a = shared[0];
        spin_ticks(5000000);
        b = shared[0];
        CHECK(a == b);
        CHECK(kill(pid, SIGCONT) == 0);
        while (shared[0] == b)
                continue;
        CHECK(kill(pid, SIGKILL) == 0);
        CHECK(waitpid(pid, &s, 0) == pid && WIFSIGNALED(s) &&
            WTERMSIG(s) == SIGKILL);
        munmap((void *)shared, 4096);
}

/* --- Each LWP's own registers: %fs base and vector state. --- */

static volatile int private_bad;

static void
private_state(void *arg)
{
        void *block[2];
        double x = (double)(intptr_t)arg, acc = 0;

        block[0] = block;
        if (lwp_private(block) != 0)
                private_bad = 1;
        for (int i = 0; i < 2000000; i++) {
                acc += x;
                if ((i & 0xffff) == 0) {
                        void *v;

                        __asm__ __volatile__("movq %%fs:0, %0" : "=r"(v));
                        if (v != (void *)block)
                                private_bad = 1;
                }
        }
        if (acc != x * 2000000)
                private_bad = 1;
}

static void
test_private(void)
{
        lwpid_t ids[NLWP], gone;
        int ok = 1;

        private_bad = 0;
        for (int i = 0; i < NLWP; i++)
                CHECK(spawn_lwp(private_state, (void *)(intptr_t)(i + 1), 0,
                    &ids[i]) == 0);
        for (int i = 0; i < NLWP; i++)
                ok &= _lwp_wait(ids[i], &gone) == 0;
        CHECK(ok && !private_bad);
}

int
main(int argc, char **argv, char **envp __attribute__((__unused__)))
{
        const char *argtest = argc > 1 ? argv[1] : "/p/bin/argtest";

        test_create_wait();
        test_sema();
        test_addr();
        test_suspend();
        test_signals();
        test_sigwait();
        test_endings(argtest);
        test_forks();
        test_concurrent_vm();
        test_jobcontrol();
        test_private();
        printf("lwptest: pid %d: %d failures\n", getpid(), failures);
        return failures;
}
