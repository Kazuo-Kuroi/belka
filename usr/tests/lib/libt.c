/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The test programs' C library. See libt.h.
 *
 * The wrappers follow the System V conventions the kernel implements:
 * fork returns the other process's id with a flag in the second
 * register (1 in the child), getpid returns the parent's id in the
 * second, and waitpid is built on waitsys.
 */

#include "libt.h"

int errno;
char **environ;
Elf64_auxv_t *__auxv;

int main(int, char **, char **);
void __libt_start(long *) __attribute__((__noreturn__));

void
__libt_start(long *sp)
{
        int argc = (int)sp[0];
        char **argv = (char **)&sp[1];
        char **envp = argv + argc + 1;
        char **e = envp;

        while (*e != NULL)
                e++;
        environ = envp;
        __auxv = (Elf64_auxv_t *)(void *)(e + 1);
        _exit(main(argc, argv, envp));
}

void
_exit(int status)
{
        (void)SYS1(SYS_exit, status);
        for (;;)
                continue;
}

pid_t
fork(void)
{
        long flag = 0;
        long r = __sysc2(SYS_fork, 0, 0, 0, &flag);

        if (r < 0)
                return -1;
        return flag != 0 ? 0 : (pid_t)r;
}

pid_t
getpid(void)
{
        long ppid;

        return (pid_t)__sysc2(SYS_getpid, 0, 0, 0, &ppid);
}

pid_t
getppid(void)
{
        long ppid = -1;

        (void)__sysc2(SYS_getpid, 0, 0, 0, &ppid);
        return (pid_t)ppid;
}

/*
 * waitpid over waitsys. The status word is the traditional one:
 * (code << 8) for an exit, the signal number for a kill.
 */
pid_t
waitpid(pid_t pid, int *status, int options)
{
        siginfo_t si;
        long r;

        memset(&si, 0, sizeof(si));
        r = __sysc(SYS_waitsys, pid == -1 ? P_ALL : P_PID, pid, (long)&si,
            WEXITED | options, 0, 0);
        if (r < 0)
                return -1;
        if (si.si_pid != 0 && status != NULL)
                *status = si.si_code == CLD_EXITED ?
                    (si.si_status & 0xff) << 8 : si.si_status & 0x7f;
        return si.si_pid;
}

int
execve(const char *path, char *const argv[], char *const envp[])
{
        return (int)SYS3(SYS_execve, path, argv, envp);
}

ssize_t
read(int fd, void *buf, size_t n)
{
        return SYS3(SYS_read, fd, buf, n);
}

ssize_t
write(int fd, const void *buf, size_t n)
{
        return SYS3(SYS_write, fd, buf, n);
}

ssize_t
pread(int fd, void *buf, size_t n, off_t off)
{
        return __sysc(SYS_pread, fd, (long)buf, (long)n, off, 0, 0);
}

ssize_t
pwrite(int fd, const void *buf, size_t n, off_t off)
{
        return __sysc(SYS_pwrite, fd, (long)buf, (long)n, off, 0, 0);
}

int
open(const char *path, int flags, ...)
{
        va_list ap;
        int mode;

        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        return (int)SYS3(SYS_open, path, flags, mode);
}

int
close(int fd)
{
        return (int)SYS1(SYS_close, fd);
}

off_t
lseek(int fd, off_t off, int whence)
{
        return SYS3(SYS_lseek, fd, off, whence);
}

int
dup(int fd)
{
        return (int)SYS1(SYS_dup, fd);
}

int
fcntl(int fd, int cmd, long arg)
{
        return (int)SYS3(SYS_fcntl, fd, cmd, arg);
}

int
stat(const char *path, struct stat *st)
{
        return (int)SYS2(SYS_stat, path, st);
}

int
fstat(int fd, struct stat *st)
{
        return (int)SYS2(SYS_fstat, fd, st);
}

int
mkdir(const char *path, mode_t mode)
{
        return (int)SYS2(SYS_mkdir, path, mode);
}

int
rmdir(const char *path)
{
        return (int)SYS1(SYS_rmdir, path);
}

int
unlink(const char *path)
{
        return (int)SYS1(SYS_unlink, path);
}

int
rename(const char *from, const char *to)
{
        return (int)SYS2(SYS_rename, from, to);
}

int
symlink(const char *target, const char *path)
{
        return (int)SYS2(SYS_symlink, target, path);
}

ssize_t
readlink(const char *path, char *buf, size_t n)
{
        return SYS3(SYS_readlink, path, buf, n);
}

int
chdir(const char *path)
{
        return (int)SYS1(SYS_chdir, path);
}

int
fchdir(int fd)
{
        return (int)SYS1(SYS_fchdir, fd);
}

int
ftruncate(int fd, off_t len)
{
        return (int)SYS2(SYS_ftruncate, fd, len);
}

long
getdents(int fd, void *buf, size_t n)
{
        return SYS3(SYS_getdents, fd, buf, n);
}

extern char end[];
static char *curbrk;

int
brk(void *addr)
{
        if (SYS1(SYS_brk, addr) < 0)
                return -1;
        curbrk = addr;
        return 0;
}

void *
sbrk(long incr)
{
        char *old;

        if (curbrk == NULL)
                curbrk = end;
        old = curbrk;
        if (incr != 0 && brk(old + incr) < 0)
                return (void *)-1;
        return old;
}

void *
mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
        long r = __sysc(SYS_mmap, (long)addr, (long)len, prot, flags, fd,
            off);

        return r == -1 ? MAP_FAILED : (void *)r;
}

int
munmap(void *addr, size_t len)
{
        return (int)SYS2(SYS_munmap, addr, len);
}

int
mprotect(void *addr, size_t len, int prot)
{
        return (int)SYS3(SYS_mprotect, addr, len, prot);
}

int
uname(struct utsname *u)
{
        return (int)SYS1(SYS_uname, u);
}

long
time(void)
{
        return SYS0(SYS_time);
}

int
lwp_private(void *p)
{
        return (int)SYS1(SYS_lwpprivate, p);
}

size_t
strlen(const char *s)
{
        size_t n = 0;

        while (s[n] != '\0')
                n++;
        return n;
}

int
strcmp(const char *a, const char *b)
{
        while (*a != '\0' && *a == *b) {
                a++;
                b++;
        }
        return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
        for (; n > 0; n--, a++, b++) {
                if (*a != *b || *a == '\0')
                        return (unsigned char)*a - (unsigned char)*b;
        }
        return 0;
}

void *
memcpy(void *d, const void *s, size_t n)
{
        unsigned char *dp = d;
        const unsigned char *sp = s;

        while (n-- > 0)
                *dp++ = *sp++;
        return d;
}

void *
memset(void *d, int c, size_t n)
{
        unsigned char *dp = d;

        while (n-- > 0)
                *dp++ = (unsigned char)c;
        return d;
}

int
memcmp(const void *a, const void *b, size_t n)
{
        const unsigned char *x = a, *y = b;

        for (; n > 0; n--, x++, y++) {
                if (*x != *y)
                        return *x - *y;
        }
        return 0;
}

/* Enough of printf for test reports: %d %u %x %p %s %c, with l. */
int
vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
        size_t n = 0;

#define PUT(c)  do { if (n + 1 < size) buf[n] = (c); n++; } while (0)
        for (; *fmt != '\0'; fmt++) {
                char tmp[24];
                unsigned long u;
                int base = 10, neg = 0, longarg = 0, i = 0;
                const char *s;

                if (*fmt != '%') {
                        PUT(*fmt);
                        continue;
                }
                fmt++;
                if (*fmt == 'l') {
                        longarg = 1;
                        fmt++;
                }
                switch (*fmt) {
                case 'd':
                        if (longarg) {
                                long v = va_arg(ap, long);

                                neg = v < 0;
                                u = neg ? -(unsigned long)v : (unsigned long)v;
                        } else {
                                int v = va_arg(ap, int);

                                neg = v < 0;
                                u = neg ? -(unsigned long)v : (unsigned long)v;
                        }
                        goto number;
                case 'x':
                        base = 16;
                        /* FALLTHROUGH */
                case 'u':
                        u = longarg ? va_arg(ap, unsigned long) :
                            va_arg(ap, unsigned int);
                        goto number;
                case 'p':
                        u = (unsigned long)va_arg(ap, void *);
                        base = 16;
                        PUT('0');
                        PUT('x');
number:
                        do {
                                tmp[i++] = "0123456789abcdef"[u % base];
                                u /= base;
                        } while (u != 0);
                        if (neg)
                                PUT('-');
                        while (i > 0)
                                PUT(tmp[--i]);
                        break;
                case 's':
                        for (s = va_arg(ap, const char *); *s != '\0'; s++)
                                PUT(*s);
                        break;
                case 'c':
                        PUT((char)va_arg(ap, int));
                        break;
                default:
                        PUT('%');
                        PUT(*fmt);
                        break;
                }
        }
        if (size != 0)
                buf[n < size ? n : size - 1] = '\0';
#undef PUT
        return (int)n;
}

int
snprintf(char *buf, size_t size, const char *fmt, ...)
{
        va_list ap;
        int n;

        va_start(ap, fmt);
        n = vsnprintf(buf, size, fmt, ap);
        va_end(ap);
        return n;
}

int
printf(const char *fmt, ...)
{
        char buf[512];
        va_list ap;
        int n;

        va_start(ap, fmt);
        n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        (void)write(1, buf, n < (int)sizeof(buf) ? (size_t)n :
            sizeof(buf) - 1);
        return n;
}
