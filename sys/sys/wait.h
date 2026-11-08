/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Waiting for children, with the System V values: the old wait() status
 * word, and waitsys()'s id types, options, and siginfo codes.
 */

#ifndef _SYS_WAIT_H_
#define _SYS_WAIT_H_

#include <stdint.h>

/* waitsys() id types. */
#define P_PID           0
#define P_PPID          1
#define P_PGID          2
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

/* The old status word. */
#define WIFEXITED(s)    (((s) & 0xff) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0xff) != 0 && ((s) & 0xff00) == 0)
#define WTERMSIG(s)     ((s) & 0x7f)

/*
 * What waitsys() reports. The layout is the native one, 128 bytes; the
 * members that signals need are added with signal delivery.
 */
typedef struct siginfo {
        int32_t         si_signo;
        int32_t         si_code;
        int32_t         si_errno;
        int32_t         si_pad0;
        int32_t         si_pid;
        int32_t         si_uid;
        int32_t         si_status;
        int32_t         si_pad1;
        uint8_t         si_pad[96];
} siginfo_t;

#endif /* _SYS_WAIT_H_ */
