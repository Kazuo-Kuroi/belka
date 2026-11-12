/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-12
 * SPDX-License-Identifier: MIT
 */

/*
 * Resource limits: getrlimit (129) and setrlimit (128), with System V's
 * resource numbers. Limits are 64-bit; RLIM_INFINITY is all ones, as
 * the C library has it.
 */

#ifndef _SYS_RESOURCE_H_
#define _SYS_RESOURCE_H_

#include <stdint.h>

#define RLIMIT_CPU      0       /* processor time, seconds */
#define RLIMIT_FSIZE    1       /* largest file written, bytes */
#define RLIMIT_DATA     2       /* data segment (the break), bytes */
#define RLIMIT_STACK    3       /* stack, bytes, taken at exec */
#define RLIMIT_CORE     4       /* core file, bytes */
#define RLIMIT_NOFILE   5       /* open files */
#define RLIMIT_VMEM     6       /* mapped address space, bytes */
#define RLIMIT_AS       RLIMIT_VMEM
#define RLIM_NLIMITS    7

typedef uint64_t        rlim_t;

#define RLIM_INFINITY   (~(rlim_t)0)

struct rlimit {
        rlim_t          rlim_cur;       /* the soft limit, enforced */
        rlim_t          rlim_max;       /* the hard limit, the soft one's ceiling */
};

#ifdef _KERNEL

struct proc;

void rlimit_init(struct proc *);
rlim_t rlimit_cur(struct proc *, int);
void rlimit_cpu_check(void);

#endif /* _KERNEL */

#endif /* _SYS_RESOURCE_H_ */
