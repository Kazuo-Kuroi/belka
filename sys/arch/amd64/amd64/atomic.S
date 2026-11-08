/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 *
 * Out-of-line atomic operations and barriers. See <machine/atomic.h>
 * for the interface. These are linked into every kernel, and are used
 * whenever the compiler cannot provide the inline versions.
 *
 * Arguments arrive in %rdi, %rsi, and %rdx; results return in %rax.
 */

#include <machine/asm.h>

ENTRY(atomic_cas_32)
        movl    %esi, %eax
        lock
        cmpxchgl %edx, (%rdi)
        ret
END(atomic_cas_32)

ENTRY(atomic_cas_64)
        movq    %rsi, %rax
        lock
        cmpxchgq %rdx, (%rdi)
        ret
END(atomic_cas_64)

ENTRY(atomic_swap_32)
        movl    %esi, %eax
        xchgl   %eax, (%rdi)
        ret
END(atomic_swap_32)

ENTRY(atomic_swap_64)
        movq    %rsi, %rax
        xchgq   %rax, (%rdi)
        ret
END(atomic_swap_64)

ENTRY(atomic_add_32_nv)
        movl    %esi, %eax
        lock
        xaddl   %eax, (%rdi)
        addl    %esi, %eax
        ret
END(atomic_add_32_nv)

ENTRY(atomic_add_64_nv)
        movq    %rsi, %rax
        lock
        xaddq   %rax, (%rdi)
        addq    %rsi, %rax
        ret
END(atomic_add_64_nv)

ENTRY(atomic_or_32)
        lock
        orl     %esi, (%rdi)
        ret
END(atomic_or_32)

ENTRY(atomic_and_32)
        lock
        andl    %esi, (%rdi)
        ret
END(atomic_and_32)

ENTRY(atomic_or_64)
        lock
        orq     %rsi, (%rdi)
        ret
END(atomic_or_64)

ENTRY(atomic_and_64)
        lock
        andq    %rsi, (%rdi)
        ret
END(atomic_and_64)

ENTRY(membar_sync)
        lock
        addq    $0, (%rsp)
        ret
END(membar_sync)

/*
 * The remaining barriers need no instruction on amd64. The call itself
 * stops the compiler from reordering memory accesses around it.
 */
ENTRY(membar_enter)
        ret
END(membar_enter)

ENTRY(membar_exit)
        ret
END(membar_exit)

ENTRY(membar_producer)
        ret
END(membar_producer)

ENTRY(membar_consumer)
        ret
END(membar_consumer)

ENTRY(insn_barrier_func)
        ret
END(insn_barrier_func)
