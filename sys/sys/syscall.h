/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * System call numbers. The native amd64 table uses the UnixWare numbers,
 * with arguments in %rdi, %rsi, %rdx, %r10, %r8, and %r9 and the number
 * in %rax; see doc/design/decisions.md for the return convention. Only
 * the calls implemented so far are listed; every other number returns
 * EINVAL, as an unassigned UnixWare slot does.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_SYSCALL_H_
#define _SYS_SYSCALL_H_

#define SYS_exit        1
#define SYS_fork        2
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_wait        7
#define SYS_creat       8
#define SYS_link        9
#define SYS_unlink      10
#define SYS_exec        11
#define SYS_chdir       12
#define SYS_time        13
#define SYS_chmod       15
#define SYS_brk         17
#define SYS_stat        18
#define SYS_lseek       19
#define SYS_getpid      20
#define SYS_getuid      24
#define SYS_fstat       28
#define SYS_access      33
#define SYS_sync        36
#define SYS_dup         41
#define SYS_getgid      47
#define SYS_ioctl       54
#define SYS_execve      59
#define SYS_umask       60
#define SYS_fcntl       62
#define SYS_rmdir       79
#define SYS_mkdir       80
#define SYS_getdents    81
#define SYS_lstat       88
#define SYS_symlink     89
#define SYS_readlink    90
#define SYS_waitsys     107
#define SYS_mmap        115
#define SYS_mprotect    116
#define SYS_munmap      117
#define SYS_fchdir      120
#define SYS_rename      134
#define SYS_uname       135
#define SYS_gettimeofday 171
#define SYS_lwpprivate  178
#define SYS_pread       189
#define SYS_pwrite      190
#define SYS_ftruncate   192
#define SYS_fork1       195

#define NSYSCALL        210     /* table size, as in UnixWare */

#endif /* _SYS_SYSCALL_H_ */
