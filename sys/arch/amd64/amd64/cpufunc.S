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

/*
 * void lgdt_load(uint64_t base, uint16_t limit, uint16_t csel,
 *     uint16_t dsel)
 *
 * Load the GDT, then reload every segment register from it. CS can
 * only be changed by a far transfer, so return through lretq with the
 * new selector. FS and GS get the null selector; their bases are set
 * through MSRs afterwards.
 */
ENTRY(lgdt_load)
        subq    $16, %rsp
        movw    %si, 6(%rsp)
        movq    %rdi, 8(%rsp)
        lgdt    6(%rsp)
        addq    $16, %rsp
        mov     %cx, %ds
        mov     %cx, %es
        mov     %cx, %ss
        xorl    %eax, %eax
        mov     %ax, %fs
        mov     %ax, %gs
        popq    %rax
        movzwq  %dx, %rdx
        pushq   %rdx
        pushq   %rax
        lretq
END(lgdt_load)

/* void lidt_load(uint64_t base, uint16_t limit) */
ENTRY(lidt_load)
        subq    $16, %rsp
        movw    %si, 6(%rsp)
        movq    %rdi, 8(%rsp)
        lidt    6(%rsp)
        addq    $16, %rsp
        ret
END(lidt_load)

/* void ltr(uint16_t sel) */
ENTRY(ltr)
        ltr     %di
        ret
END(ltr)

/* uint64_t rdmsr(uint32_t msr) */
ENTRY(rdmsr)
        movl    %edi, %ecx
        rdmsr
        shlq    $32, %rdx
        orq     %rdx, %rax
        ret
END(rdmsr)

/* void wrmsr(uint32_t msr, uint64_t value) */
ENTRY(wrmsr)
        movl    %edi, %ecx
        movl    %esi, %eax
        movq    %rsi, %rdx
        shrq    $32, %rdx
        wrmsr
        ret
END(wrmsr)

ENTRY(rcr0)
        movq    %cr0, %rax
        ret
END(rcr0)

ENTRY(rcr2)
        movq    %cr2, %rax
        ret
END(rcr2)

ENTRY(rcr3)
        movq    %cr3, %rax
        ret
END(rcr3)

ENTRY(rcr4)
        movq    %cr4, %rax
        ret
END(rcr4)

/* Raise a breakpoint exception; used by the trap self-tests. */
ENTRY(breakpoint)
        int3
        ret
END(breakpoint)

/*
 * Raise an invalid-opcode exception. A handler that expects it resumes
 * two bytes later, at the ret.
 */
ENTRY(cpu_ud2)
        ud2
        ret
END(cpu_ud2)

/* struct cpu_info *curcpu(void), for compilers without inline asm. */
ENTRY(curcpu)
        movq    %gs:0, %rax
        ret
END(curcpu)

/* void lcr3(uint64_t) */
ENTRY(lcr3)
        movq    %rdi, %cr3
        ret
END(lcr3)

/* void lcr4(uint64_t) */
ENTRY(lcr4)
        movq    %rdi, %cr4
        ret
END(lcr4)

/* void invlpg(uint64_t va) */
ENTRY(invlpg)
        invlpg  (%rdi)
        ret
END(invlpg)

/* void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) */
ENTRY(cpuid)
        pushq   %rbx
        movq    %rdx, %r8
        movl    %edi, %eax
        movl    %esi, %ecx
        cpuid
        movl    %eax, 0(%r8)
        movl    %ebx, 4(%r8)
        movl    %ecx, 8(%r8)
        movl    %edx, 12(%r8)
        popq    %rbx
        ret
END(cpuid)

/* uint64_t cpu_read_probe(const volatile uint64_t *p) */
ENTRY(cpu_read_probe)
        .globl  cpu_read_insn
        .globl  cpu_read_insn_end
cpu_read_insn:
        movq    (%rdi), %rax
cpu_read_insn_end:
        ret
END(cpu_read_probe)

/* void cpu_write_probe(volatile uint64_t *p, uint64_t value) */
ENTRY(cpu_write_probe)
        .globl  cpu_write_insn
        .globl  cpu_write_insn_end
cpu_write_insn:
        movq    %rsi, (%rdi)
cpu_write_insn_end:
        ret
END(cpu_write_probe)

/*
 * void cpu_call_probe(const void *addr)
 *
 * Call addr. A test expecting an instruction-fetch fault there resumes
 * by returning from the call.
 */
ENTRY(cpu_call_probe)
        call    *%rdi
        ret
END(cpu_call_probe)
