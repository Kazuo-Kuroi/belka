/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 *
 * Out-of-line versions of the routines in <machine/cpufunc.h>.
 */

#include <machine/asm.h>

/* void outb(uint16_t port, uint8_t value) */
ENTRY(outb)
        movl    %edi, %edx
        movl    %esi, %eax
        outb    %al, %dx
        ret
END(outb)

/* uint8_t inb(uint16_t port) */
ENTRY(inb)
        movl    %edi, %edx
        xorl    %eax, %eax
        inb     %dx, %al
        ret
END(inb)

ENTRY(disable_intr)
        cli
        ret
END(disable_intr)

ENTRY(enable_intr)
        sti
        ret
END(enable_intr)

ENTRY(cpu_halt)
        hlt
        ret
END(cpu_halt)

ENTRY(cpu_spinwait)
        pause
        ret
END(cpu_spinwait)

ENTRY(read_rflags)
        pushfq
        popq    %rax
        ret
END(read_rflags)
