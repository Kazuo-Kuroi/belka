/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * System call numbers. The native amd64 table uses the UnixWare numbers,
 * with arguments in %rdi, %rsi, %rdx, %r10, %r8, and %r9 and the number
 * in %rax; see doc/design/decisions.md for the return convention. Only
 * the calls implemented so far are listed; every other number posts
 * SIGSYS and returns EINVAL, as an unassigned UnixWare slot does (127,
 * UnixWare's invsys, returns EINVAL alone).
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
#define SYS_mknod       14
#define SYS_chmod       15
#define SYS_chown       16
#define SYS_brk         17
#define SYS_stat        18
#define SYS_lseek       19
#define SYS_getpid      20
#define SYS_mount       21
#define SYS_umount      22
#define SYS_setuid      23
#define SYS_getuid      24
#define SYS_stime       25
#define SYS_alarm       27
#define SYS_utime       30
#define SYS_fstat       28
#define SYS_pause       29
#define SYS_access      33
#define SYS_nice        34
#define SYS_sync        36
#define SYS_kill        37
#define SYS_pgrpsys     39
#define SYS_dup         41
#define SYS_pipe        42
#define SYS_times       43
#define SYS_setgid      46
#define SYS_getgid      47
#define SYS_signal      48
#define SYS_ioctl       54
#define SYS_uadmin      55
#define SYS_fsync       58
#define SYS_execve      59
#define SYS_umask       60
#define SYS_fcntl       62
#define SYS_rmdir       79
#define SYS_mkdir       80
#define SYS_getdents    81
#define SYS_poll        87
#define SYS_lstat       88
#define SYS_symlink     89
#define SYS_readlink    90
#define SYS_setgroups   91
#define SYS_getgroups   92
#define SYS_fchmod      93
#define SYS_fchown      94
#define SYS_sigprocmask 95
#define SYS_sigsuspend  96
#define SYS_sigaltstack 97
#define SYS_sigaction   98
#define SYS_sigpending  99
#define SYS_context     100
#define SYS_statvfs     103
#define SYS_fstatvfs    104
#define SYS_waitsys     107
#define SYS_mmap        115
#define SYS_mprotect    116
#define SYS_munmap      117
#define SYS_fchdir      120
#define SYS_readv       121
#define SYS_writev      122
#define SYS_setrlimit   128
#define SYS_getrlimit   129
#define SYS_lchown      130
#define SYS_rename      134
#define SYS_uname       135
#define SYS_setegid     136
#define SYS_systeminfo  139
#define SYS_seteuid     141
#define SYS_setitimer   169
#define SYS_getitimer   170
#define SYS_gettimeofday 171
#define SYS_lwpcreate   173
#define SYS_lwpexit     174
#define SYS_lwpwait     175
#define SYS_lwpself     176
#define SYS_lwpinfo     177
#define SYS_lwpprivate  178
#define SYS_lwp_addr_wait 181    /* Belka: UnixWare left 181 and 182 free */
#define SYS_lwp_addr_wake 182
#define SYS_pread       189
#define SYS_pwrite      190
#define SYS_ftruncate   192
#define SYS_lwpkill     193
#define SYS_sigwait     194
#define SYS_fork1       195
#define SYS_forkall     196
#define SYS_lwpsuspend  203
#define SYS_lwpcontinue 204
#define SYS_lwp_sema_wait 207
#define SYS_lwp_sema_post 208
#define SYS_lwp_sema_trywait 209

/*
 * Belka's own calls, for what UnixWare 2 does not have, from 210 on
 * (after its last). The i386 personality has its own table.
 */
#define SYS_clock_gettime 210
#define SYS_clock_getres 211
#define SYS_yield       212
#define SYS_openat      213
#define SYS_mknodat     214
#define SYS_mkdirat     215
#define SYS_unlinkat    216
#define SYS_renameat    217
#define SYS_linkat      218
#define SYS_symlinkat   219
#define SYS_readlinkat  220
#define SYS_fstatat     221
#define SYS_fchownat    222
#define SYS_fchmodat    223
#define SYS_faccessat   224
#define SYS_pollsys     225
#define SYS_getrusage   226
#define SYS_utimensat   227
#define SYS_flock       228
#define SYS_sigqueue    229
#define SYS_socketpair  230
#define SYS_send        231
#define SYS_recv        232
#define SYS_sockatmark  233

#define NSYSCALL        234     /* table size: UnixWare's 210, and Belka's */

#endif /* _SYS_SYSCALL_H_ */
