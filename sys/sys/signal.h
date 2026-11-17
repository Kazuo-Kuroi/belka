/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Signals: numbers, sets, actions, alternate stacks, and signal
 * information, with the UnixWare numbers and flag values. The
 * structures are the native amd64 layouts; the i386 ones belong to the
 * i386 personality.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_SIGNAL_H_
#define _SYS_SIGNAL_H_

#include <stddef.h>
#include <stdint.h>

#define SIGHUP          1
#define SIGINT          2
#define SIGQUIT         3
#define SIGILL          4
#define SIGTRAP         5
#define SIGIOT          6
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
#define SIGCLD          18
#define SIGCHLD         18
#define SIGPWR          19
#define SIGWINCH        20
#define SIGURG          21
#define SIGPOLL         22
#define SIGIO           22
#define SIGSTOP         23
#define SIGTSTP         24
#define SIGCONT         25
#define SIGTTIN         26
#define SIGTTOU         27
#define SIGVTALRM       28
#define SIGPROF         29
#define SIGXCPU         30
#define SIGXFSZ         31
#define SIGWAITING      32
#define SIGLWP          33
#define SIGAIO          34

#define SIGRTMIN        35
#define SIGRTMAX        64
#define MAXSIG          SIGRTMAX
#define NSIG            (MAXSIG + 1)
#define SIGQUEUE_MAX    32

/* A set of signals: bit (sig - 1) of the 128. */
typedef struct {
        uint32_t        __sigbits[4];
} sigset_t;

union sigval {
        int             sival_int;
        void            *sival_ptr;
};

typedef struct siginfo {
        int32_t         si_signo;
        int32_t         si_code;
        int32_t         si_errno;
        int32_t         si_pad0;
        union {
                struct {
                        int32_t _pid;
                        int32_t _uid;
                        union {
                                int32_t _status;        /* SIGCHLD */
                                union sigval _value;
                        } _u;
                        int64_t _utime;         /* SIGCHLD, exited: ticks */
                        int64_t _stime;
                } _proc;
                struct {
                        uint64_t _addr;         /* faults */
                } _fault;
                struct {
                        int64_t _band;          /* SIGPOLL */
                } _poll;
                uint8_t _pad[112];
        } _data;
} siginfo_t;

#define si_pid          _data._proc._pid
#define si_uid          _data._proc._uid
#define si_status       _data._proc._u._status
#define si_value        _data._proc._u._value
#define si_utime        _data._proc._utime
#define si_stime        _data._proc._stime
#define si_addr         _data._fault._addr
#define si_band         _data._poll._band

/* si_code: sent by a process (kill), or by the kernel for a reason. */
#define SI_USER         0
#define SI_LWP          (-1)    /* sent by _lwp_kill */
#define SI_QUEUE        (-2)    /* sent by sigqueue */
#define SI_ASYNCIO      (-4)    /* asynchronous I/O completion */
#define SI_FROMUSER(sip) ((sip)->si_code <= 0)

#define POLL_IN         1
#define POLL_OUT        2

#define ILL_ILLOPC      1
#define ILL_PRVOPC      5
#define FPE_INTDIV      1
#define FPE_FLTINV      7
#define SEGV_MAPERR     1
#define SEGV_ACCERR     2
#define BUS_ADRALN      1
#define BUS_OBJERR      3
#define TRAP_BRKPT      1
#define TRAP_TRACE      2

/* Default and special handlers. */
#define SIG_DFL         ((void (*)(int))0)
#define SIG_IGN         ((void (*)(int))1)
#define SIG_HOLD        ((void (*)(int))2)
#define SIG_ERR         ((void (*)(int))-1)

struct sigaction {
        int32_t         sa_flags;
        int32_t         sa_pad;
        union {
                void    (*_handler)(int);
                void    (*_sigaction)(int, siginfo_t *, void *);
        } _funcptr;
        sigset_t        sa_mask;
        int32_t         sa_resv[2];
};

#define sa_handler      _funcptr._handler
#define sa_sigaction    _funcptr._sigaction

#define SA_ONSTACK      0x00000001
#define SA_RESETHAND    0x00000002
#define SA_RESTART      0x00000004
#define SA_SIGINFO      0x00000008
#define SA_NODEFER      0x00000010
#define SA_NOCLDWAIT    0x00010000
#define SA_NOCLDSTOP    0x00020000

/* sigprocmask() */
#define SIG_BLOCK       1
#define SIG_UNBLOCK     2
#define SIG_SETMASK     3

/* sigpending() commands, as UnixWare's system call takes them. */
#define SIGPENDING_PENDING      1
#define SIGPENDING_FILLSET      2

/* The old signal() call (48): the signal in the low byte, and these. */
#define SIGNO_MASK      0x00ff
#define SIGDEFER        0x0100
#define SIGHOLD         0x0200
#define SIGRELSE        0x0400
#define SIGIGNORE       0x0800
#define SIGPAUSE        0x1000

/* Alternate signal stacks. */
typedef struct {
        void            *ss_sp;
        size_t          ss_size;
        int32_t         ss_flags;
        int32_t         ss_pad;
} stack_t;

#define SS_ONSTACK      0x00000001
#define SS_DISABLE      0x00000002
#define MINSIGSTKSZ     4096
#define SIGSTKSZ        16384

#endif /* _SYS_SIGNAL_H_ */
