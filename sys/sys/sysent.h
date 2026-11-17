/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * System call handlers. Each takes the six argument registers and two
 * return values and returns 0 or an errno value (see kern_syscall.c).
 */

#ifndef _SYS_SYSENT_H_
#define _SYS_SYSENT_H_

#include <stdint.h>

typedef int sy_call_t(const uint64_t *, uint64_t *);

int syscall_dispatch(uint64_t, const uint64_t *, uint64_t *);

/* kern_exit.c, kern_fork.c, kern_exec.c */
sy_call_t sys_exit, sys_wait, sys_waitsys, sys_fork, sys_fork1, sys_execve;
sy_call_t sys_exec;
/* sys_generic.c */
sy_call_t sys_read, sys_write, sys_pread, sys_pwrite, sys_lseek, sys_close;
sy_call_t sys_dup, sys_fcntl, sys_ioctl, sys_getdents, sys_ftruncate;
sy_call_t sys_flock;
sy_call_t sys_readv, sys_writev, sys_fsync, sys_fchmod, sys_fchown;
sy_call_t sys_fstat;
/* vfs_syscalls.c */
sy_call_t sys_open, sys_creat, sys_link, sys_unlink, sys_chdir, sys_fchdir;
sy_call_t sys_chmod, sys_stat, sys_lstat, sys_access, sys_rmdir, sys_mkdir;
sy_call_t sys_symlink, sys_readlink, sys_rename;
sy_call_t sys_openat, sys_mknodat, sys_mkdirat, sys_unlinkat, sys_renameat;
sy_call_t sys_linkat, sys_symlinkat, sys_readlinkat, sys_fstatat;
sy_call_t sys_fchownat, sys_fchmodat, sys_faccessat;
sy_call_t sys_statvfs, sys_fstatvfs, sys_utime, sys_utimensat;
sy_call_t sys_mount, sys_umount;
/* vm_mman.c */
sy_call_t sys_brk, sys_mmap, sys_munmap, sys_mprotect;
/* kern_sig.c */
sy_call_t sys_kill, sys_sigaction, sys_sigprocmask, sys_sigsuspend;
sy_call_t sys_sigqueue;
sy_call_t sys_socketpair, sys_send, sys_recv, sys_sockatmark;
sy_call_t sys_sigpending, sys_sigaltstack, sys_signal, sys_pause;
sy_call_t sys_context, sys_pgrpsys, sys_sigwait;
/* kern_lwp.c */
sy_call_t sys_lwpcreate, sys_lwpexit, sys_lwpwait, sys_lwpself, sys_lwpinfo;
sy_call_t sys_lwpkill, sys_lwpsuspend, sys_lwpcontinue;
sy_call_t sys_lwp_sema_wait, sys_lwp_sema_post, sys_lwp_sema_trywait;
sy_call_t sys_lwp_addr_wait, sys_lwp_addr_wake;
/* fs/fifofs/fifo_vnops.c */
sy_call_t sys_pipe, sys_mknod, sys_chown, sys_lchown, sys_poll;
sy_call_t sys_getrlimit, sys_setrlimit, sys_getrusage, sys_pollsys;
sy_call_t sys_nice;
/* kern_uadmin.c */
sy_call_t sys_uadmin;
/* kern_itimer.c */
sy_call_t sys_alarm, sys_setitimer, sys_getitimer;
/* kern_time.c */
sy_call_t sys_clock_gettime, sys_clock_getres, sys_times, sys_yield;
sy_call_t sys_stime;
/* kern_cred.c */
sy_call_t sys_setuid, sys_seteuid, sys_setgid, sys_setegid, sys_getuid;
sy_call_t sys_getgid, sys_getgroups, sys_setgroups;

#endif /* _SYS_SYSENT_H_ */
