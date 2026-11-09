/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * A minimal C library for the kernel's user-mode tests, over the native
 * system call interface and the kernel's own ABI headers.
 */

#ifndef _LIBT_H_
#define _LIBT_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/dirent.h>
#include <sys/elf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lwp.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/utsname.h>
#include <sys/wait.h>

extern int errno;
extern char **environ;
extern Elf64_auxv_t *__auxv;

long __sysc(long, long, long, long, long, long, long);
long __sysc2(long, long, long, long, long *);
long __sysc_cf(long, long, long *);

#define SYS0(n)                 __sysc((n), 0, 0, 0, 0, 0, 0)
#define SYS1(n, a)              __sysc((n), (long)(a), 0, 0, 0, 0, 0)
#define SYS2(n, a, b)           __sysc((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define SYS3(n, a, b, c)        __sysc((n), (long)(a), (long)(b), (long)(c), \
                                    0, 0, 0)

void _exit(int) __attribute__((__noreturn__));
pid_t fork(void);
pid_t getpid(void);
pid_t getppid(void);
pid_t waitpid(pid_t, int *, int);
int execve(const char *, char *const[], char *const[]);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
ssize_t pread(int, void *, size_t, off_t);
ssize_t pwrite(int, const void *, size_t, off_t);
int open(const char *, int, ...);
int close(int);
off_t lseek(int, off_t, int);
int dup(int);
int fcntl(int, int, long);
int stat(const char *, struct stat *);
int fstat(int, struct stat *);
int mkdir(const char *, mode_t);
int rmdir(const char *);
int unlink(const char *);
int rename(const char *, const char *);
int symlink(const char *, const char *);
ssize_t readlink(const char *, char *, size_t);
int chdir(const char *);
int fchdir(int);
int ftruncate(int, off_t);
long getdents(int, void *, size_t);
void *sbrk(long);
int brk(void *);
void *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int uname(struct utsname *);
long time(void);
int lwp_private(void *);

void __sigtramp(void);
int sigaction(int, const struct sigaction *, struct sigaction *);
void (*signal(int, void (*)(int)))(int);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigsuspend(const sigset_t *);
int sigpending(sigset_t *);
int sigaltstack(const stack_t *, stack_t *);
int kill(pid_t, int);
int pause(void);
unsigned int alarm(unsigned int);
int getcontext(ucontext_t *);
int setcontext(const ucontext_t *);
pid_t getpgrp(void);
int setpgid(pid_t, pid_t);
pid_t setsid(void);
int sigemptyset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigismember(const sigset_t *, int);

/* LWPs: these return 0 or an error number, as UnixWare's do. */
int _lwp_makecontext(ucontext_t *, void (*)(void *), void *, void *, size_t);
int _lwp_create(ucontext_t *, unsigned long, lwpid_t *);
void _lwp_exit(void) __attribute__((__noreturn__));
int _lwp_wait(lwpid_t, lwpid_t *);
lwpid_t _lwp_self(void);
int _lwp_kill(lwpid_t, int);
int _lwp_suspend(lwpid_t);
int _lwp_continue(lwpid_t);
int _lwp_sema_init(_lwp_sema_t *, int);
int _lwp_sema_wait(_lwp_sema_t *);
int _lwp_sema_post(_lwp_sema_t *);
int _lwp_sema_trywait(_lwp_sema_t *);
int sigtimedwait(const sigset_t *, siginfo_t *, const int64_t *);
pid_t fork1(void);
pid_t forkall(void);

size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);

int vsnprintf(char *, size_t, const char *, va_list);
int snprintf(char *, size_t, const char *, ...);
int printf(const char *, ...);

#endif /* _LIBT_H_ */
