/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Signal numbers, with the System V values. Only the numbers exist so
 * far: a fatal fault ends the process as if killed by the signal, and
 * delivery and handlers come with the signal work.
 */

#ifndef _SYS_SIGNAL_H_
#define _SYS_SIGNAL_H_

#define SIGHUP          1
#define SIGINT          2
#define SIGQUIT         3
#define SIGILL          4
#define SIGTRAP         5
#define SIGABRT         6
#define SIGEMT          7
#define SIGFPE          8
#define SIGKILL         9
#define SIGBUS          10
#define SIGSEGV         11
#define SIGSYS          12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGUSR1         16
#define SIGUSR2         17
#define SIGCHLD         18

#endif /* _SYS_SIGNAL_H_ */
