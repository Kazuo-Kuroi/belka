/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Machine-dependent support for LWPs: the system call MSRs, what the
 * context switch does for user state, and LWP register set-up for exec
 * and fork.
 *
 * An LWP's user registers live in a trap frame at the top of its
 * kernel stack, where the processor and Xsyscall build one on every
 * entry from user mode (TSS.RSP0 and ci_kstack point at that stack while
 * the LWP runs). Its floating-point state and %fs base are switched
 * eagerly with the thread.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/frame.h>
#include <machine/fpu.h>
#include <machine/intr.h>
#include <machine/md_var.h>
#include <machine/segments.h>
#include <machine/tss.h>
#include <machine/vmparam.h>

#include <vm/as.h>
#include <vm/hat.h>

#define MSR_STAR        0xc0000081
#define MSR_LSTAR       0xc0000082
#define MSR_SFMASK      0xc0000084
#define EFER_SCE        0x00000001

#define PSL_C           0x00000001
#define PSL_RESERVED1   0x00000002      /* always set */
#define PSL_T           0x00000100
#define PSL_D           0x00000400
#define PSL_IOPL        0x00003000
#define PSL_NT          0x00004000
#define PSL_AC          0x00040000

#define CR4_SMEP        0x00100000
#define CPUID7_EBX_SMEP 0x00000080

void Xsyscall(void);

/*
 * Set the calling processor up for system calls, and turn on SMEP so
 * the kernel can never execute a user page. Interrupts are disabled.
 */
void
cpu_syscall_init(void)
{
        uint32_t regs[4];

        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
        /*
         * sysret takes CS from STAR[63:48] + 16 and SS from + 8; the GDT
         * keeps the 32-bit user code, user data, and 64-bit user code
         * descriptors in that order for it.
         */
        wrmsr(MSR_STAR, ((uint64_t)UCSEL32 << 48) | ((uint64_t)KCSEL << 32));
        wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)Xsyscall);
        wrmsr(MSR_SFMASK, PSL_I | PSL_T | PSL_D | PSL_IOPL | PSL_NT | PSL_AC);

        cpuid(0, 0, regs);
        if (regs[0] >= 7) {
                cpuid(7, 0, regs);
                if ((regs[1] & CPUID7_EBX_SMEP) != 0)
                        lcr4(rcr4() | CR4_SMEP);
        }
}

/*
 * The scheduler is about to switch from old to new on this processor,
 * with interrupts disabled: save old's user state and load new's.
 */
void
cpu_switch_md(struct kthread *old, struct kthread *new)
{
        struct cpu_info *ci = curcpu();
        struct lwp *l;

        if ((l = old->kt_lwp) != NULL) {
                fpu_save(l->l_fpu);
                l->l_fsbase = rdmsr(MSR_FSBASE);
        }
        if (new->kt_stack != 0) {
                ci->ci_kstack = new->kt_stack;
                tss_set64(ci->ci_tss, TSS_RSP0, new->kt_stack);
        }
        if ((l = new->kt_lwp) != NULL) {
                struct as *as = l->l_proc->p_as;

                fpu_restore(l->l_fpu);
                wrmsr(MSR_FSBASE, l->l_fsbase);
                hat_activate(as != NULL ? as->a_hat : NULL);
        } else {
                hat_activate(NULL);
        }
}

/* Give a new LWP its trap frame and floating-point area. */
void
md_lwp_init(struct lwp *l, vaddr_t kstack_top)
{
        l->l_tf = (struct trapframe *)(uintptr_t)(kstack_top -
            sizeof(struct trapframe));
        memset(l->l_tf, 0, sizeof(*l->l_tf));
        l->l_fpu = fpu_area_alloc();
        fpu_area_reset(l->l_fpu);
        l->l_fsbase = 0;
}

void
md_lwp_free(struct lwp *l)
{
        fpu_area_free(l->l_fpu);
}

/*
 * Start a new program in the calling LWP: registers as the psABI wants
 * them at the entry point, clean floating-point state, no %fs base.
 */
void
md_setregs(struct lwp *l, uint64_t entry, uint64_t sp)
{
        struct trapframe *tf = l->l_tf;
        int s;

        memset(tf, 0, sizeof(*tf));
        tf->tf_rip = entry;
        tf->tf_rsp = sp;
        tf->tf_cs = UCSEL;
        tf->tf_ss = UDSEL;
        tf->tf_rflags = PSL_I | PSL_RESERVED1;
        s = splhi();
        fpu_area_reset(l->l_fpu);
        fpu_restore(l->l_fpu);
        l->l_fsbase = 0;
        wrmsr(MSR_FSBASE, 0);
        splx(s);
}

/*
 * Make child a copy of the calling LWP parent, as fork does: the same
 * registers, the floating-point state live in the processor, and the
 * same %fs base. The caller sets the child's return values.
 */
void
md_lwp_fork(struct lwp *parent, struct lwp *child)
{
        int s;

        *child->l_tf = *parent->l_tf;
        s = splhi();
        fpu_save(child->l_fpu);
        child->l_fsbase = rdmsr(MSR_FSBASE);
        splx(s);
}

/* Set the system call return values of an LWP that has not run yet. */
void
md_set_return(struct lwp *l, uint64_t rv1, uint64_t rv2)
{
        l->l_tf->tf_rax = rv1;
        l->l_tf->tf_rdx = rv2;
        l->l_tf->tf_rflags &= ~(uint64_t)PSL_C;
}

/*
 * Set the calling LWP's private data pointer, the %fs base. It must be
 * a user address: a non-canonical one would fault in wrmsr.
 */
int
md_set_private(struct lwp *l, uint64_t base)
{
        int s;

        if (base >= VM_USER_MAX)
                return EINVAL;
        s = splhi();
        l->l_fsbase = base;
        wrmsr(MSR_FSBASE, base);
        splx(s);
        return 0;
}

/*
 * The system call result: on success the two values, carry clear; on
 * failure the error number, carry set (System V).
 */
void
md_syscall_return(struct trapframe *tf, int error, uint64_t rv1,
    uint64_t rv2)
{
        if (error != 0) {
                tf->tf_rax = (uint64_t)error;
                tf->tf_rflags |= PSL_C;
        } else {
                tf->tf_rax = rv1;
                tf->tf_rdx = rv2;
                tf->tf_rflags &= ~(uint64_t)PSL_C;
        }
}
