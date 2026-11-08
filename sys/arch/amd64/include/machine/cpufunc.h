/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Access to amd64 instructions that C cannot express: port I/O,
 * interrupt control, and processor hints. Each routine exists out of
 * line in cpufunc.S. When the compiler supports inline assembly, the
 * inline versions below are used instead.
 */

#ifndef _MACHINE_CPUFUNC_H_
#define _MACHINE_CPUFUNC_H_

#include <stdint.h>

#include <sys/compiler.h>

#ifdef HAVE_INLINE_ASM

static inline void
outb(uint16_t port, uint8_t value)
{
        __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
        uint8_t value;

        __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
        return value;
}

static inline void
disable_intr(void)
{
        __asm__ __volatile__("cli" : : : "memory");
}

static inline void
enable_intr(void)
{
        __asm__ __volatile__("sti" : : : "memory");
}

static inline void
cpu_halt(void)
{
        __asm__ __volatile__("hlt" : : : "memory");
}

/* Hint that the processor is in a spin-wait loop. */
static inline void
cpu_spinwait(void)
{
        __asm__ __volatile__("pause" : : : "memory");
}

static inline uint64_t
read_rflags(void)
{
        uint64_t rflags;

        __asm__ __volatile__("pushfq; popq %0" : "=r"(rflags));
        return rflags;
}

#else /* !HAVE_INLINE_ASM */

void outb(uint16_t, uint8_t);
uint8_t inb(uint16_t);
void disable_intr(void);
void enable_intr(void);
void cpu_halt(void);
void cpu_spinwait(void);
uint64_t read_rflags(void);

#endif /* HAVE_INLINE_ASM */

/*
 * Routines that are only used during processor setup, on fault paths,
 * or in tests. They are always out of line.
 */
void lgdt_load(uint64_t, uint16_t, uint16_t, uint16_t);
void lidt_load(uint64_t, uint16_t);
void ltr(uint16_t);
uint64_t rdmsr(uint32_t);
void wrmsr(uint32_t, uint64_t);
uint64_t rcr0(void);
uint64_t rcr2(void);
uint64_t rcr3(void);
uint64_t rcr4(void);
void breakpoint(void);
void cpu_ud2(void);

#define PSL_I           0x00000200      /* RFLAGS interrupt enable */

/* Model-specific registers. */
#define MSR_FSBASE      0xc0000100
#define MSR_GSBASE      0xc0000101
#define MSR_KERNELGSBASE 0xc0000102

#endif /* _MACHINE_CPUFUNC_H_ */
