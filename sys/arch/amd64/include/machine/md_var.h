/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Declarations shared within the amd64 machine-dependent code.
 */

#ifndef _MACHINE_MD_VAR_H_
#define _MACHINE_MD_VAR_H_

#include <stdint.h>

#include <sys/compiler.h>

#include <machine/types.h>

struct trapframe;

/* machdep.c: called from locore.S on the bootstrap processor. */
void amd64_init(void) __dead;

/* trap.c */
void trap(struct trapframe *);

/*
 * Self-test support. trap_expect(vector, skip) arms this processor to
 * absorb one kernel-mode trap of the given vector: trap() records it
 * and resumes skip bytes past the faulting instruction, or, with
 * TRAP_RESUME_RETURN, by returning from the function that faulted.
 * trap_expect_done() disarms, returns 1 if the trap happened, and
 * stores its error code.
 */
#define TRAP_RESUME_RETURN      (-1)
void trap_expect(int, int);
int trap_expect_done(uint64_t *);

/* hat.c */
int hat_nx_enabled(void);

/* vm_machdep.c */
struct lwp;
void cpu_syscall_init(void);
void md_lwp_init(struct lwp *, vaddr_t);
void md_lwp_free(struct lwp *);
void md_setregs(struct lwp *, uint64_t, uint64_t);
void md_lwp_fork(struct lwp *, struct lwp *);
void md_set_return(struct lwp *, uint64_t, uint64_t);
int md_set_private(struct lwp *, uint64_t);

/* The nanosecond clock, if the TSC is invariant (lapic.c). */
extern int md_hrtime_ok;
uint64_t md_hrtime(void);
void md_syscall_return(struct trapframe *, int, uint64_t, uint64_t);
void user_enter(struct trapframe *) __dead;
struct siginfo;
struct ucontext;
int md_sendsig(struct lwp *, int, const struct siginfo *, uint64_t, uint64_t,
    int, int, uint64_t);
void md_getcontext(struct lwp *, struct ucontext *);
int md_setcontext(struct lwp *, const struct ucontext *);
int md_context_ok(const struct ucontext *);
void md_lwp_setcontext(struct lwp *, const struct ucontext *);
void md_lwp_copy(const struct lwp *, struct lwp *);
void syscall(struct trapframe *);

/* Symbols defined by the linker script and locore.S. */
extern char kernel_start[];
extern char etext[];
extern char erodata[];
extern char kernel_end[];
extern char boot_stack[];
extern char boot_stack_top[];

#endif /* _MACHINE_MD_VAR_H_ */
