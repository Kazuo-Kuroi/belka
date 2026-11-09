/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Waiting for children, with the System V values: the old wait() status
 * word, and waitsys()'s id types, options, and siginfo codes. waitsys()
 * reports through a siginfo_t (<sys/signal.h>).
 */

#ifndef _SYS_WAIT_H_
#define _SYS_WAIT_H_

#include <stdint.h>

#include <sys/signal.h>

/* waitsys() id types. */
#define P_PID           0
#define P_PPID          1
#define P_PGID          2
#define P_SID           3
#define P_ALL           7

/* waitsys() options. */
#define WEXITED         0x01
#define WTRAPPED        0x02
#define WSTOPPED        0x04
#define WCONTINUED      0x08
#define WNOHANG         0x40
#define WNOWAIT         0x80

/* si_code for SIGCHLD. */
#define CLD_EXITED      1
#define CLD_KILLED      2
#define CLD_DUMPED      3
#define CLD_TRAPPED     4
#define CLD_STOPPED     5
#define CLD_CONTINUED   6

/* The old status word. */
#define WIFEXITED(s)    (((s) & 0xff) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0xff) != 0 && ((s) & 0xff00) == 0)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#define WSTOPFLG        0x7f    /* the low byte of a stopped status */
#define WCONTFLG        0xffff  /* the status of a continued child */


#endif /* _SYS_WAIT_H_ */
