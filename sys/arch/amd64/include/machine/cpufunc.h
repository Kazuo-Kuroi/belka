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
outw(uint16_t port, uint16_t value)
{
        __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t
inw(uint16_t port)
{
        uint16_t value;

        __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
        return value;
}

static inline void
outl(uint16_t port, uint32_t value)
{
        __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t
inl(uint16_t port)
{
        uint32_t value;

        __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port));
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
rdtsc(void)
{
        uint32_t lo, hi;

        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
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
void outw(uint16_t, uint16_t);
uint16_t inw(uint16_t);
void outl(uint16_t, uint32_t);
uint32_t inl(uint16_t);
void disable_intr(void);
void enable_intr(void);
void cpu_halt(void);
void cpu_spinwait(void);
uint64_t rdtsc(void);
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
void lcr3(uint64_t);
void lcr4(uint64_t);
void invlpg(uint64_t);
void cpuid(uint32_t, uint32_t, uint32_t *);
int cpu_rdrand(uint64_t *);
int cpu_rdseed(uint64_t *);
void breakpoint(void);
void cpu_ud2(void);

/*
 * Enable interrupts and halt until one arrives, with no window between
 * the two in which an interrupt could be missed. Returns with
 * interrupts enabled.
 */
void cpu_wait_for_interrupt(void);

/*
 * Probes for the fault self-tests. Each performs one memory access with
 * an instruction that runs from cpu_*_insn to cpu_*_insn_end, so a test
 * expecting a fault knows how far to step over it.
 */
uint64_t cpu_read_probe(const volatile uint64_t *);
void cpu_write_probe(volatile uint64_t *, uint64_t);
void cpu_call_probe(const void *);
extern const char cpu_read_insn[], cpu_read_insn_end[];
extern const char cpu_write_insn[], cpu_write_insn_end[];

#define PSL_I           0x00000200      /* RFLAGS interrupt enable */

#define CR4_PGE         0x00000080      /* global pages */

/* CPUID leaf 0x80000001, EDX. */
#define CPUID_EXT_NX    0x00100000      /* no-execute */
#define CPUID_EXT_1GB   0x04000000      /* 1 GB pages */

/* Model-specific registers. */
#define MSR_PAT         0x00000277
#define MSR_EFER        0xc0000080
#define EFER_NXE        0x00000800      /* no-execute enable */
#define MSR_FSBASE      0xc0000100
#define MSR_GSBASE      0xc0000101
#define MSR_KERNELGSBASE 0xc0000102

#endif /* _MACHINE_CPUFUNC_H_ */
