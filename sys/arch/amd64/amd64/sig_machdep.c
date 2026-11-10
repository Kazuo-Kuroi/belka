/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Signal frames and user contexts on amd64.
 *
 * A handler is entered as a psABI function: handler(sig, siginfo_t *,
 * ucontext_t *) in %rdi, %rsi, %rdx, with %rsp 8 off a 16-byte boundary
 * and the return address on top. Below the interrupted %rsp, past its
 * 128-byte red zone (or at the top of the alternate stack), the frame
 * holds:
 *
 *      ucontext_t      the interrupted context and the mask to restore
 *      siginfo_t       with SA_SIGINFO
 *      return address  the library's trampoline, registered with
 *                      sigaction's fourth argument (UnixWare's
 *                      p_sigactret), or a non-canonical value that
 *                      faults if there is none
 *
 * %rbx also holds the ucontext_t address, so a trampoline can pass it to
 * setcontext after the handler returns. The handler gets clean
 * floating-point state; the interrupted state is in the context.
 *
 * setcontext loads only what user mode may hold: the code and stack
 * selectors are the user ones, rflags keeps only user-changeable bits
 * and interrupts enabled, %rip and the %fs base must be user addresses,
 * and the floating-point image is cleaned of anything the processor
 * would refuse.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/systm.h>
#include <sys/ucontext.h>

#include <machine/cpufunc.h>
#include <machine/frame.h>
#include <machine/fpu.h>
#include <machine/intr.h>
#include <machine/md_var.h>
#include <machine/param.h>
#include <machine/segments.h>
#include <machine/vmparam.h>

#define RED_ZONE        128
#define NO_RETURN       UINT64_C(0xdead000000000000)    /* not canonical */

#define PSL_C           0x00000001
#define PSL_RESERVED1   0x00000002
#define PSL_T           0x00000100
#define PSL_D           0x00000400
/* What user mode may change: the arithmetic flags, TF, DF, OF, AC, ID. */
#define PSL_USER        0x00240dd5

/* The registers of the LWP's trap frame, as gregs. */
static void
fill_gregs(const struct lwp *l, greg_t *g)
{
        const struct trapframe *tf = l->l_tf;

        g[REG_RDI] = (greg_t)tf->tf_rdi;
        g[REG_RSI] = (greg_t)tf->tf_rsi;
        g[REG_RDX] = (greg_t)tf->tf_rdx;
        g[REG_RCX] = (greg_t)tf->tf_rcx;
        g[REG_R8] = (greg_t)tf->tf_r8;
        g[REG_R9] = (greg_t)tf->tf_r9;
        g[REG_RAX] = (greg_t)tf->tf_rax;
        g[REG_RBX] = (greg_t)tf->tf_rbx;
        g[REG_RBP] = (greg_t)tf->tf_rbp;
        g[REG_R10] = (greg_t)tf->tf_r10;
        g[REG_R11] = (greg_t)tf->tf_r11;
        g[REG_R12] = (greg_t)tf->tf_r12;
        g[REG_R13] = (greg_t)tf->tf_r13;
        g[REG_R14] = (greg_t)tf->tf_r14;
        g[REG_R15] = (greg_t)tf->tf_r15;
        g[REG_TRAPNO] = (greg_t)tf->tf_trapno;
        g[REG_ERR] = (greg_t)tf->tf_err;
        g[REG_RIP] = (greg_t)tf->tf_rip;
        g[REG_CS] = (greg_t)tf->tf_cs;
        g[REG_RFL] = (greg_t)tf->tf_rflags;
        g[REG_RSP] = (greg_t)tf->tf_rsp;
        g[REG_SS] = (greg_t)tf->tf_ss;
        g[REG_FSBASE] = (greg_t)l->l_fsbase;
}

/* The LWP's live floating-point state, into fpregs. */
static void
fill_fpregs(struct lwp *l, fpregset_t *fp)
{
        int s = splhi();

        fpu_save(l->l_fpu);
        memcpy(fp->fp_state, l->l_fpu, fpu_area_size());
        splx(s);
}

void
md_getcontext(struct lwp *l, ucontext_t *uc)
{
        fill_gregs(l, uc->uc_mcontext.gregs);
        fill_fpregs(l, &uc->uc_mcontext.fpregs);
        /* Resumed, the context returns 0 from getcontext. */
        uc->uc_mcontext.gregs[REG_RAX] = 0;
        uc->uc_mcontext.gregs[REG_RFL] &= ~(greg_t)PSL_C;
}

/* Whether a context is one user mode may be given. */
int
md_context_ok(const ucontext_t *uc)
{
        const greg_t *g = uc->uc_mcontext.gregs;

        if ((uc->uc_flags & UC_CPU) != 0 &&
            ((uint64_t)g[REG_RIP] >= VM_USER_MAX ||
            (uint64_t)g[REG_FSBASE] >= VM_USER_MAX))
                return EINVAL;
        return 0;
}

/* Load gregs into a trap frame, keeping to what user mode may hold. */
static void
load_gregs(struct trapframe *tf, const greg_t *g)
{
        tf->tf_rdi = (uint64_t)g[REG_RDI];
        tf->tf_rsi = (uint64_t)g[REG_RSI];
        tf->tf_rdx = (uint64_t)g[REG_RDX];
        tf->tf_rcx = (uint64_t)g[REG_RCX];
        tf->tf_r8 = (uint64_t)g[REG_R8];
        tf->tf_r9 = (uint64_t)g[REG_R9];
        tf->tf_rax = (uint64_t)g[REG_RAX];
        tf->tf_rbx = (uint64_t)g[REG_RBX];
        tf->tf_rbp = (uint64_t)g[REG_RBP];
        tf->tf_r10 = (uint64_t)g[REG_R10];
        tf->tf_r11 = (uint64_t)g[REG_R11];
        tf->tf_r12 = (uint64_t)g[REG_R12];
        tf->tf_r13 = (uint64_t)g[REG_R13];
        tf->tf_r14 = (uint64_t)g[REG_R14];
        tf->tf_r15 = (uint64_t)g[REG_R15];
        tf->tf_rip = (uint64_t)g[REG_RIP];
        tf->tf_rsp = (uint64_t)g[REG_RSP];
        tf->tf_rflags = ((uint64_t)g[REG_RFL] & PSL_USER) | PSL_I |
            PSL_RESERVED1;
        tf->tf_cs = UCSEL;
        tf->tf_ss = UDSEL;
}

/* A user FPU image, cleaned, into area. */
static void
load_fpregs(void *area, const fpregset_t *fp)
{
        char *img = fpu_area_alloc();

        memcpy(img, fp->fp_state, fpu_area_size());
        fpu_sanitize(img);
        memcpy(area, img, fpu_area_size());
        fpu_area_free(img);
}

/* setcontext for the calling LWP: its live registers change. */
int
md_setcontext(struct lwp *l, const ucontext_t *uc)
{
        const greg_t *g = uc->uc_mcontext.gregs;
        int s, error;

        if ((error = md_context_ok(uc)) != 0)
                return error;
        /*
         * The image is cleaned in a buffer first: while this LWP can be
         * switched out, l_fpu is where its live state is saved, so the
         * new state goes in only with interrupts off, together with
         * loading it.
         */
        if ((uc->uc_flags & UC_FP) != 0) {
                char *img = fpu_area_alloc();

                load_fpregs(img, &uc->uc_mcontext.fpregs);
                s = splhi();
                memcpy(l->l_fpu, img, fpu_area_size());
                fpu_restore(l->l_fpu);
                splx(s);
                fpu_area_free(img);
        }
        if ((uc->uc_flags & UC_CPU) != 0) {
                load_gregs(l->l_tf, g);
                (void)md_set_private(l, (uint64_t)g[REG_FSBASE]);
        }
        return 0;
}

/*
 * The starting context of a new LWP that has not run yet (_lwp_create);
 * md_context_ok() has passed it. Without UC_FP it starts with clean
 * floating-point state.
 */
void
md_lwp_setcontext(struct lwp *l, const ucontext_t *uc)
{
        if ((uc->uc_flags & UC_FP) != 0)
                load_fpregs(l->l_fpu, &uc->uc_mcontext.fpregs);
        if ((uc->uc_flags & UC_CPU) != 0) {
                load_gregs(l->l_tf, uc->uc_mcontext.gregs);
                l->l_fsbase = (uint64_t)uc->uc_mcontext.gregs[REG_FSBASE];
        }
}

int
md_sendsig(struct lwp *l, int sig, const siginfo_t *info, uint64_t handler,
    uint64_t restore, int onstack, int insig, uint64_t retaddr)
{
        struct trapframe *tf = l->l_tf;
        ucontext_t *uc = kmem_zalloc(sizeof(*uc), KM_SLEEP);
        uint64_t sp, ucp, sip = 0;
        int s, error;

        fill_gregs(l, uc->uc_mcontext.gregs);
        fill_fpregs(l, &uc->uc_mcontext.fpregs);
        uc->uc_flags = UC_ALL;
        uc->uc_sigmask.__sigbits[0] = (uint32_t)restore;
        uc->uc_sigmask.__sigbits[1] = (uint32_t)(restore >> 32);
        uc->uc_stack.ss_sp = (void *)(uintptr_t)l->l_altsp;
        uc->uc_stack.ss_size = l->l_altsize;
        uc->uc_stack.ss_flags = insig ? SS_ONSTACK :
            (l->l_altflags & SS_DISABLE);

        sp = onstack ? l->l_altsp + l->l_altsize : tf->tf_rsp - RED_ZONE;
        sp = (sp - sizeof(*uc)) & ~(uint64_t)15;
        ucp = sp;
        if (info != NULL) {
                sp = (sp - sizeof(*info)) & ~(uint64_t)15;
                sip = sp;
        }
        sp -= 8;
        if (retaddr == 0)
                retaddr = NO_RETURN;
        error = 0;
        if (sp >= VM_USER_MAX || sp < VM_USER_MIN ||
            (onstack && sp < l->l_altsp))
                error = EFAULT;
        if (error == 0)
                error = copyout(uc, (void *)(uintptr_t)ucp, sizeof(*uc));
        if (error == 0 && info != NULL)
                error = copyout(info, (void *)(uintptr_t)sip, sizeof(*info));
        if (error == 0)
                error = copyout(&retaddr, (void *)(uintptr_t)sp,
                    sizeof(retaddr));
        kmem_free(uc, sizeof(*uc));
        if (error != 0)
                return error;

        tf->tf_rip = handler;
        tf->tf_rsp = sp;
        tf->tf_rdi = (uint64_t)sig;
        tf->tf_rsi = sip;
        tf->tf_rdx = ucp;
        tf->tf_rbx = ucp;
        tf->tf_rax = 0;
        tf->tf_rflags = (tf->tf_rflags & ~(uint64_t)(PSL_T | PSL_D)) | PSL_I;
        tf->tf_cs = UCSEL;
        tf->tf_ss = UDSEL;
        s = splhi();
        fpu_area_reset(l->l_fpu);
        fpu_restore(l->l_fpu);
        splx(s);
        return 0;
}
