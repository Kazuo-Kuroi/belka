/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 */

/*
 * Compiled against the mlibc sysroot, with the header kernel.c wrote:
 * fails to compile if mlibc's headers disagree with the kernel's about
 * any constant, size, or offset in abi.def.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stropts.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <ucontext.h>

#include "kernel-abi.h"

#define PASTE2(a, b)    a##b
#define PASTE(a, b)     PASTE2(a, b)
#define ASSERT(cond)    typedef char PASTE(abi_check_, __LINE__)[(cond) ? 1 : -1];

#define CONST(n)        ASSERT((long long)(n) == K_##n)
#define SIZE(k, l)      ASSERT(sizeof(l) == PASTE(K_SIZE_, __LINE__))
#define OFF(k, kf, l, lf) ASSERT(offsetof(l, lf) == PASTE(K_OFF_, __LINE__))

#include "abi.def"
