/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 *
 * Floating-point state instructions, always out of line. See
 * <machine/fpu.h>.
 */

#include <machine/asm.h>

/* void fpu_fxsave(void *area) */
ENTRY(fpu_fxsave)
        fxsave64 (%rdi)
        ret
END(fpu_fxsave)

/* void fpu_fxrstor(void *area) */
ENTRY(fpu_fxrstor)
        fxrstor64 (%rdi)
        ret
END(fpu_fxrstor)

/* void fpu_xsave(void *area, uint64_t mask) */
ENTRY(fpu_xsave)
        movl    %esi, %eax
        movq    %rsi, %rdx
        shrq    $32, %rdx
        xsave64 (%rdi)
        ret
END(fpu_xsave)

/* void fpu_xrstor(void *area, uint64_t mask) */
ENTRY(fpu_xrstor)
        movl    %esi, %eax
        movq    %rsi, %rdx
        shrq    $32, %rdx
        xrstor64 (%rdi)
        ret
END(fpu_xrstor)

/* void fpu_xsetbv(uint32_t reg, uint64_t value) */
ENTRY(fpu_xsetbv)
        movl    %edi, %ecx
        movl    %esi, %eax
        movq    %rsi, %rdx
        shrq    $32, %rdx
        xsetbv
        ret
END(fpu_xsetbv)

/* void fpu_fninit(void) */
ENTRY(fpu_fninit)
        fninit
        ret
END(fpu_fninit)

/* void fpu_ldmxcsr(const uint32_t *mxcsr) */
ENTRY(fpu_ldmxcsr)
        ldmxcsr (%rdi)
        ret
END(fpu_ldmxcsr)

/* void lcr0(uint64_t value) */
ENTRY(lcr0)
        movq    %rdi, %cr0
        ret
END(lcr0)
